//go:build windows

package utils

import (
	"context"
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/sys/windows"
)

func setCreationTime(t *testing.T, path string) {
	t.Helper()
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	created := windows.NsecToFiletime(1700000000000000000)
	if err := windows.SetFileTime(windows.Handle(f.Fd()), &created, nil, nil); err != nil {
		t.Fatal(err)
	}
}

func TestFileIdentity(t *testing.T) {
	d := t.TempDir()
	p := filepath.Join(d, "file")
	if err := os.WriteFile(p, []byte("first"), 0o600); err != nil {
		t.Fatal(err)
	}
	identity := func(path string, follow bool) [2]uint64 {
		t.Helper()
		stat := os.Lstat
		if follow {
			stat = os.Stat
		}
		s, err := stat(path)
		if err != nil {
			t.Fatal(err)
		}
		dev, ino, err := FileIdentity(s, path)
		if err != nil {
			t.Fatal(err)
		}
		return [2]uint64{dev, ino}
	}
	want := identity(p, false)
	if err := os.WriteFile(p, []byte("different size"), 0o600); err != nil {
		t.Fatal(err)
	}
	setCreationTime(t, p)
	if got := identity(p, false); got != want {
		t.Fatalf("identity changed with size or creation time: %v != %v", got, want)
	}
	for _, name := range []string{"hardlink", "symlink", "dangling"} {
		q := filepath.Join(d, name)
		var err error
		switch name {
		case "hardlink":
			err = os.Link(p, q)
		case "symlink":
			err = os.Symlink(p, q)
		case "dangling":
			err = os.Symlink(filepath.Join(d, "absent"), q)
		}
		if err != nil {
			t.Fatal(err)
		}
		got := identity(q, false)
		if (got == want) != (name == "hardlink") {
			t.Errorf("unexpected identity for %s: %v (target %v)", name, got, want)
		}
		if name != "dangling" && identity(q, true) != want {
			t.Errorf("stat must identify the target for %s", name)
		}
	}
	if identity(d, false) == want {
		t.Error("directory and file have the same identity")
	}
	t.Run("PythonStat", func(t *testing.T) {
		python, err := exec.LookPath("python")
		if err != nil {
			t.Skip("Python not available for os.stat compatibility check")
		}
		out, err := exec.Command(python, "-c", "import json,os,sys; s=os.stat(sys.argv[1]); print(json.dumps([s.st_dev,s.st_ino]))", p).CombinedOutput()
		if err != nil {
			t.Fatalf("Python os.stat: %v: %s", err, out)
		}
		var got [2]uint64
		if err := json.Unmarshal(out, &got); err != nil {
			t.Fatal(err)
		}
		if got != want {
			t.Errorf("Python os.stat identity %v != Go identity %v", got, want)
		}
	})
	t.Run("LongUnicodePath", func(t *testing.T) {
		q := filepath.Join(d, strings.Repeat("long-directory-", 8), strings.Repeat("long-directory-", 8), "文件.txt")
		if err := os.MkdirAll(filepath.Dir(q), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.Link(p, q); err != nil {
			t.Fatal(err)
		}
		if got := identity(q, false); got != want {
			t.Errorf("long path identity %v != %v", got, want)
		}
	})
	t.Run("MissingPath", func(t *testing.T) {
		s, err := os.Stat(p)
		if err != nil {
			t.Fatal(err)
		}
		if _, _, err := FileIdentity(s, filepath.Join(d, "absent")); !errors.Is(err, fs.ErrNotExist) {
			t.Errorf("expected a missing-path error, got %v", err)
		}
	})
}

func TestCopyFolderFileIdentity(t *testing.T) {
	src, dest := t.TempDir(), t.TempDir()
	for name, data := range map[string]string{"a": "first", "b": "other"} {
		p := filepath.Join(src, name)
		if err := os.WriteFile(p, []byte(data), 0o600); err != nil {
			t.Fatal(err)
		}
		setCreationTime(t, p)
	}
	if err := os.Symlink("a", filepath.Join(src, "z-link")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(".", filepath.Join(src, "z-loop")); err != nil {
		t.Fatal(err)
	}
	sf, err := OpenDir(src)
	if err != nil {
		t.Fatal(err)
	}
	defer sf.Close()
	df, err := OpenDir(dest)
	if err != nil {
		t.Fatal(err)
	}
	defer df.Close()
	if err := CopyFolderContents(context.Background(), sf, df, CopyFolderOptions{Follow_symlinks: true, Disallow_hardlinks: true}); err != nil {
		t.Fatal(err)
	}
	for name, want := range map[string]string{"a": "first", "b": "other", "z-link": "first"} {
		got, err := os.ReadFile(filepath.Join(dest, name))
		if err != nil || string(got) != want {
			t.Errorf("%s: got %q, %v; want %q", name, got, err, want)
		}
	}
	if target, err := os.Readlink(filepath.Join(dest, "z-loop")); err != nil || target != "." {
		t.Errorf("symlink loop: target %q, error %v", target, err)
	}
}

//go:build windows

package ssh

import (
	"archive/tar"
	"os"
	"path/filepath"
	"testing"

	"golang.org/x/sys/windows"
)

func TestCopiedFileIdentity(t *testing.T) {
	d := t.TempDir()
	contents := map[string]string{"a": "first", "b": "other", "empty-a": "", "empty-b": ""}
	created := windows.NsecToFiletime(1700000000000000000)
	for name, data := range contents {
		p := filepath.Join(d, name)
		if err := os.WriteFile(p, []byte(data), 0o600); err != nil {
			t.Fatal(err)
		}
		f, err := os.OpenFile(p, os.O_RDWR, 0)
		if err != nil {
			t.Fatal(err)
		}
		err = windows.SetFileTime(windows.Handle(f.Fd()), &created, nil, nil)
		f.Close()
		if err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Link(filepath.Join(d, "a"), filepath.Join(d, "hardlink")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("a", filepath.Join(d, "symlink")); err != nil {
		t.Fatal(err)
	}
	seen := make(map[string]bool)
	err := get_file_data(func(h *tar.Header, data []byte) error {
		name := filepath.Base(h.Name)
		seen[name] = true
		if want, ok := contents[name]; ok {
			if h.Typeflag != tar.TypeReg || string(data) != want {
				t.Errorf("%s: expected regular file with %q, got type %c, data %q", name, want, h.Typeflag, data)
			}
		} else if name == "hardlink" {
			if h.Typeflag != tar.TypeLink || h.Linkname != "copy/a" {
				t.Errorf("hard link not preserved: %+v", h)
			}
		} else if name == "symlink" {
			if h.Typeflag != tar.TypeSymlink || h.Linkname != "a" {
				t.Errorf("symbolic link not preserved: %+v", h)
			}
		}
		return nil
	}, map[file_unique_id]string{}, d, "copy", nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"a", "b", "empty-a", "empty-b", "hardlink", "symlink"} {
		if !seen[name] {
			t.Errorf("missing archive entry: %s", name)
		}
	}
}

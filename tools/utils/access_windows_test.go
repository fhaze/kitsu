//go:build windows

package utils

import (
	"io/fs"
	"os"
	"path/filepath"
	"testing"
)

func TestPosixPermissions(t *testing.T) {
	tdir := t.TempDir()
	expected := map[string]fs.FileMode{}
	create := func(name string, data string, read_only bool, perm fs.FileMode) {
		p := filepath.Join(tdir, name)
		if err := os.WriteFile(p, []byte(data), 0o644); err != nil {
			t.Fatal(err)
		}
		if read_only {
			if err := os.Chmod(p, 0o444); err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = os.Chmod(p, 0o644) })
		}
		expected[name] = perm
	}
	create("plain.txt", "text", false, 0o644)
	create("empty", "", false, 0o644)
	create("script", "#!/bin/sh\n", false, 0o755)
	create("elf", "\x7fELF\x02\x01", false, 0o755)
	create("tool.EXE", "MZ", false, 0o755)
	create("tool.cmd", "@echo off", false, 0o755)
	create("tool.Bat", "", false, 0o755)
	create("prog.com", "", false, 0o755)
	create("tool.js", "", false, 0o644) // in PATHEXT, but not an executable on POSIX
	create("read-only.txt", "", true, 0o444)
	create("read-only-script", "#!/bin/sh\n", true, 0o555)
	// Windows ignores the read-only attribute of directories, Explorer sets it on customized folders
	for _, name := range []string{"dir", "read-only-dir"} {
		p := filepath.Join(tdir, name)
		if err := os.Mkdir(p, 0o755); err != nil {
			t.Fatal(err)
		}
		expected[name] = 0o755
	}
	if err := os.Chmod(filepath.Join(tdir, "read-only-dir"), 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(filepath.Join(tdir, "read-only-dir"), 0o755) })
	for name, perm := range expected {
		p := filepath.Join(tdir, name)
		s, err := os.Lstat(p)
		if err != nil {
			t.Fatal(err)
		}
		if actual := PosixPermissions(p, s); actual != perm {
			t.Errorf("%s: %o != %o", name, actual, perm)
		}
	}
}

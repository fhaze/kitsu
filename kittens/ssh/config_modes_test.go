// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package ssh

import (
	"archive/tar"
	"io/fs"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/go-cmp/cmp"
)

// Windows has no mode bits, files copied to the remote must get the modes
// they would have on a POSIX system rather than 0o666 and 0o777 for all
func TestCopiedFileModes(t *testing.T) {
	d := filepath.Join(t.TempDir(), "d")
	for _, x := range []string{d, filepath.Join(d, "sub")} {
		if err := os.Mkdir(x, 0o755); err != nil {
			t.Fatal(err)
		}
		_ = os.Chmod(x, 0o755)
	}
	// distinct sizes, as Windows has no inode numbers to tell hard links apart
	create := func(name, data string, perm fs.FileMode) {
		p := filepath.Join(d, name)
		if err := os.WriteFile(p, []byte(data), perm); err != nil {
			t.Fatal(err)
		}
		if err := os.Chmod(p, perm); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = os.Chmod(p, 0o644) })
	}
	create("plain.txt", "x", 0o644)
	create("script", "#!/bin/sh\n", 0o755)
	create("read-only", "xx", 0o444)
	create(filepath.Join("sub", "conf"), "xxx", 0o644)
	actual := map[string]int64{}
	err := get_file_data(func(h *tar.Header, data []byte) error {
		actual[h.Name] = h.Mode
		return nil
	}, map[file_unique_id]string{}, d, "home/d", nil)
	if err != nil {
		t.Fatal(err)
	}
	expected := map[string]int64{
		"home/d/": 0o755, "home/d/sub/": 0o755, "home/d/plain.txt": 0o644, "home/d/script": 0o755, "home/d/read-only": 0o444, "home/d/sub/conf": 0o644,
	}
	if diff := cmp.Diff(expected, actual); diff != "" {
		t.Fatalf("Unexpected modes for copied files:\n%s", diff)
	}
}

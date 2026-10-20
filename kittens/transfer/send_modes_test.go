// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package transfer

import (
	"io/fs"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/go-cmp/cmp"
)

// Windows has no mode bits, sent files must get the permissions they would
// have on a POSIX system rather than 0o666 and 0o777 for all
func TestSendPermissions(t *testing.T) {
	d := filepath.Join(t.TempDir(), "d")
	if err := os.Mkdir(d, 0o755); err != nil {
		t.Fatal(err)
	}
	_ = os.Chmod(d, 0o755)
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
	counter := 0
	files, err := process(&Options{}, []string{d}, "/dest/", &counter)
	if err != nil {
		t.Fatal(err)
	}
	actual := map[string]fs.FileMode{}
	for _, f := range files {
		actual[filepath.Base(f.expanded_local_path)] = f.permissions
	}
	expected := map[string]fs.FileMode{"d": 0o755, "plain.txt": 0o644, "script": 0o755, "read-only": 0o444}
	if diff := cmp.Diff(expected, actual); diff != "" {
		t.Fatalf("Unexpected permissions for sent files:\n%s", diff)
	}
}

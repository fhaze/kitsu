// License: GPLv3 Copyright: 2023, Kovid Goyal, <kovid at kovidgoyal.net>

package shell_integration

import (
	"archive/tar"
	"bytes"
	"fmt"
	"github.com/kovidgoyal/kitty"
	"os"
	"path/filepath"
	"testing"
)

var _ = fmt.Print

func TestExtractShellIntegration(t *testing.T) {
	tdir := t.TempDir()
	if err := extract_shell_integration_for("zsh", tdir); err != nil {
		t.Fatal(err)
	}
	kzsh := filepath.Join(tdir, "shell-integration", "zsh", "kitty.zsh")
	if _, err := os.Stat(kzsh); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(tdir, "shell-integration", "zsh", "completions", "_kitty")); err != nil {
		t.Fatal(err)
	}
	orig, err := os.ReadFile(kzsh)
	if err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(kzsh, []byte("changed"), 0o644)
	if err := extract_shell_integration_for("zsh", tdir); err != nil {
		t.Fatal(err)
	}
	changed, err := os.ReadFile(kzsh)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(changed, orig) {
		t.Fatalf("Failed to update shell integration file")
	}

	if err = extract_terminfo(tdir); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(tdir, "terminfo", "78", kitty.DefaultTermName)); err != nil {
		t.Fatal(err)
	}
	TerminfoData()
}

// The modes of the files the ssh kitten sends to remote hosts must not depend
// on the OS kitty was built on. Windows reports 0o666 for every file, which
// made the sourced scripts world writable and the kitty and kitten wrappers
// not executable on the remote.
func TestEmbeddedFileModes(t *testing.T) {
	executables := map[string]bool{"shell-integration/ssh/kitty": true, "shell-integration/ssh/kitten": true}
	found := 0
	for name, entry := range Data() {
		expected := int64(0o644)
		switch {
		case entry.Metadata.Typeflag == tar.TypeDir:
			expected = 0o755
		case executables[name]:
			expected = 0o755
			found++
		}
		if entry.Metadata.Mode != expected {
			t.Errorf("%s has mode %o instead of %o", name, entry.Metadata.Mode, expected)
		}
	}
	if found != len(executables) {
		t.Fatalf("Only %d of the executables %v are embedded", found, executables)
	}
}

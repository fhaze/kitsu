// License: GPLv3 Copyright: 2026, Kitsu contributors

package tui

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestResolveWindowsShell(t *testing.T) {
	cmd := filepath.Join(os.Getenv("SystemRoot"), "System32", "cmd.exe")
	data, err := os.ReadFile(cmd)
	if err != nil {
		t.Fatal(err)
	}
	cmd = filepath.Join(t.TempDir(), "shell with spaces.exe")
	if err := os.WriteFile(cmd, data, 0700); err != nil {
		t.Fatal(err)
	}
	for _, text := range []string{`"` + cmd + `" /d /c "exit 17"`, `'` + cmd + `' /d /c 'exit 17'`} {
		got := ResolveShell(text)
		if diff := cmp.Diff([]string{cmd, "/d", "/c", "exit 17"}, got); diff != "" {
			t.Fatalf("ResolveShell(%q): %s", text, diff)
		}
		err := exec.Command(got[0], got[1:]...).Run()
		if exit, ok := err.(*exec.ExitError); !ok || exit.ExitCode() != 17 {
			t.Fatalf("Resolved shell did not run: %v", err)
		}
	}
	t.Setenv("ComSpec", cmd)
	original_opts := relevant_kitty_opts
	relevant_kitty_opts = func() KittyOpts { return KittyOpts{Shell: "."} }
	t.Cleanup(func() { relevant_kitty_opts = original_opts })
	if diff := cmp.Diff([]string{cmd}, ResolveShell("")); diff != "" {
		t.Fatalf("Default shell fallback: %s", diff)
	}
	for _, text := range []string{"nonexistent-issue-65-shell.exe", "   ", `""`} {
		if diff := cmp.Diff([]string{cmd}, ResolveShell(text)); diff != "" {
			t.Fatalf("Fallback for %q: %s", text, diff)
		}
	}
	t.Setenv("ComSpec", "")
	if diff := cmp.Diff([]string{"cmd.exe"}, ResolveShell("nonexistent-issue-65-shell.exe")); diff != "" {
		t.Fatal(diff)
	}
}

// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build !windows

package utils

import (
	"os/exec"
)

// Command is exec.Command. On Windows it also runs batch files safely, see
// command_windows.go, so use it for every program the user chooses.
func Command(name string, arg ...string) *exec.Cmd {
	return exec.Command(name, arg...)
}

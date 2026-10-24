// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build !windows

package ssh

import (
	"errors"
	"io"

	"github.com/kovidgoyal/kitty/tools/tty"
)

// Only needed on Windows, see pseudoconsole_windows.go
type pseudoconsole struct{}

func open_pseudoconsole(*tty.Term) (*pseudoconsole, error) { return nil, errors.ErrUnsupported }

func (self *pseudoconsole) Close() {}

func (self *pseudoconsole) run(*tty.Term, []string, io.Writer) (int, error) {
	return 1, errors.ErrUnsupported
}

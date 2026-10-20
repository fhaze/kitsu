// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package utils

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"golang.org/x/sys/windows"
)

// The prefix of command lines for running batch files via cmd.exe, the same as
// KITTY_WIN32_BATCH_PREFIX in kitty/win32-compat.h: /d skips the AutoRun
// commands from the registry, /e:ON /v:OFF are needed for the escaping to work
// and /s strips the outer quotes regardless of what is inside them.
const batch_command_line_prefix = `cmd.exe /d /e:ON /v:OFF /s /c "`

// IsBatchFile reports whether CreateProcess() would run path as a batch file,
// via cmd.exe: its extension is .bat or .cmd, also with trailing dots or
// spaces, which Windows strips from file names unless the path is a \\?\ one.
func IsBatchFile(path string) bool {
	if !strings.HasPrefix(path, `\\?\`) {
		path = strings.TrimRight(path, ". ")
	}
	if len(path) < 4 {
		return false
	}
	ext := strings.ToLower(path[len(path)-4:])
	return ext == ".bat" || ext == ".cmd"
}

func is_safe_unquoted_batch_char(ch byte) bool {
	return ('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z') || ('0' <= ch && ch <= '9') || strings.IndexByte("#$*+-./:?@\\_", ch) > -1
}

// A port of kitty_win32_append_batch_arg() from kitty/win32-compat.c, see the
// comments there. cmd.exe parses the command line of batch files itself, so
// quote every argument with anything but known safe characters, double
// embedded quotes, which keeps cmd.exe inside the quoted region, and turn every
// % into the zero length substring %%cd:~,% so that no variable name can be
// formed. CR and LF end the command and cannot be escaped at all.
func append_batch_arg(buf *strings.Builder, arg string) error {
	if strings.ContainsAny(arg, "\r\n") {
		return fmt.Errorf("arguments containing CR or LF cannot be passed to batch files: %w", syscall.EINVAL)
	}
	// Quote empty arguments so they are not dropped and those ending with a
	// backslash so that it does not escape the closing quote of "%~1"
	needs_quotes := arg == "" || arg[len(arg)-1] == '\\'
	for i := 0; i < len(arg) && !needs_quotes; i++ {
		if ch := arg[i]; ch < 0x80 {
			needs_quotes = !is_safe_unquoted_batch_char(ch)
		} else if ch == 0xc2 && i+1 < len(arg) && arg[i+1] >= 0x80 && arg[i+1] < 0xa0 {
			needs_quotes = true // C1 control characters
		}
	}
	if buf.Len() > 0 {
		buf.WriteByte(' ')
	}
	if needs_quotes {
		buf.WriteByte('"')
	}
	backslashes := 0
	for i := 0; i < len(arg); i++ {
		ch := arg[i]
		if ch == '\\' {
			backslashes++
		} else {
			switch ch {
			case '"':
				// n backslashes before a quote become 2n, followed by ""
				buf.WriteString(strings.Repeat(`\`, backslashes))
				buf.WriteByte('"')
			case '%':
				buf.WriteString("%%cd:~,")
			}
			backslashes = 0
		}
		buf.WriteByte(ch)
	}
	if needs_quotes {
		buf.WriteString(strings.Repeat(`\`, backslashes))
		buf.WriteByte('"')
	}
	return nil
}

// BatchCommandLine returns the command line for running the batch file script
// with args via cmd.exe, escaped for it the same way kitty does for the
// programs it runs in its windows, see kitty/win32-pty.c
func BatchCommandLine(script string, args []string) (string, error) {
	var buf strings.Builder
	buf.WriteString(batch_command_line_prefix)
	if err := append_batch_arg(&buf, script); err != nil {
		return "", err
	}
	for _, arg := range args {
		if err := append_batch_arg(&buf, arg); err != nil {
			return "", err
		}
	}
	buf.WriteByte('"')
	return buf.String(), nil
}

func system_cmd_exe() (string, error) {
	dir, err := windows.GetSystemDirectory()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "cmd.exe"), nil
}

// Command is like exec.Command, except that batch files are run safely.
// Windows runs .bat and .cmd files via cmd.exe, which parses their command line
// itself, so the quoting exec uses does not stop arguments from running
// commands (CVE-2024-24576, aka BatBadBut). Run them via the system cmd.exe
// with a command line escaped for it instead. Use it for every program the
// user chooses, as name can resolve to a batch file. Do not replace
// cmd.SysProcAttr afterwards, use DetachCommand() to detach, and do not set
// cmd.Dir when name is a relative path, as it is resolved against our cwd.
func Command(name string, arg ...string) *exec.Cmd {
	cmd := exec.Command(name, arg...)
	if cmd.Err != nil {
		return cmd
	}
	path := cmd.Path
	if filepath.Base(name) != name {
		// For a name with a directory exec.Command() leaves adding a PATHEXT
		// extension to Start(), so C:\x\code can still end up running
		// C:\x\code.cmd
		if lp, err := exec.LookPath(name); err == nil {
			path = lp
		}
	}
	if !IsBatchFile(path) {
		return cmd
	}
	// cmd.exe resolves a relative script against the cwd of the child, not ours
	script, err := filepath.Abs(path)
	if err == nil {
		// cmd.exe would start and report the error itself, keep failing to start
		_, err = os.Stat(script)
	}
	if err == nil {
		var cmdline, cmd_exe string
		if cmdline, err = BatchCommandLine(script, arg); err == nil {
			if cmd_exe, err = system_cmd_exe(); err == nil {
				cmd.Path = cmd_exe
				cmd.SysProcAttr = &syscall.SysProcAttr{CmdLine: cmdline}
				return cmd
			}
		}
	}
	cmd.Err = err
	return cmd
}

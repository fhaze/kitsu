// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package ssh

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"unsafe"

	"golang.org/x/sys/windows"

	"github.com/kovidgoyal/kitty/tools/tty"
)

// Win32-OpenSSH reads the window size from its console and watches it for
// resizes. The kitten has to read ssh's output itself to pick out the
// @kitty-* DCS codes, so instead of a plain pipe, which has no size, ssh runs
// in a pseudoconsole of its own. The kitten relays input into it, passes its
// output through the DCS filter and resizes it along with the real console.
//
// The conpty.dll + OpenConsole.exe bundled with kitty relay DCS codes they do
// not understand unchanged, the in-box conhost behind kernel32's
// CreatePseudoConsole() does not, so only the bundled one is used.

type conpty_api struct {
	create, resize, close *windows.LazyProc
}

var load_conpty_api = sync.OnceValues(func() (*conpty_api, error) {
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	dir := filepath.Dir(exe)
	// kitten.exe is also installed in cmd\ which only holds launchers
	for _, candidate := range []string{filepath.Join(dir, "conpty.dll"), filepath.Join(filepath.Dir(dir), "bin", "conpty.dll")} {
		if _, err := os.Stat(candidate); err != nil {
			continue
		}
		dll := windows.NewLazyDLL(candidate)
		api := &conpty_api{dll.NewProc("ConptyCreatePseudoConsole"), dll.NewProc("ConptyResizePseudoConsole"), dll.NewProc("ConptyClosePseudoConsole")}
		if err = errors.Join(api.create.Find(), api.resize.Find(), api.close.Find()); err != nil {
			return nil, err
		}
		return api, nil
	}
	return nil, fmt.Errorf("conpty.dll not found next to %s", exe)
})

func coord(sz *tty.Winsize) uintptr {
	return uintptr(uint32(sz.Col) | uint32(sz.Row)<<16)
}

type pseudoconsole struct {
	api          *conpty_api
	hpc          windows.Handle
	input        windows.Handle // write end of the pseudoconsole's input pipe
	output       windows.Handle // read end of the pseudoconsole's output pipe
	size         tty.Winsize
	close_once   sync.Once
	release_once sync.Once
}

// open_pseudoconsole creates a pseudoconsole the size of term
func open_pseudoconsole(term *tty.Term) (p *pseudoconsole, err error) {
	api, err := load_conpty_api()
	if err != nil {
		return nil, err
	}
	sz, err := term.GetSize()
	if err != nil {
		return nil, err
	}
	if sz.Row == 0 || sz.Col == 0 {
		return nil, fmt.Errorf("invalid console size: %dx%d", sz.Col, sz.Row)
	}
	var in_read, in_write, out_read, out_write windows.Handle
	if err = windows.CreatePipe(&in_read, &in_write, nil, 0); err != nil {
		return nil, err
	}
	if err = windows.CreatePipe(&out_read, &out_write, nil, 0); err != nil {
		windows.CloseHandle(in_read)
		windows.CloseHandle(in_write)
		return nil, err
	}
	p = &pseudoconsole{api: api, input: in_write, output: out_read, size: *sz}
	hr, _, _ := api.create.Call(coord(sz), uintptr(in_read), uintptr(out_write), 0, uintptr(unsafe.Pointer(&p.hpc)))
	// the pseudoconsole has its own copies of these
	windows.CloseHandle(in_read)
	windows.CloseHandle(out_write)
	if hr != uintptr(windows.S_OK) {
		windows.CloseHandle(in_write)
		windows.CloseHandle(out_read)
		return nil, fmt.Errorf("ConptyCreatePseudoConsole failed with HRESULT: 0x%08x", uint32(hr))
	}
	return p, nil
}

// close_console closes the pseudoconsole, which ends its output once
// everything written to it has been read.
func (self *pseudoconsole) close_console() {
	self.close_once.Do(func() {
		_, _, _ = self.api.close.Call(uintptr(self.hpc))
	})
}

// Close releases the pseudoconsole, safe to call more than once
func (self *pseudoconsole) Close() {
	self.close_console()
	self.release_once.Do(func() {
		windows.CloseHandle(self.input)
		windows.CloseHandle(self.output)
	})
}

func (self *pseudoconsole) resize_to(term *tty.Term) {
	sz, err := term.GetSize()
	if err != nil || sz.Row == 0 || sz.Col == 0 || (sz.Row == self.size.Row && sz.Col == self.size.Col) {
		return
	}
	if hr, _, _ := self.api.resize.Call(uintptr(self.hpc), coord(sz)); hr == uintptr(windows.S_OK) {
		self.size = *sz
	}
}

func (self *pseudoconsole) start(argv []string) (process windows.Handle, err error) {
	exe, err := exec.LookPath(argv[0])
	if err != nil {
		return 0, err
	}
	if exe, err = filepath.Abs(exe); err != nil {
		return 0, err
	}
	attrs, err := windows.NewProcThreadAttributeList(1)
	if err != nil {
		return 0, err
	}
	defer attrs.Delete()
	// The attribute value is the HPCON itself, not a pointer to it
	hpc := *(*unsafe.Pointer)(unsafe.Pointer(&self.hpc))
	if err = attrs.Update(windows.PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, unsafe.Sizeof(self.hpc)); err != nil {
		return 0, err
	}
	si := windows.StartupInfoEx{ProcThreadAttributeList: attrs.List()}
	si.Cb = uint32(unsafe.Sizeof(si))
	// Null standard handles so that ssh uses the pseudoconsole rather than
	// inheriting the kitten's console handles
	si.Flags = windows.STARTF_USESTDHANDLES
	exe16, err := windows.UTF16PtrFromString(exe)
	if err != nil {
		return 0, err
	}
	cmdline, err := windows.UTF16PtrFromString(windows.ComposeCommandLine(argv))
	if err != nil {
		return 0, err
	}
	var pi windows.ProcessInformation
	if err = windows.CreateProcess(exe16, cmdline, nil, nil, false, windows.EXTENDED_STARTUPINFO_PRESENT|windows.CREATE_UNICODE_ENVIRONMENT, nil, nil, &si.StartupInfo, &pi); err != nil {
		return 0, &os.PathError{Op: "CreateProcess", Path: exe, Err: err}
	}
	windows.CloseHandle(pi.Thread)
	return pi.Process, nil
}

// The console modes the kitten needs while relaying: input arrives as VT
// sequences along with resize events, output is VT which wraps at the margin
// like a normal terminal.
var relay_console_mode tty.TermiosOperation = func(t *tty.Termios) {
	tty.SetRaw(t)
	t.OutMode |= windows.ENABLE_WRAP_AT_EOL_OUTPUT
}

// run runs argv in the pseudoconsole until it exits, relaying input from
// term and the pseudoconsole's output to out. It returns the exit code of
// the process. The pseudoconsole is closed when it returns.
func (self *pseudoconsole) run(term *tty.Term, argv []string, out io.Writer) (exit_code int, err error) {
	defer self.Close()
	if err = term.ApplyOperations(tty.TCSANOW, relay_console_mode); err != nil {
		return 1, err
	}
	process, err := self.start(argv)
	if err != nil {
		return 1, err
	}
	defer windows.CloseHandle(process)

	output_done := make(chan struct{})
	go func() {
		defer close(output_done)
		buf := make([]byte, 64*1024)
		for {
			var n uint32
			if err := windows.ReadFile(self.output, buf, &n, nil); err != nil || n == 0 {
				return
			}
			if _, err := out.Write(buf[:n]); err != nil {
				// keep draining so the pseudoconsole never blocks on a full pipe
				out = io.Discard
			}
		}
	}()

	// the size may have changed since the pseudoconsole was created
	self.resize_to(term)
	resized := make(chan os.Signal, 1)
	term.NotifyResize(resized, os.Interrupt)
	input_done := make(chan struct{})
	go func() {
		defer close(input_done)
		buf := make([]byte, 4096)
		for {
			n, rerr := term.Read(buf)
			for data := buf[:n]; len(data) > 0; {
				var written uint32
				if werr := windows.WriteFile(self.input, data, &written, nil); werr != nil {
					return
				}
				data = data[written:]
			}
			if rerr != nil {
				return
			}
		}
	}()
	process_done := make(chan struct{})
	go func() {
		defer close(process_done)
		_, _ = windows.WaitForSingleObject(process, windows.INFINITE)
	}()
	for waiting := true; waiting; {
		select {
		case <-resized:
			self.resize_to(term)
		case <-process_done:
			waiting = false
		}
	}
	term.NotifyResize(nil, nil)
	// Stop relaying input. Input typed after ssh exited stays in the console
	// for whatever runs next. Only interrupt a read that is in progress, as
	// a pending interrupt would cut short the next read of the console.
	select {
	case <-input_done:
	default:
		term.Interrupt()
		<-input_done
	}
	self.close_console()
	<-output_done

	var code uint32
	if err := windows.GetExitCodeProcess(process, &code); err != nil {
		return 1, err
	}
	return int(code), nil
}

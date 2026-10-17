// License: GPLv3 Copyright: 2025, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package tty

import (
	"errors"
	"fmt"
	"io"
	"os"
	"sync"
	"time"
	"unicode/utf16"
	"unicode/utf8"
	"unsafe"

	"golang.org/x/sys/windows"

	"github.com/kovidgoyal/kitty/tools/utils"
)

var _ = fmt.Print

// The Windows console has no termios. The equivalent state is the pair of
// console mode flags for the input and output handles.
type Termios struct {
	InMode, OutMode uint32
}

type Winsize struct {
	Row, Col, Xpixel, Ypixel uint16
}

// Term is a handle to the console. Input is read via ReadConsoleInputW,
// output is written via WriteConsoleW.
type Term struct {
	in, out    windows.Handle
	name       string
	states     []Termios
	interrupt  windows.Handle
	pending    []byte
	pending_hi rune // high surrogate waiting for its low half
	resize_ch  chan<- os.Signal
	resize_sig os.Signal
	lock       sync.Mutex
}

var (
	kernel32                  = windows.NewLazySystemDLL("kernel32.dll")
	proc_read_console_input_w = kernel32.NewProc("ReadConsoleInputW")
)

const (
	key_event                = 0x0001
	window_buffer_size_event = 0x0004
)

type input_record struct {
	event_type uint16
	_          uint16
	data       [16]byte
}

type key_event_record struct {
	key_down          int32
	repeat_count      uint16
	virtual_key_code  uint16
	virtual_scan_code uint16
	unicode_char      uint16
	control_key_state uint32
}

func read_console_input(h windows.Handle, records []input_record) (n uint32, err error) {
	r1, _, e1 := proc_read_console_input_w.Call(uintptr(h), uintptr(unsafe.Pointer(&records[0])), uintptr(len(records)), uintptr(unsafe.Pointer(&n)))
	if r1 == 0 {
		return 0, e1
	}
	return n, nil
}

func eintr_retry_noret(f func() error) error { return f() }

func eintr_retry_intret(f func() (int, error)) (int, error) { return f() }

func IsTerminal(fd uintptr) bool {
	var mode uint32
	return windows.GetConsoleMode(windows.Handle(fd), &mode) == nil
}

type TermiosOperation func(t *Termios)

func SetReadTimeout(d time.Duration) TermiosOperation {
	return func(t *Termios) {}
}

var SetBlockingRead TermiosOperation = SetReadTimeout(0)

var SetNoCanonical TermiosOperation = func(t *Termios) {
	t.InMode &^= windows.ENABLE_LINE_INPUT | windows.ENABLE_PROCESSED_INPUT
	t.InMode |= windows.ENABLE_VIRTUAL_TERMINAL_INPUT
}

var SetRaw TermiosOperation = func(t *Termios) {
	t.InMode &^= windows.ENABLE_ECHO_INPUT | windows.ENABLE_LINE_INPUT | windows.ENABLE_PROCESSED_INPUT | windows.ENABLE_MOUSE_INPUT | windows.ENABLE_QUICK_EDIT_MODE
	t.InMode |= windows.ENABLE_VIRTUAL_TERMINAL_INPUT | windows.ENABLE_WINDOW_INPUT | windows.ENABLE_EXTENDED_FLAGS
	t.OutMode |= windows.ENABLE_PROCESSED_OUTPUT | windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING | windows.DISABLE_NEWLINE_AUTO_RETURN
	t.OutMode &^= windows.ENABLE_WRAP_AT_EOL_OUTPUT
}

// SetRawWithKeyEvents keeps native console key records instead of VT text.
var SetRawWithKeyEvents TermiosOperation = func(t *Termios) {
	SetRaw(t)
	// VT input turns even real keys into synthetic records with no key identity.
	t.InMode &^= windows.ENABLE_VIRTUAL_TERMINAL_INPUT
}

var SetNoEcho TermiosOperation = func(t *Termios) {
	t.InMode &^= windows.ENABLE_ECHO_INPUT
}

var SetReadPassword TermiosOperation = func(t *Termios) {
	t.InMode &^= windows.ENABLE_ECHO_INPUT | windows.ENABLE_LINE_INPUT
	t.InMode |= windows.ENABLE_PROCESSED_INPUT | windows.ENABLE_VIRTUAL_TERMINAL_INPUT
}

func open_console(name string, access uint32) (windows.Handle, error) {
	p, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return windows.InvalidHandle, err
	}
	h, err := windows.CreateFile(p, access, windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE, nil, windows.OPEN_EXISTING, 0, 0)
	if err != nil {
		return windows.InvalidHandle, &os.PathError{Op: "open", Path: name, Err: err}
	}
	return h, nil
}

func new_term(in, out windows.Handle, name string, operations ...TermiosOperation) (self *Term, err error) {
	self = &Term{in: in, out: out, name: name}
	if self.interrupt, err = windows.CreateEvent(nil, 1, 0, nil); err != nil {
		return nil, err
	}
	if err = self.ApplyOperations(TCSANOW, operations...); err != nil {
		self.Close()
		return nil, err
	}
	return self, nil
}

// WrapTerm uses the specified handle for both input and output
func WrapTerm(fd int, name string, operations ...TermiosOperation) (self *Term, err error) {
	if name == "" {
		name = fmt.Sprintf("<fd: %d>", fd)
	}
	p := windows.CurrentProcess()
	var h windows.Handle
	if err = windows.DuplicateHandle(p, windows.Handle(fd), p, &h, 0, false, windows.DUPLICATE_SAME_ACCESS); err != nil {
		return nil, &os.PathError{Op: "dup", Path: name, Err: err}
	}
	return new_term(h, h, name, operations...)
}

// OpenTerm opens the console. The name is ignored as Windows has only a
// single console per process.
func OpenTerm(name string, operations ...TermiosOperation) (self *Term, err error) {
	in, err := open_console("CONIN$", windows.GENERIC_READ|windows.GENERIC_WRITE)
	if err != nil {
		return nil, err
	}
	out, err := open_console("CONOUT$", windows.GENERIC_READ|windows.GENERIC_WRITE)
	if err != nil {
		windows.CloseHandle(in)
		return nil, err
	}
	if self, err = new_term(in, out, name, operations...); err != nil {
		windows.CloseHandle(in)
		windows.CloseHandle(out)
	}
	return
}

func OpenControllingTerm(operations ...TermiosOperation) (self *Term, err error) {
	return OpenTerm(Ctermid(), operations...)
}

// Fd returns the console input handle
func (self *Term) Fd() int {
	if self.in == windows.InvalidHandle {
		return -1
	}
	return int(self.in)
}

func (self *Term) Close() error {
	var err error
	if self.in != windows.InvalidHandle {
		err = windows.CloseHandle(self.in)
		if self.out != self.in {
			if cerr := windows.CloseHandle(self.out); err == nil {
				err = cerr
			}
		}
		self.in, self.out = windows.InvalidHandle, windows.InvalidHandle
	}
	if self.interrupt != 0 {
		windows.CloseHandle(self.interrupt)
		self.interrupt = 0
	}
	return err
}

func (self *Term) WasEchoOnOriginally() bool {
	if len(self.states) > 0 {
		return self.states[0].InMode&windows.ENABLE_ECHO_INPUT != 0
	}
	return false
}

func (self *Term) Tcgetattr(ans *Termios) error {
	if err := windows.GetConsoleMode(self.in, &ans.InMode); err != nil {
		return err
	}
	return windows.GetConsoleMode(self.out, &ans.OutMode)
}

func (self *Term) Tcsetattr(when uintptr, ans *Termios) error {
	if err := windows.SetConsoleMode(self.in, ans.InMode); err != nil {
		return err
	}
	if self.out == self.in {
		return nil
	}
	return windows.SetConsoleMode(self.out, ans.OutMode)
}

// NotifyResize arranges for sig to be sent on ch whenever the console
// window is resized, the Windows equivalent of SIGWINCH.
func (self *Term) NotifyResize(ch chan<- os.Signal, sig os.Signal) {
	self.lock.Lock()
	defer self.lock.Unlock()
	self.resize_ch, self.resize_sig = ch, sig
}

// Interrupt causes any blocked Read to return with an error
func (self *Term) Interrupt() {
	if self.interrupt != 0 {
		windows.SetEvent(self.interrupt)
	}
}

var ErrInterrupted = errors.New("read from terminal was interrupted")

func (self *Term) wait_for_input(d time.Duration) error {
	timeout := uint32(windows.INFINITE)
	if d >= 0 {
		timeout = uint32(clamp(d.Milliseconds(), 0, int64(windows.INFINITE)-1))
	}
	ev, err := windows.WaitForMultipleObjects([]windows.Handle{self.in, self.interrupt}, false, timeout)
	if err != nil {
		return err
	}
	switch ev {
	case windows.WAIT_OBJECT_0:
		return nil
	case windows.WAIT_OBJECT_0 + 1:
		windows.ResetEvent(self.interrupt)
		return ErrInterrupted
	case uint32(windows.WAIT_TIMEOUT):
		return os.ErrDeadlineExceeded
	}
	return fmt.Errorf("unexpected result from WaitForMultipleObjects: %d", ev)
}

// KeyEventEncoder converts native console keys to terminal input. Returning an
// empty string preserves the character, including synthetic VT input records.
type KeyEventEncoder func(virtual_key_code uint16, char rune, control_key_state uint32) string

func (self *Term) append_key_event(k *key_event_record, encode KeyEventEncoder) {
	if k.key_down == 0 {
		return
	}
	r := rune(k.unicode_char)
	if utf16.IsSurrogate(r) {
		if self.pending_hi != 0 {
			r = utf16.DecodeRune(self.pending_hi, r)
			self.pending_hi = 0
		} else {
			self.pending_hi = r
			return
		}
	}
	var encoded string
	if encode != nil {
		encoded = encode(k.virtual_key_code, r, k.control_key_state)
	}
	for i := uint16(0); i < max(k.repeat_count, 1); i++ {
		if encoded != "" {
			self.pending = append(self.pending, encoded...)
		} else if r != 0 {
			self.pending = utf8.AppendRune(self.pending, r)
		}
	}
}

// Drain all available console input records into self.pending
func (self *Term) fill_pending(encode KeyEventEncoder) error {
	var records [64]input_record
	n, err := read_console_input(self.in, records[:])
	if err != nil {
		return err
	}
	for _, rec := range records[:n] {
		switch rec.event_type {
		case key_event:
			k := (*key_event_record)(unsafe.Pointer(&rec.data[0]))
			self.append_key_event(k, encode)
		case window_buffer_size_event:
			self.lock.Lock()
			ch, sig := self.resize_ch, self.resize_sig
			self.lock.Unlock()
			if ch != nil {
				select {
				case ch <- sig:
				default:
				}
			}
		}
	}
	return nil
}

func (self *Term) ReadWithTimeout(b []byte, d time.Duration) (n int, err error) {
	return self.read_with_timeout(b, d, nil)
}

// ReadKeyEvents encodes native keys from a console in SetRawWithKeyEvents mode,
// without changing the byte-oriented input used by other terminal readers.
func (self *Term) ReadKeyEvents(b []byte, encode KeyEventEncoder) (n int, err error) {
	return self.read_with_timeout(b, -1, encode)
}

func (self *Term) read_with_timeout(b []byte, d time.Duration, encode KeyEventEncoder) (n int, err error) {
	for len(self.pending) == 0 {
		if err = self.wait_for_input(d); err != nil {
			return 0, err
		}
		if !IsTerminal(uintptr(self.in)) {
			return self.read_file(b)
		}
		if err = self.fill_pending(encode); err != nil {
			return 0, err
		}
	}
	n = copy(b, self.pending)
	self.pending = self.pending[n:]
	return n, nil
}

func (self *Term) read_file(b []byte) (n int, err error) {
	var done uint32
	if err = windows.ReadFile(self.in, b, &done, nil); err != nil {
		return 0, err
	}
	if done == 0 {
		return 0, io.EOF
	}
	return int(done), nil
}

func is_temporary_read_error(err error) bool {
	return errors.Is(err, os.ErrDeadlineExceeded)
}

func (self *Term) Read(b []byte) (n int, err error) {
	return self.ReadWithTimeout(b, -1)
}

func (self *Term) Write(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	if !IsTerminal(uintptr(self.out)) {
		var done uint32
		if err := windows.WriteFile(self.out, b, &done, nil); err != nil {
			return int(done), err
		}
		return int(done), nil
	}
	// Trailing incomplete UTF-8 sequences are not written, as with a
	// partial write on a POSIX file descriptor.
	end := len(b)
	for end > 0 && !utf8.FullRune(b[end-min(end, utf8.UTFMax):end]) {
		end--
	}
	for end > 0 && end < len(b) && !utf8.RuneStart(b[end]) {
		end--
	}
	if end == 0 {
		return 0, io.ErrShortWrite
	}
	u := utf16.Encode([]rune(string(b[:end])))
	for written := 0; written < len(u); {
		var done uint32
		if err := windows.WriteConsole(self.out, &u[written], uint32(len(u)-written), &done, nil); err != nil {
			return 0, err
		}
		written += int(done)
	}
	if end < len(b) {
		return end, io.ErrShortWrite
	}
	return end, nil
}

func is_temporary_error(err error) bool {
	return errors.Is(err, io.ErrShortWrite)
}

func (self *Term) WriteString(b string) (int, error) {
	return self.Write(utils.UnsafeStringToBytes(b))
}

func GetSize(fd int) (*Winsize, error) {
	var info windows.ConsoleScreenBufferInfo
	if err := windows.GetConsoleScreenBufferInfo(windows.Handle(fd), &info); err != nil {
		return nil, err
	}
	return &Winsize{
		Col: uint16(info.Window.Right - info.Window.Left + 1),
		Row: uint16(info.Window.Bottom - info.Window.Top + 1),
	}, nil
}

func (self *Term) GetSize() (*Winsize, error) {
	return GetSize(int(self.out))
}

func Ctermid() string { return "CONIN$" }

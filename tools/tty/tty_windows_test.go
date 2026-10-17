// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package tty

import (
	"testing"

	"golang.org/x/sys/windows"
)

func TestRawConsoleKeyMode(t *testing.T) {
	for _, initial := range []uint32{0, 0x3a8, ^uint32(0)} {
		raw := Termios{InMode: initial, OutMode: initial}
		SetRaw(&raw)
		keys := Termios{InMode: initial, OutMode: initial}
		SetRawWithKeyEvents(&keys)
		if raw.InMode&windows.ENABLE_VIRTUAL_TERMINAL_INPUT == 0 {
			t.Fatal("byte-oriented raw mode lost VT input")
		}
		if keys.InMode&windows.ENABLE_VIRTUAL_TERMINAL_INPUT != 0 {
			t.Fatal("VT input would turn native keys into synthetic text")
		}
		raw.InMode &^= windows.ENABLE_VIRTUAL_TERMINAL_INPUT
		if keys != raw {
			t.Fatalf("key mode changed unrelated console flags: raw=%+v keys=%+v", raw, keys)
		}
	}
}

func TestConsoleKeyRecords(t *testing.T) {
	term := &Term{}
	var calls int
	encoder := func(vk uint16, char rune, state uint32) string {
		calls++
		if vk != windows.VK_UP || char != 0 || state != windows.LEFT_CTRL_PRESSED {
			t.Fatalf("key identity was not preserved: vk=%d char=%d state=%d", vk, char, state)
		}
		return "\x1b[1;5A"
	}
	k := key_event_record{key_down: 1, repeat_count: 3, virtual_key_code: windows.VK_UP, control_key_state: windows.LEFT_CTRL_PRESSED}
	term.append_key_event(&k, encoder)
	if calls != 1 || string(term.pending) != "\x1b[1;5A\x1b[1;5A\x1b[1;5A" {
		t.Fatalf("repeat count was not preserved: %q, calls=%d", term.pending, calls)
	}
	k.key_down = 0
	term.append_key_event(&k, encoder)
	if calls != 1 {
		t.Fatal("key release was converted to a press")
	}
}

func TestConsoleTextRecords(t *testing.T) {
	for _, encode := range []KeyEventEncoder{nil, func(uint16, rune, uint32) string { return "" }} {
		term := &Term{}
		for _, ch := range []uint16{27, '[', '1', '2', ';', '3', '4', 'R', 0xd83d, 0xde00} {
			term.append_key_event(&key_event_record{key_down: 1, unicode_char: ch}, encode)
		}
		if got := string(term.pending); got != "\x1b[12;34R😀" {
			t.Fatalf("synthetic VT input or UTF-16 was corrupted: %q", got)
		}
	}
	term := &Term{}
	for _, ch := range []uint16{'a', '\b', '\r', 27, 0} {
		term.append_key_event(&key_event_record{key_down: 1, unicode_char: ch}, nil)
	}
	if got := string(term.pending); got != "a\b\r\x1b" {
		t.Fatalf("byte-oriented terminal input changed: %q", got)
	}
}

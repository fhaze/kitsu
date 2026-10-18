// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package tty

import (
	"testing"
	"unsafe"

	"golang.org/x/sys/windows"
)

func TestConsoleMouseRecords(t *testing.T) {
	for _, tc := range []struct {
		name                           string
		previous, buttons, mods, flags uint32
		want                           string
	}{
		{name: "left press", buttons: 1, want: "\x1b[<0;854;573M"},
		{name: "left release", previous: 1, want: "\x1b[<0;854;573m"},
		{name: "right press", buttons: 2, want: "\x1b[<2;854;573M"},
		{name: "middle press", buttons: 4, want: "\x1b[<1;854;573M"},
		{name: "fourth press", buttons: 8, want: "\x1b[<128;854;573M"},
		{name: "fifth release", previous: 16, want: "\x1b[<129;854;573m"},
		{name: "release one button", previous: 3, buttons: 2, want: "\x1b[<0;854;573m"},
		{name: "release all buttons", previous: 7, want: "\x1b[<0;854;573m\x1b[<2;854;573m\x1b[<1;854;573m"},
		{name: "hover", flags: windows.MOUSE_MOVED, want: "\x1b[<35;854;573M"},
		{name: "drag", previous: 1, buttons: 1, flags: windows.MOUSE_MOVED, want: "\x1b[<32;854;573M"},
		{name: "right drag", previous: 2, buttons: 2, flags: windows.MOUSE_MOVED, want: "\x1b[<34;854;573M"},
		{name: "double click press", buttons: 1, flags: windows.DOUBLE_CLICK, want: "\x1b[<0;854;573M"},
		{name: "unchanged buttons", previous: 1, buttons: 1},
		{name: "wheel up", buttons: 120 << 16, flags: windows.MOUSE_WHEELED, want: "\x1b[<64;854;573M"},
		{name: "wheel down", buttons: 0xff88 << 16, flags: windows.MOUSE_WHEELED, want: "\x1b[<65;854;573M"},
		{name: "wheel right", buttons: 120 << 16, flags: windows.MOUSE_HWHEELED, want: "\x1b[<67;854;573M"},
		{name: "wheel left", buttons: 0xff88 << 16, flags: windows.MOUSE_HWHEELED, want: "\x1b[<66;854;573M"},
		{name: "wheel repeats", buttons: 240 << 16, flags: windows.MOUSE_WHEELED, want: "\x1b[<64;854;573M\x1b[<64;854;573M"},
		{name: "wheel with button", previous: 1, buttons: 1 | 120<<16, flags: windows.MOUSE_WHEELED, want: "\x1b[<64;854;573M"},
		{name: "zero wheel delta", flags: windows.MOUSE_WHEELED},
		{name: "modifiers", buttons: 1, mods: windows.SHIFT_PRESSED | windows.LEFT_ALT_PRESSED | windows.RIGHT_CTRL_PRESSED, want: "\x1b[<28;854;573M"},
		{name: "other modifiers", buttons: 1, mods: windows.RIGHT_ALT_PRESSED | windows.LEFT_CTRL_PRESSED | windows.CAPSLOCK_ON, want: "\x1b[<24;854;573M"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			term := &Term{mouse_buttons: tc.previous}
			record := input_record{event_type: mouse_event}
			*(*mouse_event_record)(unsafe.Pointer(&record.data[0])) = mouse_event_record{
				x: 853, y: 572, button_state: tc.buttons, control_key_state: tc.mods, event_flags: tc.flags,
			}
			term.append_input_records([]input_record{record}, func(uint16, rune, uint32) string { return "" })
			if got := string(term.pending); got != tc.want {
				t.Fatalf("got %q, want %q", got, tc.want)
			}
			if term.mouse_buttons != tc.buttons&0x1f {
				t.Fatalf("button state includes wheel delta: %x", term.mouse_buttons)
			}
		})
	}
}

func TestConsoleMouseClickAndKeyRecords(t *testing.T) {
	term := &Term{}
	encoder := func(uint16, rune, uint32) string { return "\x1b[121u" }
	for _, buttons := range []uint32{1, 0, 1, 0} {
		record := input_record{event_type: mouse_event}
		*(*mouse_event_record)(unsafe.Pointer(&record.data[0])) = mouse_event_record{button_state: buttons}
		term.append_input_records([]input_record{record}, encoder)
	}
	record := input_record{event_type: key_event}
	*(*key_event_record)(unsafe.Pointer(&record.data[0])) = key_event_record{key_down: 1, virtual_key_code: 'Y', unicode_char: 'y'}
	term.append_input_records([]input_record{record}, encoder)
	want := "\x1b[<0;1;1M\x1b[<0;1;1m\x1b[<0;1;1M\x1b[<0;1;1m\x1b[121u"
	if got := string(term.pending); got != want {
		t.Fatalf("mouse click or native key encoding lost: %q", got)
	}
}

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

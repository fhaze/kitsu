// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package loop

import (
	"strings"
	"testing"

	"golang.org/x/sys/windows"
)

func TestWindowsConsoleKeys(t *testing.T) {
	const vk_packet = 0xe7
	for _, tc := range []struct {
		name, shortcut, text string
		vk                   uint16
		char                 rune
		state                uint32
	}{
		{name: "backspace", shortcut: "backspace", vk: windows.VK_BACK, char: '\b'},
		{name: "enter", shortcut: "enter", vk: windows.VK_RETURN, char: '\r'},
		{name: "escape", shortcut: "esc", vk: windows.VK_ESCAPE, char: '\x1b'},
		{name: "tab", shortcut: "tab", vk: windows.VK_TAB, char: '\t'},
		{name: "shift enter", shortcut: "shift+enter", vk: windows.VK_RETURN, char: '\r', state: windows.SHIFT_PRESSED},
		{name: "ctrl enter", shortcut: "ctrl+enter", vk: windows.VK_RETURN, char: '\n', state: windows.LEFT_CTRL_PRESSED},
		{name: "alt enter", shortcut: "alt+enter", vk: windows.VK_RETURN, char: '\r', state: windows.LEFT_ALT_PRESSED},
		{name: "shift tab", shortcut: "shift+tab", vk: windows.VK_TAB, char: '\t', state: windows.SHIFT_PRESSED},
		{name: "ctrl backspace", shortcut: "ctrl+backspace", vk: windows.VK_BACK, char: '\x7f', state: windows.RIGHT_CTRL_PRESSED},
		{name: "ctrl c", shortcut: "ctrl+c", vk: 'C', char: 3, state: windows.LEFT_CTRL_PRESSED},
		{name: "ctrl u", shortcut: "ctrl+u", vk: 'U', char: 21, state: windows.RIGHT_CTRL_PRESSED},
		{name: "ctrl space", shortcut: "ctrl+space", vk: windows.VK_SPACE, state: windows.LEFT_CTRL_PRESSED},
		{name: "up", shortcut: "up", vk: windows.VK_UP, state: windows.ENHANCED_KEY},
		{name: "ctrl home", shortcut: "ctrl+home", vk: windows.VK_HOME, state: windows.LEFT_CTRL_PRESSED | windows.ENHANCED_KEY},
		{name: "delete", shortcut: "delete", vk: windows.VK_DELETE},
		{name: "f1", shortcut: "f1", vk: windows.VK_F1},
		{name: "f24", shortcut: "f24", vk: windows.VK_F24},
		{name: "text", shortcut: "a", vk: 'A', char: 'a', text: "a"},
		{name: "shift text", shortcut: "shift+a", vk: 'A', char: 'A', text: "A", state: windows.SHIFT_PRESSED},
		{name: "alt shortcut", shortcut: "alt+a", vk: 'A', char: 'a', state: windows.LEFT_ALT_PRESSED},
		{name: "alt backslash from conpty APC reply", shortcut: "alt+\\", vk: 0xdc, char: '\\', state: windows.LEFT_ALT_PRESSED},
		{name: "ctrl printable shortcut", shortcut: "ctrl+2", vk: '2', char: '2', state: windows.LEFT_CTRL_PRESSED},
		{name: "ctrl alt shortcut", shortcut: "ctrl+alt+e", vk: 'E', char: '€', state: windows.LEFT_CTRL_PRESSED | windows.LEFT_ALT_PRESSED},
		{name: "locks", shortcut: "enter", vk: windows.VK_RETURN, char: '\r', state: windows.CAPSLOCK_ON | windows.NUMLOCK_ON},
		{name: "unicode packet", shortcut: "é", vk: vk_packet, char: 'é', text: "é"},
		{name: "cjk packet", shortcut: "中", vk: vk_packet, char: '中', text: "中"},
		{name: "emoji packet", shortcut: "😀", vk: vk_packet, char: '😀', text: "😀"},
		{name: "altgr", shortcut: "e", vk: 'E', char: '€', text: "€", state: windows.RIGHT_ALT_PRESSED | windows.LEFT_CTRL_PRESSED},
	} {
		t.Run(tc.name, func(t *testing.T) {
			lp := new_loop()
			var got *KeyEvent
			var text string
			lp.OnKeyEvent = func(ev *KeyEvent) error {
				got = ev
				return nil
			}
			lp.OnText = func(s string, from_key, in_paste bool) error {
				if !from_key || in_paste {
					t.Error("native key text lost its origin")
				}
				text += s
				return nil
			}
			if err := lp.dispatch_input_data([]byte(encode_windows_key_event(tc.vk, tc.char, tc.state))); err != nil {
				t.Fatal(err)
			}
			if got == nil || !got.MatchesPressOrRepeat(tc.shortcut) {
				t.Fatalf("expected %s, got %v", tc.shortcut, got)
			}
			if text != tc.text {
				t.Fatalf("expected text %q, got %q", tc.text, text)
			}
		})
	}
}

func TestWindowsSyntheticInput(t *testing.T) {
	for _, raw := range []string{"\x1b[?29u", "\x1b[12;34R", "\x1b[A", "\x1b[127u", "\x1b[200~paste\r\n\x1b[201~", "é😀"} {
		for _, ch := range raw {
			if encoded := encode_windows_key_event(0, ch, 0); encoded != "" {
				t.Fatalf("synthetic VT input %q was re-encoded as %q", raw, encoded)
			}
		}
	}
	for _, vk := range []uint16{windows.VK_SHIFT, windows.VK_CONTROL, windows.VK_MENU} {
		if encoded := encode_windows_key_event(vk, 0, windows.LEFT_CTRL_PRESSED|windows.SHIFT_PRESSED); encoded != "" {
			t.Fatalf("modifier-only key was encoded as %q", encoded)
		}
	}
	// ESC must not remain pending and swallow the next input sequence.
	lp := new_loop()
	var keys []string
	lp.OnKeyEvent = func(ev *KeyEvent) error {
		keys = append(keys, ev.Key)
		ev.Handled = true
		return nil
	}
	for _, vk := range []uint16{windows.VK_ESCAPE, windows.VK_UP, windows.VK_RETURN} {
		for _, ch := range []byte(encode_windows_key_event(vk, 0, 0)) {
			if err := lp.dispatch_input_data([]byte{ch}); err != nil {
				t.Fatal(err)
			}
		}
	}
	if got := strings.Join(keys, ","); got != "ESCAPE,UP,ENTER" {
		t.Fatalf("unexpected keys after fragmented input: %s", got)
	}
}

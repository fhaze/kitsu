// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package loop

import (
	"fmt"
	"unicode"

	"golang.org/x/sys/windows"
)

var map_virtual_key = windows.NewLazySystemDLL("user32.dll").NewProc("MapVirtualKeyW")

var windows_functional_keys = map[uint16]string{
	windows.VK_BACK: "BACKSPACE", windows.VK_TAB: "TAB", windows.VK_RETURN: "ENTER",
	windows.VK_ESCAPE: "ESCAPE", windows.VK_INSERT: "INSERT", windows.VK_DELETE: "DELETE",
	windows.VK_HOME: "HOME", windows.VK_END: "END", windows.VK_PRIOR: "PAGE_UP", windows.VK_NEXT: "PAGE_DOWN",
	windows.VK_LEFT: "LEFT", windows.VK_RIGHT: "RIGHT", windows.VK_UP: "UP", windows.VK_DOWN: "DOWN",
}

func encode_windows_key_event(vk uint16, char rune, state uint32) string {
	// Terminal replies and already encoded input use synthetic records with no VK.
	if vk == 0 {
		return ""
	}
	ev := KeyEvent{Type: PRESS, Key: windows_functional_keys[vk]}
	if state&windows.SHIFT_PRESSED != 0 {
		ev.Mods |= SHIFT
	}
	if state&(windows.LEFT_ALT_PRESSED|windows.RIGHT_ALT_PRESSED) != 0 {
		ev.Mods |= ALT
	}
	if state&(windows.LEFT_CTRL_PRESSED|windows.RIGHT_CTRL_PRESSED) != 0 {
		ev.Mods |= CTRL
	}
	if state&windows.CAPSLOCK_ON != 0 {
		ev.Mods |= CAPS_LOCK
	}
	if state&windows.NUMLOCK_ON != 0 {
		ev.Mods |= NUM_LOCK
	}
	if ev.Key == "" && vk >= windows.VK_F1 && vk <= windows.VK_F24 {
		ev.Key = fmt.Sprintf("F%d", vk-windows.VK_F1+1)
	}
	if ev.Key == "" {
		if char == 0 && ev.Mods&(CTRL|ALT) == 0 {
			return ""
		}
		key, _, _ := map_virtual_key.Call(uintptr(vk), 2) // MAPVK_VK_TO_CHAR: unshifted character in the current layout
		r := rune(key & 0x7fffffff)
		if r == 0 {
			r = char
		}
		if r < 32 || r == 127 {
			return ""
		}
		ev.Key = string(unicode.ToLower(r))
		if char >= 32 && char != 127 {
			if state&(windows.RIGHT_ALT_PRESSED|windows.LEFT_CTRL_PRESSED) == windows.RIGHT_ALT_PRESSED|windows.LEFT_CTRL_PRESSED {
				ev.Mods &^= CTRL | ALT // AltGr produces text, not a Ctrl+Alt shortcut.
			}
			if ev.Mods&(CTRL|ALT) == 0 {
				ev.Text = string(char)
			}
			if ev.Mods&SHIFT != 0 && char != unicode.ToLower(r) {
				ev.ShiftedKey = string(char)
			}
		}
	}
	return ev.AsCSI()
}

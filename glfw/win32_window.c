//========================================================================
// GLFW 3.4 Win32 - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2002-2006 Marcus Geelnard
// Copyright (c) 2006-2019 Camilla Löwy <elmindreda@glfw.org>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================
// Please use C89 style variable declarations in this file because VS 2010
//========================================================================

#include "internal.h"

#include <limits.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <windowsx.h>
#include <shellapi.h>
#include <imm.h>
#include <errno.h>
#include "win32_keys.h"

static int getKeyMods(void);
static void applyTitlebarTheme(_GLFWwindow *window, GLFWColorScheme scheme);
static GLFWColorScheme query_system_color_theme(void);

#define WM_KITTY_TIMER_CHECK (WM_APP + 1)
#define WM_KITTY_DROP_DATA (WM_APP + 2)
#define WM_KITTY_WAKEUP (WM_APP + 3)
#define WM_KITTY_RESTORE_FOCUS (WM_APP + 4)
#define KITTY_MODAL_TIMER 1

static void
dispatchPendingTick(void) {
    if (!_glfw.win32.keep_going || !_glfw.win32.tick_callback || _glfw.win32.dispatching || !_glfw.win32.wakeup_pending) return;
    _glfw.win32.wakeup_pending = false;
    _glfw.win32.dispatching = true;
    _glfw.win32.tick_callback(_glfw.win32.tick_callback_data);
    _glfw.win32.dispatching = false;
}

static void
dispatchModalEvents(void) {
    if (!_glfw.win32.modal_loop_depth || !_glfw.win32.keep_going || _glfw.win32.dispatching) return;
    _glfwDispatchTimersWin32();
    dispatchPendingTick();
}

static void
enterModalLoop(_GLFWwindow *window) {
    window->win32.modal_loop_depth++;
    if (_glfw.win32.modal_loop_depth++ == 0) {
        // DefWindowProc owns the message pump until the move, resize or menu ends.
        if (!SetTimer(_glfw.win32.helperWindowHandle, KITTY_MODAL_TIMER, USER_TIMER_MINIMUM, NULL))
            _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create modal loop timer");
    }
}

static void
leaveModalLoop(_GLFWwindow *window) {
    if (window->win32.modal_loop_depth) {
        window->win32.modal_loop_depth--;
        if (--_glfw.win32.modal_loop_depth == 0) KillTimer(_glfw.win32.helperWindowHandle, KITTY_MODAL_TIMER);
    }
}

static const char *drop_mimes[] = {"text/uri-list", "text/plain"};

static bool
isWSLgFocusWindow(HWND handle) {
    WCHAR className[64];
    if (!handle || IsWindowVisible(handle)) return false;
    if (!GetClassNameW(handle, className, sizeof(className) / sizeof(className[0]))) return false;
    return wcscmp(className, L"TscShellContainerClass") == 0;
}

static void
encode_utf8(uint32_t ch, char *buf, size_t *len) {
    if (ch < 0x80) {
        buf[(*len)++] = (char)ch;
    } else if (ch < 0x800) {
        buf[(*len)++] = (char)(0xc0 | (ch >> 6));
        buf[(*len)++] = (char)(0x80 | (ch & 0x3f));
    } else if (ch < 0x10000) {
        buf[(*len)++] = (char)(0xe0 | (ch >> 12));
        buf[(*len)++] = (char)(0x80 | ((ch >> 6) & 0x3f));
        buf[(*len)++] = (char)(0x80 | (ch & 0x3f));
    } else {
        buf[(*len)++] = (char)(0xf0 | (ch >> 18));
        buf[(*len)++] = (char)(0x80 | ((ch >> 12) & 0x3f));
        buf[(*len)++] = (char)(0x80 | ((ch >> 6) & 0x3f));
        buf[(*len)++] = (char)(0x80 | (ch & 0x3f));
    }
}

static uint32_t
decode_utf16_pair(WCHAR high, WCHAR low) {
    return (((uint32_t)high - 0xd800) << 10) + ((uint32_t)low - 0xdc00) + 0x10000;
}

static bool
is_pua_char(uint32_t ch) {
    return (0xe000 <= ch && ch <= 0xf8ff) || (0xf0000 <= ch && ch <= 0xffffd) || (0x100000 <= ch && ch <= 0x10fffd);
}

static uint32_t
first_codepoint(const WCHAR *chars, int n) {
    if (n < 1) return 0;
    if (chars[0] >= 0xd800 && chars[0] <= 0xdbff && n > 1) return decode_utf16_pair(chars[0], chars[1]);
    return chars[0];
}

static uint32_t
to_lower_codepoint(uint32_t ch) {
    if (ch < 0x10000) {
        WCHAR w = (WCHAR)ch;
        CharLowerBuffW(&w, 1);
        return w;
    }
    return ch;
}

static bool
is_ignored_system_key(UINT vk) {
    switch (vk) {
        case VK_BROWSER_BACK:
        case VK_BROWSER_FORWARD:
        case VK_BROWSER_REFRESH:
        case VK_BROWSER_STOP:
        case VK_BROWSER_SEARCH:
        case VK_BROWSER_FAVORITES:
        case VK_BROWSER_HOME:
        case VK_VOLUME_MUTE:
        case VK_VOLUME_DOWN:
        case VK_VOLUME_UP:
        case VK_MEDIA_NEXT_TRACK:
        case VK_MEDIA_PREV_TRACK:
        case VK_MEDIA_STOP:
        case VK_MEDIA_PLAY_PAUSE:
        case VK_LAUNCH_MAIL:
        case VK_LAUNCH_MEDIA_SELECT:
        case VK_LAUNCH_APP1:
        case VK_LAUNCH_APP2:
        case VK_SNAPSHOT:
        case VK_SLEEP: return true;
        default: return false;
    }
}

// Translate a virtual key to the unicode character it produces with the given
// modifier state, without disturbing the keyboard layout's dead key state.
static uint32_t
vk_to_unicode(UINT vk, UINT scancode, bool shift, bool altgr) {
    BYTE state[256] = {0};
    WCHAR chars[8];
    if (shift) state[VK_SHIFT] = 0x80;
    if (altgr) {
        state[VK_CONTROL] = 0x80;
        state[VK_MENU] = 0x80;
    }
    int n = ToUnicode(vk, scancode, state, chars, sizeof(chars) / sizeof(WCHAR), 0x4);
    if (n < 1) return 0;
    uint32_t ans = first_codepoint(chars, n);
    if (ans < 32 || ans == 127) return 0;
    return ans;
}

static uint32_t
functional_key_for_vk(UINT vk, bool extended) {
    switch (vk) {
        case VK_ESCAPE: return GLFW_FKEY_ESCAPE;
        case VK_RETURN: return extended ? GLFW_FKEY_KP_ENTER : GLFW_FKEY_ENTER;
        case VK_TAB: return GLFW_FKEY_TAB;
        case VK_BACK: return GLFW_FKEY_BACKSPACE;
        case VK_INSERT: return extended ? GLFW_FKEY_INSERT : GLFW_FKEY_KP_INSERT;
        case VK_DELETE: return extended ? GLFW_FKEY_DELETE : GLFW_FKEY_KP_DELETE;
        case VK_LEFT: return extended ? GLFW_FKEY_LEFT : GLFW_FKEY_KP_LEFT;
        case VK_RIGHT: return extended ? GLFW_FKEY_RIGHT : GLFW_FKEY_KP_RIGHT;
        case VK_UP: return extended ? GLFW_FKEY_UP : GLFW_FKEY_KP_UP;
        case VK_DOWN: return extended ? GLFW_FKEY_DOWN : GLFW_FKEY_KP_DOWN;
        case VK_PRIOR: return extended ? GLFW_FKEY_PAGE_UP : GLFW_FKEY_KP_PAGE_UP;
        case VK_NEXT: return extended ? GLFW_FKEY_PAGE_DOWN : GLFW_FKEY_KP_PAGE_DOWN;
        case VK_HOME: return extended ? GLFW_FKEY_HOME : GLFW_FKEY_KP_HOME;
        case VK_END: return extended ? GLFW_FKEY_END : GLFW_FKEY_KP_END;
        case VK_CLEAR: return GLFW_FKEY_KP_BEGIN;
        case VK_CAPITAL: return GLFW_FKEY_CAPS_LOCK;
        case VK_SCROLL: return GLFW_FKEY_SCROLL_LOCK;
        case VK_NUMLOCK: return GLFW_FKEY_NUM_LOCK;
        case VK_SNAPSHOT: return GLFW_FKEY_PRINT_SCREEN;
        case VK_PAUSE: return GLFW_FKEY_PAUSE;
        case VK_APPS: return GLFW_FKEY_MENU;
        case VK_F1: return GLFW_FKEY_F1;
        case VK_F2: return GLFW_FKEY_F2;
        case VK_F3: return GLFW_FKEY_F3;
        case VK_F4: return GLFW_FKEY_F4;
        case VK_F5: return GLFW_FKEY_F5;
        case VK_F6: return GLFW_FKEY_F6;
        case VK_F7: return GLFW_FKEY_F7;
        case VK_F8: return GLFW_FKEY_F8;
        case VK_F9: return GLFW_FKEY_F9;
        case VK_F10: return GLFW_FKEY_F10;
        case VK_F11: return GLFW_FKEY_F11;
        case VK_F12: return GLFW_FKEY_F12;
        case VK_F13: return GLFW_FKEY_F13;
        case VK_F14: return GLFW_FKEY_F14;
        case VK_F15: return GLFW_FKEY_F15;
        case VK_F16: return GLFW_FKEY_F16;
        case VK_F17: return GLFW_FKEY_F17;
        case VK_F18: return GLFW_FKEY_F18;
        case VK_F19: return GLFW_FKEY_F19;
        case VK_F20: return GLFW_FKEY_F20;
        case VK_F21: return GLFW_FKEY_F21;
        case VK_F22: return GLFW_FKEY_F22;
        case VK_F23: return GLFW_FKEY_F23;
        case VK_F24: return GLFW_FKEY_F24;
        case VK_NUMPAD0: return GLFW_FKEY_KP_0;
        case VK_NUMPAD1: return GLFW_FKEY_KP_1;
        case VK_NUMPAD2: return GLFW_FKEY_KP_2;
        case VK_NUMPAD3: return GLFW_FKEY_KP_3;
        case VK_NUMPAD4: return GLFW_FKEY_KP_4;
        case VK_NUMPAD5: return GLFW_FKEY_KP_5;
        case VK_NUMPAD6: return GLFW_FKEY_KP_6;
        case VK_NUMPAD7: return GLFW_FKEY_KP_7;
        case VK_NUMPAD8: return GLFW_FKEY_KP_8;
        case VK_NUMPAD9: return GLFW_FKEY_KP_9;
        case VK_DECIMAL: return GLFW_FKEY_KP_DECIMAL;
        case VK_DIVIDE: return GLFW_FKEY_KP_DIVIDE;
        case VK_MULTIPLY: return GLFW_FKEY_KP_MULTIPLY;
        case VK_SUBTRACT: return GLFW_FKEY_KP_SUBTRACT;
        case VK_ADD: return GLFW_FKEY_KP_ADD;
        case VK_LSHIFT: return GLFW_FKEY_LEFT_SHIFT;
        case VK_RSHIFT: return GLFW_FKEY_RIGHT_SHIFT;
        case VK_LCONTROL: return GLFW_FKEY_LEFT_CONTROL;
        case VK_RCONTROL: return GLFW_FKEY_RIGHT_CONTROL;
        case VK_LMENU: return GLFW_FKEY_LEFT_ALT;
        case VK_RMENU: return GLFW_FKEY_RIGHT_ALT;
        case VK_LWIN: return GLFW_FKEY_LEFT_SUPER;
        case VK_RWIN: return GLFW_FKEY_RIGHT_SUPER;
        case VK_SHIFT: return extended ? GLFW_FKEY_RIGHT_SHIFT : GLFW_FKEY_LEFT_SHIFT;
        case VK_CONTROL: return extended ? GLFW_FKEY_RIGHT_CONTROL : GLFW_FKEY_LEFT_CONTROL;
        case VK_MENU: return extended ? GLFW_FKEY_RIGHT_ALT : GLFW_FKEY_LEFT_ALT;
        case VK_MEDIA_PLAY_PAUSE: return GLFW_FKEY_MEDIA_PLAY_PAUSE;
        case VK_MEDIA_STOP: return GLFW_FKEY_MEDIA_STOP;
        case VK_MEDIA_NEXT_TRACK: return GLFW_FKEY_MEDIA_TRACK_NEXT;
        case VK_MEDIA_PREV_TRACK: return GLFW_FKEY_MEDIA_TRACK_PREVIOUS;
        case VK_VOLUME_DOWN: return GLFW_FKEY_LOWER_VOLUME;
        case VK_VOLUME_UP: return GLFW_FKEY_RAISE_VOLUME;
        case VK_VOLUME_MUTE: return GLFW_FKEY_MUTE_VOLUME;
        default: return 0;
    }
}

static void
send_ime_text(_GLFWwindow *window, GLFWIMEState state, const char *text) {
    GLFWkeyevent ev = {.action = GLFW_PRESS, .ime_state = state, .text = text};
    _glfwInputKeyboard(window, &ev);
}

static char *
utf8_from_wide_n(const WCHAR *src, int n) {
    if (n <= 0) return NULL;
    int size = WideCharToMultiByte(CP_UTF8, 0, src, n, NULL, 0, NULL, NULL);
    if (size <= 0) return NULL;
    char *ans = calloc((size_t)size + 1, 1);
    if (!ans) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, src, n, ans, size, NULL, NULL);
    return ans;
}

static void
handle_ime_composition(_GLFWwindow *window, LPARAM lParam) {
    HIMC himc = ImmGetContext(window->win32.handle);
    if (!himc) return;
    if (lParam & GCS_RESULTSTR) {
        LONG sz = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
        if (sz > 0) {
            WCHAR *buf = calloc((size_t)sz / sizeof(WCHAR) + 1, sizeof(WCHAR));
            if (buf) {
                ImmGetCompositionStringW(himc, GCS_RESULTSTR, buf, (DWORD)sz);
                char *text = utf8_from_wide_n(buf, (int)(sz / sizeof(WCHAR)));
                send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, "");
                window->win32.ime.preedit_active = false;
                if (text) send_ime_text(window, GLFW_IME_COMMIT_TEXT, text);
                free(text);
                free(buf);
            }
        }
    }
    if (lParam & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS)) {
        LONG sz = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
        if (sz > 0) {
            WCHAR *buf = calloc((size_t)sz / sizeof(WCHAR) + 1, sizeof(WCHAR));
            if (buf) {
                ImmGetCompositionStringW(himc, GCS_COMPSTR, buf, (DWORD)sz);
                char *text = utf8_from_wide_n(buf, (int)(sz / sizeof(WCHAR)));
                if (text) send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, text);
                window->win32.ime.preedit_active = true;
                free(text);
                free(buf);
            }
        } else if (window->win32.ime.preedit_active) {
            send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, "");
            window->win32.ime.preedit_active = false;
        }
    }
    ImmReleaseContext(window->win32.handle, himc);
}

// Pull any WM_CHAR/WM_DEADCHAR messages that TranslateMessage() queued for the
// key down message currently being processed, and accumulate their text.
static void
collect_key_text(_GLFWwindow *window, char *text, size_t text_capacity, bool *is_dead_key) {
    MSG msg;
    size_t len = 0;
    *is_dead_key = false;
    text[0] = 0;
    while (PeekMessageW(&msg, window->win32.handle, WM_CHAR, WM_DEADCHAR, PM_REMOVE) ||
           PeekMessageW(&msg, window->win32.handle, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE) ||
           PeekMessageW(&msg, window->win32.handle, WM_UNICHAR, WM_UNICHAR, PM_REMOVE)) {
        uint32_t codepoint = 0;
        if (msg.message == WM_DEADCHAR || msg.message == WM_SYSDEADCHAR) *is_dead_key = true;
        if (msg.message == WM_UNICHAR) {
            if (msg.wParam == UNICODE_NOCHAR) continue;
            codepoint = (uint32_t)msg.wParam;
        } else if (msg.wParam >= 0xd800 && msg.wParam <= 0xdbff) {
            window->win32.highSurrogate = (WCHAR)msg.wParam;
            continue;
        } else if (msg.wParam >= 0xdc00 && msg.wParam <= 0xdfff) {
            if (window->win32.highSurrogate) codepoint = decode_utf16_pair(window->win32.highSurrogate, (WCHAR)msg.wParam);
            window->win32.highSurrogate = 0;
        } else {
            codepoint = (uint32_t)msg.wParam;
            window->win32.highSurrogate = 0;
        }
        if (codepoint && len + 4 < text_capacity) encode_utf8(codepoint, text, &len);
    }
    text[len] = 0;
}

static DWORD
win32_control_key_state(bool extended) {
    DWORD state = extended ? ENHANCED_KEY : 0;
    if (GetKeyState(VK_RMENU) & 0x8000) state |= RIGHT_ALT_PRESSED;
    if (GetKeyState(VK_LMENU) & 0x8000) state |= LEFT_ALT_PRESSED;
    if (GetKeyState(VK_RCONTROL) & 0x8000) state |= RIGHT_CTRL_PRESSED;
    if (GetKeyState(VK_LCONTROL) & 0x8000) state |= LEFT_CTRL_PRESSED;
    if (GetKeyState(VK_SHIFT) & 0x8000) state |= SHIFT_PRESSED;
    if (GetKeyState(VK_NUMLOCK) & 1) state |= NUMLOCK_ON;
    if (GetKeyState(VK_CAPITAL) & 1) state |= CAPSLOCK_ON;
    if (GetKeyState(VK_SCROLL) & 1) state |= SCROLLLOCK_ON;
    return state;
}

static void
handle_key_message(_GLFWwindow *window, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const bool is_release = (HIWORD(lParam) & KF_UP) != 0;
    const bool extended = (HIWORD(lParam) & KF_EXTENDED) != 0;
    int mods = getKeyMods();
    UINT vk = (UINT)wParam;
    int scancode = (HIWORD(lParam) & (KF_EXTENDED | 0xff));
    if (is_ignored_system_key(vk)) return;
    if (!scancode) {
        // NOTE: Some synthetic key messages have a scancode of zero
        // HACK: Map the virtual key back to a usable scancode
        scancode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }
    // HACK: Alt+PrtSc has a different scancode than just PrtSc
    if (scancode == 0x54) scancode = 0x137;
    // HACK: Ctrl+Pause has a different scancode than just Pause
    if (scancode == 0x146) scancode = 0x45;
    // HACK: CJK IME sets the extended bit for right Shift
    if (scancode == 0x136) scancode = 0x36;

    if (vk == VK_PROCESSKEY) {
        // IME notifies that keys have been filtered by setting the
        // virtual key-code to VK_PROCESSKEY
        return;
    }
    // Resolve generic modifier virtual keys to their left/right variants
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        UINT resolved = MapVirtualKeyW((UINT)scancode, MAPVK_VSC_TO_VK_EX);
        if (resolved) vk = resolved;
    }
    if (vk == VK_LCONTROL && !is_release) {
        // NOTE: Alt Gr sends Left Ctrl followed by Right Alt
        // HACK: We only want one event for Alt Gr, so if we detect
        //       this sequence we discard this Left Ctrl message now
        //       and later report Right Alt normally
        MSG next;
        const DWORD time = GetMessageTime();
        if (PeekMessageW(&next, NULL, 0, 0, PM_NOREMOVE)) {
            if (next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN || next.message == WM_KEYUP || next.message == WM_SYSKEYUP) {
                if (next.wParam == VK_MENU && (HIWORD(next.lParam) & KF_EXTENDED) && next.time == time) return;
            }
        }
    }

    GLFWkeyevent ev = {.native_key = scancode, .native_key_id = (uint32_t)scancode, .action = is_release ? GLFW_RELEASE : GLFW_PRESS, .mods = mods};
    ev.win32_vk = vk;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) ev.win32_vk = VK_SHIFT;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) ev.win32_vk = VK_CONTROL;
    if (vk == VK_LMENU || vk == VK_RMENU) ev.win32_vk = VK_MENU;
    ev.win32_control_state = win32_control_key_state(extended);
    BYTE key_state[256];
    WCHAR chars[8];
    if (GetKeyboardState(key_state)) {
        int n = ToUnicode(vk, (UINT)scancode, key_state, chars, sizeof(chars) / sizeof(WCHAR), 0x4);
        if (n > 0) ev.win32_char = first_codepoint(chars, n);
        // VK_PACKET can deliver half a surrogate pair; collect_key_text assembles the full character.
        if (ev.win32_char >= 0xd800 && ev.win32_char <= 0xdfff) ev.win32_char = 0;
    }
    // Keypad keys report a different vk depending on numlock, use the scancode table for them
    ev.key = functional_key_for_vk(vk, extended);
    if (!ev.key && scancode < 512) {
        uint32_t k = _glfw.win32.keycodes[scancode];
        if (k >= GLFW_FKEY_FIRST) ev.key = k;
    }
    if (!ev.key) {
        ev.key = to_lower_codepoint(vk_to_unicode(vk, (UINT)scancode, false, false));
        if (!ev.key && scancode < 512) ev.key = _glfw.win32.keycodes[scancode];
        if (mods & GLFW_MOD_SHIFT) {
            uint32_t sk = vk_to_unicode(vk, (UINT)scancode, true, false);
            if (sk > 31 && sk != ev.key && !is_pua_char(sk)) ev.shifted_key = sk;
        }
        if (scancode < 512) {
            uint32_t ak = _glfw.win32.keycodes[scancode];
            if (ak > 31 && ak != ev.key && ak < GLFW_FKEY_FIRST && !is_pua_char(ak)) ev.alternate_key = ak;
        }
    }

    char text[64] = {0};
    if (!is_release) {
        bool is_dead_key = false;
        collect_key_text(window, text, sizeof(text), &is_dead_key);
        if (is_dead_key) {
            if (text[0]) send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, text);
            window->win32.ime.dead_key_active = true;
            return;
        }
        if (window->win32.ime.dead_key_active) {
            send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, "");
            window->win32.ime.dead_key_active = false;
        }
        if (text[0] && (unsigned char)text[0] < 32) text[0] = 0; // do not send text for ascii control codes
        if (text[0] == 127) text[0] = 0;
        // AltGr on Windows is reported as Ctrl+Alt; when it produced text treat it as a plain text key
        if (text[0] && (mods & GLFW_MOD_CONTROL) && (mods & GLFW_MOD_ALT) && (GetKeyState(VK_RMENU) & 0x8000)) {
            ev.mods = mods & ~(GLFW_MOD_CONTROL | GLFW_MOD_ALT);
        }
        ev.text = text;
    }

    // Wait for complete packet text (including surrogate pairs), but keep releases for repeat tracking.
    if (vk == VK_PACKET && !is_release && !text[0]) return;

    if (is_release && (vk == VK_LSHIFT || vk == VK_RSHIFT)) {
        // HACK: Release both Shift keys on Shift up event, as when both
        //       are pressed the first release does not emit any event
        // NOTE: The other half of this is in _glfwPlatformPollEvents
        GLFWkeyevent other = ev;
        other.key = vk == VK_LSHIFT ? GLFW_FKEY_RIGHT_SHIFT : GLFW_FKEY_LEFT_SHIFT;
        other.native_key = vk == VK_LSHIFT ? 0x36 : 0x2a;
        other.native_key_id = other.native_key;
        _glfwInputKeyboard(window, &ev);
        _glfwInputKeyboard(window, &other);
    } else {
        _glfwInputKeyboard(window, &ev);
    }
    (void)uMsg;
}

static void
send_scroll(_GLFWwindow *window, double x, double y) {
    GLFWScrollEvent ev = {
        .x_offset = x, .y_offset = y, .unscaled.x = x, .unscaled.y = y, .offset_type = GLFW_SCROLL_OFFEST_V120, .keyboard_modifiers = getKeyMods()};
    _glfwInputScroll(window, &ev);
}

// Drop handling {{{
static void
free_drop_data(_GLFWwindow *window) {
    free(window->win32.drop.uri_list);
    free(window->win32.drop.plain_text);
    memset(&window->win32.drop, 0, sizeof(window->win32.drop));
}

static void
handle_drop_files(_GLFWwindow *window, HDROP drop) {
    POINT pt;
    const int count = DragQueryFileW(drop, 0xffffffff, NULL, 0);
    DragQueryPoint(drop, &pt);
    free_drop_data(window);
    size_t uri_cap = 4096, uri_len = 0, plain_cap = 4096, plain_len = 0;
    char *uri = malloc(uri_cap), *plain = malloc(plain_cap);
    if (!uri || !plain) {
        free(uri);
        free(plain);
        DragFinish(drop);
        return;
    }
    uri[0] = 0;
    plain[0] = 0;
    for (int i = 0; i < count; i++) {
        const UINT length = DragQueryFileW(drop, i, NULL, 0);
        WCHAR *buffer = calloc((size_t)length + 1, sizeof(WCHAR));
        if (!buffer) continue;
        DragQueryFileW(drop, i, buffer, length + 1);
        char *path = _glfwCreateUTF8FromWideStringWin32(buffer);
        free(buffer);
        if (!path) continue;
        // file:///C:/foo/bar with percent-encoding of reserved chars, text/plain
        // gets the native path
        size_t needed = strlen(path) * 3 + 16;
        if (uri_len + needed >= uri_cap) {
            uri_cap = (uri_len + needed) * 2;
            uri = realloc(uri, uri_cap);
            if (!uri) break;
        }
        if (plain_len + needed >= plain_cap) {
            plain_cap = (plain_len + needed) * 2;
            plain = realloc(plain, plain_cap);
            if (!plain) break;
        }
        if (uri_len) {
            memcpy(uri + uri_len, "\r\n", 2);
            uri_len += 2;
        }
        if (plain_len) { plain[plain_len++] = '\n'; }
        // \\server\share\x is file://server/share/x
        const bool unc = path[0] == '\\' && path[1] == '\\';
        memcpy(uri + uri_len, "file:///", unc ? 5 : 8);
        uri_len += unc ? 5 : 8;
        for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
            if (*c == '\\') uri[uri_len++] = '/';
            else if (isalnum(*c) || strchr("-._~/:", *c)) uri[uri_len++] = (char)*c;
            else uri_len += (size_t)snprintf(uri + uri_len, 4, "%%%02X", *c);
        }
        size_t pl = strlen(path);
        memcpy(plain + plain_len, path, pl);
        plain_len += pl;
        free(path);
    }
    DragFinish(drop);
    if (!uri || !plain) {
        free(uri);
        free(plain);
        return;
    }
    uri[uri_len] = 0;
    plain[plain_len] = 0;
    window->win32.drop.uri_list = uri;
    window->win32.drop.uri_list_len = uri_len;
    window->win32.drop.plain_text = plain;
    window->win32.drop.plain_text_len = plain_len;
    window->win32.drop.active = true;

    const char *mimes[2] = {drop_mimes[0], drop_mimes[1]};
    _glfwInputDropEvent(window, GLFW_DROP_ENTER, pt.x, pt.y, mimes, 2, false);
    mimes[0] = drop_mimes[0];
    mimes[1] = drop_mimes[1];
    size_t accepted = _glfwInputDropEvent(window, GLFW_DROP_DROP, pt.x, pt.y, mimes, 2, false);
    for (size_t i = 0; i < accepted && i < 2; i++) _glfwPlatformRequestDropData(window, mimes[i]);
}
// }}}

// Returns the window style for the specified window
//
static DWORD
getWindowStyle(const _GLFWwindow *window) {
    DWORD style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

    if (window->monitor) style |= WS_POPUP;
    else {
        style |= WS_SYSMENU | WS_MINIMIZEBOX;

        if (window->decorated) {
            style |= WS_CAPTION;

            if (window->resizable) style |= WS_MAXIMIZEBOX | WS_THICKFRAME;
        } else style |= WS_POPUP;
    }

    return style;
}

// Returns the extended window style for the specified window
//
static DWORD
getWindowExStyle(const _GLFWwindow *window) {
    DWORD style = WS_EX_APPWINDOW;

    if (window->monitor || window->floating) style |= WS_EX_TOPMOST;

    return style;
}

// Returns the image whose area most closely matches the desired one
//
static const GLFWimage *
chooseImage(int count, const GLFWimage *images, int width, int height) {
    int i, leastDiff = INT_MAX;
    const GLFWimage *closest = NULL;

    for (i = 0; i < count; i++) {
        const int currDiff = abs(images[i].width * images[i].height - width * height);
        if (currDiff < leastDiff) {
            closest = images + i;
            leastDiff = currDiff;
        }
    }

    return closest;
}

// Creates an RGBA icon or cursor
//
static HICON
createIcon(const GLFWimage *image, int xhot, int yhot, bool icon) {
    int i;
    HDC dc;
    HICON handle;
    HBITMAP color, mask;
    BITMAPV5HEADER bi;
    ICONINFO ii;
    unsigned char *target = NULL;
    const unsigned char *source = image->pixels;

    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = image->width;
    bi.bV5Height = -image->height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask = 0x000000ff;
    bi.bV5AlphaMask = 0xff000000;

    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&target, NULL, (DWORD)0);
    ReleaseDC(NULL, dc);

    if (!color) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create RGBA bitmap");
        return NULL;
    }

    mask = CreateBitmap(image->width, image->height, 1, 1, NULL);
    if (!mask) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create mask bitmap");
        DeleteObject(color);
        return NULL;
    }

    for (i = 0; i < image->width * image->height; i++) {
        target[0] = source[2];
        target[1] = source[1];
        target[2] = source[0];
        target[3] = source[3];
        target += 4;
        source += 4;
    }

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = icon;
    ii.xHotspot = xhot;
    ii.yHotspot = yhot;
    ii.hbmMask = mask;
    ii.hbmColor = color;

    handle = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);

    if (!handle) {
        if (icon) {
            _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create icon");
        } else {
            _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create cursor");
        }
    }

    return handle;
}

// Enforce the content area aspect ratio based on which edge is being dragged
//
static void
applyAspectRatio(_GLFWwindow *window, int edge, RECT *area) {
    RECT frame = {0};
    const float ratio = (float)window->numer / (float)window->denom;
    const DWORD style = getWindowStyle(window);
    const DWORD exStyle = getWindowExStyle(window);

    if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
        AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, GetDpiForWindow(window->win32.handle));
    } else AdjustWindowRectEx(&frame, style, FALSE, exStyle);

    if (edge == WMSZ_LEFT || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_RIGHT || edge == WMSZ_BOTTOMRIGHT) {
        area->bottom = area->top + (frame.bottom - frame.top) + (int)(((area->right - area->left) - (frame.right - frame.left)) / ratio);
    } else if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        area->top = area->bottom - (frame.bottom - frame.top) - (int)(((area->right - area->left) - (frame.right - frame.left)) / ratio);
    } else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        area->right = area->left + (frame.right - frame.left) + (int)(((area->bottom - area->top) - (frame.bottom - frame.top)) * ratio);
    }
}

// Updates the cursor image according to its cursor mode
//
static void
updateCursorImage(_GLFWwindow *window) {
    if (window->cursorMode == GLFW_CURSOR_NORMAL) {
        if (window->cursor) SetCursor(window->cursor->win32.handle);
        else SetCursor(LoadCursorW(NULL, IDC_ARROW));
    } else SetCursor(NULL);
}

// Sets the cursor clip rect to the window content area
//
static void
captureCursor(_GLFWwindow *window) {
    RECT clipRect;
    GetClientRect(window->win32.handle, &clipRect);
    ClientToScreen(window->win32.handle, (POINT *)&clipRect.left);
    ClientToScreen(window->win32.handle, (POINT *)&clipRect.right);
    ClipCursor(&clipRect);
    _glfw.win32.capturedCursorWindow = window;
}

// Disabled clip cursor
//
static void
releaseCursor(void) {
    ClipCursor(NULL);
    _glfw.win32.capturedCursorWindow = NULL;
}

// Enables WM_INPUT messages for the mouse for the specified window
//
static void
enableRawMouseMotion(_GLFWwindow *window) {
    const RAWINPUTDEVICE rid = {0x01, 0x02, 0, window->win32.handle};

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) { _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to register raw input device"); }
}

// Disables WM_INPUT messages for the mouse
//
static void
disableRawMouseMotion(_GLFWwindow *window UNUSED) {
    const RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_REMOVE, NULL};

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) { _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to remove raw input device"); }
}

// Apply disabled cursor mode to a focused window
//
static void
disableCursor(_GLFWwindow *window) {
    _glfw.win32.disabledCursorWindow = window;
    _glfwPlatformGetCursorPos(window, &_glfw.win32.restoreCursorPosX, &_glfw.win32.restoreCursorPosY);
    updateCursorImage(window);
    _glfwCenterCursorInContentArea(window);
    captureCursor(window);

    if (window->rawMouseMotion) enableRawMouseMotion(window);
}

// Exit disabled cursor mode for the specified window
//
static void
enableCursor(_GLFWwindow *window) {
    if (window->rawMouseMotion) disableRawMouseMotion(window);

    _glfw.win32.disabledCursorWindow = NULL;
    releaseCursor();
    _glfwPlatformSetCursorPos(window, _glfw.win32.restoreCursorPosX, _glfw.win32.restoreCursorPosY);
    updateCursorImage(window);
}

// Returns whether the cursor is in the content area of the specified window
//
static bool
cursorInContentArea(_GLFWwindow *window) {
    RECT area;
    POINT pos;

    if (!GetCursorPos(&pos)) return false;

    if (WindowFromPoint(pos) != window->win32.handle) return false;

    GetClientRect(window->win32.handle, &area);
    ClientToScreen(window->win32.handle, (POINT *)&area.left);
    ClientToScreen(window->win32.handle, (POINT *)&area.right);

    return PtInRect(&area, pos);
}

// Update native window styles to match attributes
//
static void
updateWindowStyles(const _GLFWwindow *window) {
    RECT rect;
    DWORD style = GetWindowLongW(window->win32.handle, GWL_STYLE);
    style &= ~(WS_OVERLAPPEDWINDOW | WS_POPUP);
    style |= getWindowStyle(window);

    GetClientRect(window->win32.handle, &rect);

    if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
        AdjustWindowRectExForDpi(&rect, style, FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
    } else AdjustWindowRectEx(&rect, style, FALSE, getWindowExStyle(window));

    ClientToScreen(window->win32.handle, (POINT *)&rect.left);
    ClientToScreen(window->win32.handle, (POINT *)&rect.right);
    SetWindowLongW(window->win32.handle, GWL_STYLE, style);
    SetWindowPos(
        window->win32.handle, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
}

// Update window framebuffer transparency
//
static void
updateFramebufferTransparency(const _GLFWwindow *window) {
    BOOL composition, opaque;
    DWORD color;

    if (!IsWindowsVistaOrGreater()) return;

    if (FAILED(DwmIsCompositionEnabled(&composition)) || !composition) return;

    if (IsWindows8OrGreater() || (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)) && !opaque)) {
        HRGN region = CreateRectRgn(0, 0, -1, -1);
        DWM_BLURBEHIND bb = {0};
        bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        bb.hRgnBlur = region;
        bb.fEnable = TRUE;

        DwmEnableBlurBehindWindow(window->win32.handle, &bb);
        DeleteObject(region);
    } else {
        // HACK: Disable framebuffer transparency on Windows 7 when the
        //       colorization color is opaque, because otherwise the window
        //       contents is blended additively with the previous frame instead
        //       of replacing it
        DWM_BLURBEHIND bb = {0};
        bb.dwFlags = DWM_BB_ENABLE;
        DwmEnableBlurBehindWindow(window->win32.handle, &bb);
    }
}

// Retrieves and translates modifier keys
//
static int
getKeyMods(void) {
    int mods = 0;

    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= GLFW_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= GLFW_MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= GLFW_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= GLFW_MOD_SUPER;
    if (GetKeyState(VK_CAPITAL) & 1) mods |= GLFW_MOD_CAPS_LOCK;
    if (GetKeyState(VK_NUMLOCK) & 1) mods |= GLFW_MOD_NUM_LOCK;

    return mods;
}

static void
fitToMonitor(_GLFWwindow *window) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(window->monitor->win32.handle, &mi);
    SetWindowPos(
        window->win32.handle,
        HWND_TOPMOST,
        mi.rcMonitor.left,
        mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

// Make the specified window and its video mode active on its monitor
//
static void
acquireMonitor(_GLFWwindow *window) {
    if (!_glfw.win32.acquiredMonitorCount) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);

        // HACK: When mouse trails are enabled the cursor becomes invisible when
        //       the OpenGL ICD switches to page flipping
        if (IsWindowsXPOrGreater()) {
            SystemParametersInfoW(SPI_GETMOUSETRAILS, 0, &_glfw.win32.mouseTrailSize, 0);
            SystemParametersInfoW(SPI_SETMOUSETRAILS, 0, 0, 0);
        }
    }

    if (!window->monitor->window) _glfw.win32.acquiredMonitorCount++;

    _glfwSetVideoModeWin32(window->monitor, &window->videoMode);
    _glfwInputMonitorWindow(window->monitor, window);
}

// Remove the window and restore the original video mode
//
static void
releaseMonitor(_GLFWwindow *window) {
    if (window->monitor->window != window) return;

    _glfw.win32.acquiredMonitorCount--;
    if (!_glfw.win32.acquiredMonitorCount) {
        SetThreadExecutionState(ES_CONTINUOUS);

        // HACK: Restore mouse trail length saved in acquireMonitor
        if (IsWindowsXPOrGreater()) SystemParametersInfoW(SPI_SETMOUSETRAILS, _glfw.win32.mouseTrailSize, 0, 0);
    }

    _glfwInputMonitorWindow(window->monitor, NULL);
    _glfwRestoreVideoModeWin32(window->monitor);
}

// Manually maximize the window, for when SW_MAXIMIZE cannot be used
//
static void
maximizeWindowManually(_GLFWwindow *window) {
    RECT rect;
    DWORD style;
    MONITORINFO mi = {sizeof(mi)};

    GetMonitorInfoW(MonitorFromWindow(window->win32.handle, MONITOR_DEFAULTTONEAREST), &mi);

    rect = mi.rcWork;

    if (window->maxwidth != GLFW_DONT_CARE && window->maxheight != GLFW_DONT_CARE) {
        rect.right = _glfw_min(rect.right, rect.left + window->maxwidth);
        rect.bottom = _glfw_min(rect.bottom, rect.top + window->maxheight);
    }

    style = GetWindowLongW(window->win32.handle, GWL_STYLE);
    style |= WS_MAXIMIZE;
    SetWindowLongW(window->win32.handle, GWL_STYLE, style);

    if (window->decorated) {
        const DWORD exStyle = GetWindowLongW(window->win32.handle, GWL_EXSTYLE);

        if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
            const UINT dpi = GetDpiForWindow(window->win32.handle);
            AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
            OffsetRect(&rect, 0, GetSystemMetricsForDpi(SM_CYCAPTION, dpi));
        } else {
            AdjustWindowRectEx(&rect, style, FALSE, exStyle);
            OffsetRect(&rect, 0, GetSystemMetrics(SM_CYCAPTION));
        }

        rect.bottom = _glfw_min(rect.bottom, mi.rcWork.bottom);
    }

    SetWindowPos(
        window->win32.handle, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

// A high polling rate mouse sends a WM_MOUSEMOVE every millisecond or less. Only one of
// them per frame can ever be rendered, but every one is reported to programs that track
// the mouse, which then fall further and further behind, since ConPTY cannot keep up with
// a redraw per report. So deliver at most one mouse move per frame, the way macOS coalesces
// them, and deliver a pending one before any other input, so that clicks, scrolls and
// leaving the window all happen where the mouse actually is.

static bool armMotionTimer(monotonic_t at);

static void
deliverCursorPos(_GLFWwindow *window, monotonic_t now) {
    const monotonic_t interval = _glfwFrameIntervalWin32(window);
    // While the mouse keeps moving, keep to the cadence rather than restarting it from now,
    // so that timers firing a little late do not make it fall below one move per frame.
    // Restart it after a pause, or when delivering early because other input arrived.
    monotonic_t next = window->win32.motion.next_at + interval;
    if (next <= now || next > now + interval) next = now + interval;
    window->win32.motion.next_at = next;
    window->win32.motion.pending = false;
    _glfwInputCursorPos(window, window->win32.motion.x, window->win32.motion.y);
}

static void
flushCursorPos(_GLFWwindow *window) {
    if (window->win32.motion.pending) deliverCursorPos(window, monotonic());
}

static void
motionTimerFired(unsigned long long timer_id UNUSED, void *data UNUSED) {
    _glfw.win32.motion_timer.enabled = false;
    const monotonic_t now = monotonic();
    monotonic_t soonest = MONOTONIC_T_MAX;
    for (_GLFWwindow *w = _glfw.windowListHead, *next; w; w = next) {
        next = w->next;
        if (!w->win32.motion.pending) continue;
        if (now >= w->win32.motion.next_at) deliverCursorPos(w, now);
        else if (w->win32.motion.next_at < soonest) soonest = w->win32.motion.next_at;
    }
    if (soonest < MONOTONIC_T_MAX) armMotionTimer(soonest);
    else _glfwPlatformUpdateTimer(_glfw.win32.motion_timer.id, ms_to_monotonic_t(1000ll), false);
}

// Makes sure the motion timer fires no later than at, returns false if it could not be created
static bool
armMotionTimer(monotonic_t at) {
    if (_glfw.win32.motion_timer.enabled && _glfw.win32.motion_timer.fires_at <= at) return true;
    const monotonic_t delay = MAX(0, at - monotonic());
    if (!_glfw.win32.motion_timer.id) {
        _glfw.win32.motion_timer.id = _glfwPlatformAddTimer(delay, true, motionTimerFired, NULL, NULL);
        if (!_glfw.win32.motion_timer.id) return false;
    } else _glfwPlatformUpdateTimer(_glfw.win32.motion_timer.id, delay, true);
    _glfw.win32.motion_timer.enabled = true;
    _glfw.win32.motion_timer.fires_at = at;
    return true;
}

static void
coalesceCursorPos(_GLFWwindow *window, int x, int y) {
    window->win32.motion.x = x;
    window->win32.motion.y = y;
    window->win32.motion.pending = true;
    const monotonic_t now = monotonic();
    // Without a timer to deliver it later, deliver it now rather than lose it
    if (now >= window->win32.motion.next_at || !armMotionTimer(window->win32.motion.next_at)) deliverCursorPos(window, now);
}

static bool
isInputMessage(UINT uMsg) {
    return (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST) || (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) ||
           uMsg == WM_MOUSELEAVE || uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS;
}

// Window callback function (handles window messages)
//
static LRESULT CALLBACK
windowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    _GLFWwindow *window = GetPropW(hWnd, L"GLFW");
    if (!window) {
        // This is the message handling for the hidden helper window
        // and for a regular window during its initial creation

        switch (uMsg) {
            case WM_NCCREATE: {
                if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
                    const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lParam;
                    const _GLFWwndconfig *wndconfig = cs->lpCreateParams;

                    // On per-monitor DPI aware V1 systems, only enable
                    // non-client scaling for windows that scale the client area
                    // We need WM_GETDPISCALEDSIZE from V2 to keep the client
                    // area static when the non-client area is scaled
                    if (wndconfig && wndconfig->scaleToMonitor) EnableNonClientDpiScaling(hWnd);
                }

                break;
            }

            case WM_DISPLAYCHANGE:
                _glfwPollMonitorsWin32();
                _glfwVSyncDisplayChangedWin32();
                break;

            case _GLFW_WM_VBLANK:
                _glfwDispatchVSyncWin32(wParam);
                dispatchModalEvents();
                return 0;

            case WM_TIMER:
                if (wParam == KITTY_MODAL_TIMER) {
                    dispatchModalEvents();
                    return 0;
                }
                break;

            case WM_KITTY_TIMER_CHECK:
            case WM_KITTY_WAKEUP:
            case WM_NULL:
                _glfw.win32.wakeup_pending = true;
                dispatchModalEvents();
                break;

            case WM_SETTINGCHANGE:
                if (lParam && wcscmp((const WCHAR *)lParam, L"ImmersiveColorSet") == 0) _glfwInputColorScheme(glfwGetCurrentSystemColorTheme(true), false);
                break;
        }

        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    if (window->win32.motion.pending && uMsg != WM_MOUSEMOVE && isInputMessage(uMsg)) flushCursorPos(window);

    switch (uMsg) {
        case WM_MOUSEACTIVATE: {
            // HACK: Postpone cursor disabling when the window was activated by
            //       clicking a caption button
            if (HIWORD(lParam) == WM_LBUTTONDOWN) {
                if (LOWORD(lParam) != HTCLIENT) window->win32.frameAction = true;
            }

            break;
        }

        case WM_CAPTURECHANGED: {
            // HACK: Disable the cursor once the caption button action has been
            //       completed or cancelled
            if (lParam == 0 && window->win32.frameAction) {
                if (window->cursorMode == GLFW_CURSOR_DISABLED) disableCursor(window);

                window->win32.frameAction = false;
            }

            break;
        }

        case WM_SETFOCUS: {
            _glfwInputWindowFocus(window, true);

            // HACK: Do not disable cursor while the user is interacting with
            //       a caption button
            if (window->win32.frameAction) break;

            if (window->cursorMode == GLFW_CURSOR_DISABLED) disableCursor(window);

            return 0;
        }

        case WM_KILLFOCUS: {
            if (window->cursorMode == GLFW_CURSOR_DISABLED) enableCursor(window);

            if (window->monitor && window->autoIconify) _glfwPlatformIconifyWindow(window);

            _glfwInputWindowFocus(window, false);
            PostMessageW(hWnd, WM_KITTY_RESTORE_FOCUS, 0, 0);
            return 0;
        }

        case WM_KITTY_RESTORE_FOCUS: {
            if (isWSLgFocusWindow(GetForegroundWindow())) _glfwPlatformFocusWindow(window);
            return 0;
        }

        case WM_SYSCOMMAND: {
            switch (wParam & 0xfff0) {
                case SC_SCREENSAVE:
                case SC_MONITORPOWER: {
                    if (window->monitor) {
                        // We are running in full screen mode, so disallow
                        // screen saver and screen blanking
                        return 0;
                    } else break;
                }

                // User trying to access application menu using ALT?
                case SC_KEYMENU: return 0;
            }
            break;
        }

        case WM_CLOSE: {
            _glfwInputWindowCloseRequest(window);
            return 0;
        }

        case WM_INPUTLANGCHANGE: {
            _glfwUpdateKeyNamesWin32();
            break;
        }

        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_UNICHAR: {
            // Text that did not arrive as part of a key down message (for
            // example from an IME that does not use WM_IME_COMPOSITION)
            if (uMsg == WM_UNICHAR && wParam == UNICODE_NOCHAR) return TRUE;
            uint32_t codepoint = 0;
            if (uMsg != WM_UNICHAR && wParam >= 0xd800 && wParam <= 0xdbff) {
                window->win32.highSurrogate = (WCHAR)wParam;
                return 0;
            }
            if (uMsg != WM_UNICHAR && wParam >= 0xdc00 && wParam <= 0xdfff) {
                if (window->win32.highSurrogate) codepoint = decode_utf16_pair(window->win32.highSurrogate, (WCHAR)wParam);
            } else codepoint = (uint32_t)wParam;
            window->win32.highSurrogate = 0;
            if (codepoint >= 32 && codepoint != 127) {
                char text[8] = {0};
                size_t len = 0;
                encode_utf8(codepoint, text, &len);
                send_ime_text(window, GLFW_IME_COMMIT_TEXT, text);
            }
            return 0;
        }

        case WM_DEADCHAR:
        case WM_SYSDEADCHAR: return 0;

        case WM_IME_STARTCOMPOSITION: window->win32.ime.preedit_active = true; return 0;

        case WM_IME_COMPOSITION: handle_ime_composition(window, lParam); return 0;

        case WM_IME_ENDCOMPOSITION:
            if (window->win32.ime.preedit_active) {
                send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, "");
                window->win32.ime.preedit_active = false;
            }
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            handle_key_message(window, uMsg, wParam, lParam);
            // Let DefWindowProc handle Alt+F4 etc. but not the Alt menu
            if ((uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) && wParam == VK_F4) break;
            return 0;
        }

        case WM_KITTY_DROP_DATA: {
            if (window->win32.drop.active) {
                for (unsigned i = 0; i < 2; i++) {
                    if (window->win32.drop.data_requested[i]) {
                        window->win32.drop.data_requested[i] = false;
                        const char *mimes[1] = {drop_mimes[i]};
                        _glfwInputDropEvent(window, GLFW_DROP_DATA_AVAILABLE, 0, 0, mimes, 1, false);
                    }
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            int i, button, action;

            if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP) button = GLFW_MOUSE_BUTTON_LEFT;
            else if (uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP) button = GLFW_MOUSE_BUTTON_RIGHT;
            else if (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP) button = GLFW_MOUSE_BUTTON_MIDDLE;
            else if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) button = GLFW_MOUSE_BUTTON_4;
            else button = GLFW_MOUSE_BUTTON_5;

            if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN) {
                action = GLFW_PRESS;
            } else action = GLFW_RELEASE;

            for (i = 0; i <= GLFW_MOUSE_BUTTON_LAST; i++) {
                if (window->mouseButtons[i] == GLFW_PRESS) break;
            }

            if (i > GLFW_MOUSE_BUTTON_LAST) SetCapture(hWnd);

            _glfwInputMouseClick(window, button, action, getKeyMods());

            for (i = 0; i <= GLFW_MOUSE_BUTTON_LAST; i++) {
                if (window->mouseButtons[i] == GLFW_PRESS) break;
            }

            if (i > GLFW_MOUSE_BUTTON_LAST) ReleaseCapture();

            if (uMsg == WM_XBUTTONDOWN || uMsg == WM_XBUTTONUP) return TRUE;

            return 0;
        }

        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);

            if (!window->win32.cursorTracked) {
                TRACKMOUSEEVENT tme;
                ZeroMemory(&tme, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = window->win32.handle;
                TrackMouseEvent(&tme);

                window->win32.cursorTracked = true;
                _glfwInputCursorEnter(window, true);
            }

            if (window->cursorMode == GLFW_CURSOR_DISABLED) {
                const int dx = x - window->win32.lastCursorPosX;
                const int dy = y - window->win32.lastCursorPosY;

                if (_glfw.win32.disabledCursorWindow != window) break;
                if (window->rawMouseMotion) break;

                _glfwInputCursorPos(window, window->virtualCursorPosX + dx, window->virtualCursorPosY + dy);
            } else coalesceCursorPos(window, x, y);

            window->win32.lastCursorPosX = x;
            window->win32.lastCursorPosY = y;

            return 0;
        }

        case WM_INPUT: {
            UINT size = 0;
            HRAWINPUT ri = (HRAWINPUT)lParam;
            RAWINPUT *data = NULL;
            int dx, dy;

            if (_glfw.win32.disabledCursorWindow != window) break;
            if (!window->rawMouseMotion) break;

            GetRawInputData(ri, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
            if (size > (UINT)_glfw.win32.rawInputSize) {
                free(_glfw.win32.rawInput);
                _glfw.win32.rawInput = calloc(size, 1);
                _glfw.win32.rawInputSize = size;
            }

            size = _glfw.win32.rawInputSize;
            if (GetRawInputData(ri, RID_INPUT, _glfw.win32.rawInput, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
                _glfwInputError(GLFW_PLATFORM_ERROR, "Win32: Failed to retrieve raw input data");
                break;
            }

            data = _glfw.win32.rawInput;
            if (data->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                dx = data->data.mouse.lLastX - window->win32.lastCursorPosX;
                dy = data->data.mouse.lLastY - window->win32.lastCursorPosY;
            } else {
                dx = data->data.mouse.lLastX;
                dy = data->data.mouse.lLastY;
            }

            _glfwInputCursorPos(window, window->virtualCursorPosX + dx, window->virtualCursorPosY + dy);

            window->win32.lastCursorPosX += dx;
            window->win32.lastCursorPosY += dy;
            break;
        }

        case WM_MOUSELEAVE: {
            window->win32.cursorTracked = false;
            _glfwInputCursorEnter(window, false);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            send_scroll(window, 0.0, (SHORT)HIWORD(wParam));
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            // This message is only sent on Windows Vista and later
            // NOTE: The X-axis is inverted for consistency with macOS and X11
            send_scroll(window, -((SHORT)HIWORD(wParam)), 0.0);
            return 0;
        }

        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP: {
            enterModalLoop(window);
            if (uMsg == WM_ENTERSIZEMOVE) window->win32.size_move_in_progress = true;
            if (window->win32.frameAction) break;

            // HACK: Enable the cursor while the user is moving or
            //       resizing the window or using the window menu
            if (window->cursorMode == GLFW_CURSOR_DISABLED) enableCursor(window);

            break;
        }

        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP: {
            leaveModalLoop(window);
            if (uMsg == WM_EXITSIZEMOVE) {
                window->win32.size_move_in_progress = false;
                if (window->win32.live_resize_in_progress) {
                    window->win32.live_resize_in_progress = false;
                    _glfwInputLiveResize(window, false);
                }
            }
            if (window->win32.frameAction) break;

            // HACK: Disable the cursor once the user is done moving or
            //       resizing the window or using the menu
            if (window->cursorMode == GLFW_CURSOR_DISABLED) disableCursor(window);

            break;
        }

        case WM_SIZE: {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            const bool iconified = wParam == SIZE_MINIMIZED;
            const bool maximized = wParam == SIZE_MAXIMIZED || (window->win32.maximized && wParam != SIZE_RESTORED);

            if (_glfw.win32.capturedCursorWindow == window) captureCursor(window);

            if (window->win32.iconified != iconified) _glfwInputWindowIconify(window, iconified);

            if (window->win32.maximized != maximized) _glfwInputWindowMaximize(window, maximized);

            if (width != window->win32.width || height != window->win32.height) {
                window->win32.width = width;
                window->win32.height = height;

                _glfwInputFramebufferSize(window, width, height);
                _glfwInputWindowSize(window, width, height);
            }

            if (window->monitor && window->win32.iconified != iconified) {
                if (iconified) releaseMonitor(window);
                else {
                    acquireMonitor(window);
                    fitToMonitor(window);
                }
            }

            window->win32.iconified = iconified;
            window->win32.maximized = maximized;
            dispatchModalEvents();
            return 0;
        }

        case WM_MOVE: {
            if (_glfw.win32.capturedCursorWindow == window) captureCursor(window);

            // NOTE: This cannot use LOWORD/HIWORD recommended by MSDN, as
            // those macros do not handle negative window positions correctly
            _glfwInputWindowPos(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }

        case WM_SIZING: {
            if (window->win32.size_move_in_progress && !window->win32.live_resize_in_progress) {
                window->win32.live_resize_in_progress = true;
                _glfwInputLiveResize(window, true);
            }
            if (window->numer == GLFW_DONT_CARE || window->denom == GLFW_DONT_CARE) { break; }

            applyAspectRatio(window, (int)wParam, (RECT *)lParam);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            RECT frame = {0};
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            const DWORD style = getWindowStyle(window);
            const DWORD exStyle = getWindowExStyle(window);

            if (window->monitor) break;

            if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
                AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, GetDpiForWindow(window->win32.handle));
            } else AdjustWindowRectEx(&frame, style, FALSE, exStyle);

            if (window->minwidth != GLFW_DONT_CARE && window->minheight != GLFW_DONT_CARE) {
                mmi->ptMinTrackSize.x = window->minwidth + frame.right - frame.left;
                mmi->ptMinTrackSize.y = window->minheight + frame.bottom - frame.top;
            }

            if (window->maxwidth != GLFW_DONT_CARE && window->maxheight != GLFW_DONT_CARE) {
                mmi->ptMaxTrackSize.x = window->maxwidth + frame.right - frame.left;
                mmi->ptMaxTrackSize.y = window->maxheight + frame.bottom - frame.top;
            }

            if (!window->decorated) {
                MONITORINFO mi;
                const HMONITOR mh = MonitorFromWindow(window->win32.handle, MONITOR_DEFAULTTONEAREST);

                ZeroMemory(&mi, sizeof(mi));
                mi.cbSize = sizeof(mi);
                GetMonitorInfoW(mh, &mi);

                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }

            return 0;
        }

        case WM_PAINT: {
            _glfwInputWindowDamage(window);
            const LRESULT result = DefWindowProcW(hWnd, uMsg, wParam, lParam);
            dispatchModalEvents();
            return result;
        }

        case WM_ERASEBKGND: {
            return TRUE;
        }

        case WM_NCACTIVATE:
        case WM_NCPAINT: {
            // Prevent title bar from being drawn after restoring a minimized
            // undecorated window
            if (!window->decorated) return TRUE;

            break;
        }

        case WM_DWMCOMPOSITIONCHANGED:
        case WM_DWMCOLORIZATIONCOLORCHANGED: {
            if (window->win32.transparent) updateFramebufferTransparency(window);
            return 0;
        }

        case WM_GETDPISCALEDSIZE: {
            if (window->win32.scaleToMonitor) break;

            // Adjust the window size to keep the content area size constant
            if (_glfwIsWindows10CreatorsUpdateOrGreaterWin32()) {
                RECT source = {0}, target = {0};
                SIZE *size = (SIZE *)lParam;

                AdjustWindowRectExForDpi(&source, getWindowStyle(window), FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
                AdjustWindowRectExForDpi(&target, getWindowStyle(window), FALSE, getWindowExStyle(window), LOWORD(wParam));

                size->cx += (target.right - target.left) - (source.right - source.left);
                size->cy += (target.bottom - target.top) - (source.bottom - source.top);
                return TRUE;
            }

            break;
        }

        case WM_DPICHANGED: {
            const float xscale = HIWORD(wParam) / (float)USER_DEFAULT_SCREEN_DPI;
            const float yscale = LOWORD(wParam) / (float)USER_DEFAULT_SCREEN_DPI;

            // Resize windowed mode windows that either permit rescaling or that
            // need it to compensate for non-client area scaling
            if (!window->monitor && (window->win32.scaleToMonitor || _glfwIsWindows10CreatorsUpdateOrGreaterWin32())) {
                RECT *suggested = (RECT *)lParam;
                SetWindowPos(
                    window->win32.handle,
                    HWND_TOP,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }

            _glfwInputWindowContentScale(window, xscale, yscale);
            break;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                updateCursorImage(window);
                return TRUE;
            }

            break;
        }

        case WM_DROPFILES: {
            handle_drop_files(window, (HDROP)wParam);
            return 0;
        }

        case WM_SETTINGCHANGE: {
            if (lParam && wcscmp((const WCHAR *)lParam, L"ImmersiveColorSet") == 0) _glfwInputColorScheme(glfwGetCurrentSystemColorTheme(true), false);
            break;
        }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Creates the GLFW window
//
static int
createNativeWindow(_GLFWwindow *window, const _GLFWwndconfig *wndconfig, const _GLFWfbconfig *fbconfig) {
    int frameX, frameY, frameWidth, frameHeight;
    WCHAR *wideTitle;
    DWORD style = getWindowStyle(window);
    DWORD exStyle = getWindowExStyle(window);

    if (window->monitor) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(window->monitor->win32.handle, &mi);

        // NOTE: This window placement is temporary and approximate, as the
        //       correct position and size cannot be known until the monitor
        //       video mode has been picked in _glfwSetVideoModeWin32
        frameX = mi.rcMonitor.left;
        frameY = mi.rcMonitor.top;
        frameWidth = mi.rcMonitor.right - mi.rcMonitor.left;
        frameHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else {
        RECT rect = {0, 0, wndconfig->width, wndconfig->height};

        window->win32.maximized = wndconfig->maximized;
        if (wndconfig->maximized) style |= WS_MAXIMIZE;

        AdjustWindowRectEx(&rect, style, FALSE, exStyle);

        frameX = CW_USEDEFAULT;
        frameY = CW_USEDEFAULT;
        frameWidth = rect.right - rect.left;
        frameHeight = rect.bottom - rect.top;
        if (wndconfig->scaleToMonitor) {
            const HWND active = GetActiveWindow();
            POINT cursor;
            HMONITOR monitor = NULL;
            if (active) monitor = MonitorFromWindow(active, MONITOR_DEFAULTTONEAREST);
            else if (GetCursorPos(&cursor)) monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            if (monitor && GetMonitorInfoW(monitor, &mi)) {
                frameX = mi.rcWork.left;
                frameY = mi.rcWork.top;
            }
        }
    }

    wideTitle = _glfwCreateWideStringFromUTF8Win32(wndconfig->title);
    if (!wideTitle) return false;

    window->win32.handle = CreateWindowExW(
        exStyle,
        _GLFW_WNDCLASSNAME,
        wideTitle,
        style,
        frameX,
        frameY,
        frameWidth,
        frameHeight,
        NULL, // No parent window
        NULL, // No window menu
        _glfw.win32.instance,
        (LPVOID)wndconfig);

    free(wideTitle);

    if (!window->win32.handle) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create window");
        return false;
    }

    SetPropW(window->win32.handle, L"GLFW", window);

    if (IsWindows7OrGreater()) {
        ChangeWindowMessageFilterEx(window->win32.handle, WM_DROPFILES, MSGFLT_ALLOW, NULL);
        ChangeWindowMessageFilterEx(window->win32.handle, WM_COPYDATA, MSGFLT_ALLOW, NULL);
        ChangeWindowMessageFilterEx(window->win32.handle, WM_COPYGLOBALDATA, MSGFLT_ALLOW, NULL);
    }

    window->win32.scaleToMonitor = wndconfig->scaleToMonitor;

    if (!window->monitor) {
        RECT rect = {0, 0, wndconfig->width, wndconfig->height};
        WINDOWPLACEMENT wp = {sizeof(wp)};
        const HMONITOR mh = MonitorFromWindow(window->win32.handle, MONITOR_DEFAULTTONEAREST);

        // Adjust window rect to account for DPI scaling of the window frame and
        // (if enabled) DPI scaling of the content area
        // This cannot be done until we know what monitor the window was placed on
        // Only update the restored window rect as the window may be maximized

        if (wndconfig->scaleToMonitor) {
            float xscale, yscale;
            _glfwPlatformGetWindowContentScale(window, &xscale, &yscale);

            if (xscale > 0.f && yscale > 0.f) {
                rect.right = (int)(rect.right * xscale);
                rect.bottom = (int)(rect.bottom * yscale);
            }
        }

        if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
            AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, GetDpiForWindow(window->win32.handle));
        } else AdjustWindowRectEx(&rect, style, FALSE, exStyle);

        GetWindowPlacement(window->win32.handle, &wp);
        OffsetRect(&rect, wp.rcNormalPosition.left - rect.left, wp.rcNormalPosition.top - rect.top);

        wp.rcNormalPosition = rect;
        wp.showCmd = SW_HIDE;
        SetWindowPlacement(window->win32.handle, &wp);

        // Adjust rect of maximized undecorated window, because by default Windows will
        // make such a window cover the whole monitor instead of its workarea

        if (wndconfig->maximized && !wndconfig->decorated) {
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoW(mh, &mi);

            SetWindowPos(
                window->win32.handle,
                HWND_TOP,
                mi.rcWork.left,
                mi.rcWork.top,
                mi.rcWork.right - mi.rcWork.left,
                mi.rcWork.bottom - mi.rcWork.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }

    DragAcceptFiles(window->win32.handle, TRUE);

    applyTitlebarTheme(window, query_system_color_theme());

    if (fbconfig->transparent) {
        updateFramebufferTransparency(window);
        window->win32.transparent = true;
    }

    _glfwPlatformGetWindowSize(window, &window->win32.width, &window->win32.height);

    return true;
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW internal API                      //////
//////////////////////////////////////////////////////////////////////////

// Registers the GLFW window class
//
bool
_glfwRegisterWindowClassWin32(void) {
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = _glfw.win32.instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = _GLFW_WNDCLASSNAME;

    // Load user-provided icon if available
    wc.hIcon = LoadImageW(GetModuleHandleW(NULL), L"GLFW_ICON", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!wc.hIcon) {
        // No user-provided icon found, load default icon
        wc.hIcon = LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    }

    if (!RegisterClassExW(&wc)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to register window class");
        return false;
    }

    return true;
}

// Unregisters the GLFW window class
//
void
_glfwUnregisterWindowClassWin32(void) {
    UnregisterClassW(_GLFW_WNDCLASSNAME, _glfw.win32.instance);
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

int
_glfwPlatformCreateWindow(
    _GLFWwindow *window, const _GLFWwndconfig *wndconfig, const _GLFWctxconfig *ctxconfig, const _GLFWfbconfig *fbconfig, const GLFWLayerShellConfig *lsc) {
    if (!createNativeWindow(window, wndconfig, fbconfig)) return false;
    if (lsc) window->win32.layer_shell_config = *lsc;
    window->win32.blur_mode = wndconfig->win32.blur_mode;
    if (wndconfig->blur_radius > 0) _glfwPlatformSetWindowBlur(window, wndconfig->blur_radius);

    if (ctxconfig->client != GLFW_NO_API) {
        if (ctxconfig->source == GLFW_NATIVE_CONTEXT_API) {
            if (!_glfwInitWGL()) return false;
            if (!_glfwCreateContextWGL(window, ctxconfig, fbconfig)) return false;
        } else if (ctxconfig->source == GLFW_EGL_CONTEXT_API) {
            if (!_glfwInitEGL()) return false;
            if (!_glfwCreateContextEGL(window, ctxconfig, fbconfig)) return false;
        } else if (ctxconfig->source == GLFW_OSMESA_CONTEXT_API) {
            if (!_glfwInitOSMesa()) return false;
            if (!_glfwCreateContextOSMesa(window, ctxconfig, fbconfig)) return false;
        }

        if (!_glfwRefreshContextAttribs(window, ctxconfig)) return false;
    }

    if (window->monitor) {
        _glfwPlatformShowWindow(window, false);
        _glfwPlatformFocusWindow(window);
        acquireMonitor(window);
        fitToMonitor(window);

        if (wndconfig->centerCursor) _glfwCenterCursorInContentArea(window);
    } else {
        if (wndconfig->visible) {
            _glfwPlatformShowWindow(window, false);
            if (wndconfig->focused) _glfwPlatformFocusWindow(window);
        }
    }

    return true;
}

void
_glfwPlatformDestroyWindow(_GLFWwindow *window) {
    while (window->win32.modal_loop_depth) leaveModalLoop(window);
    free_drop_data(window);
    if (window->monitor) releaseMonitor(window);

    if (window->context.destroy) window->context.destroy(window);

    if (_glfw.win32.disabledCursorWindow == window) enableCursor(window);

    if (_glfw.win32.capturedCursorWindow == window) releaseCursor();

    if (window->win32.handle) {
        RemovePropW(window->win32.handle, L"GLFW");
        DestroyWindow(window->win32.handle);
        window->win32.handle = NULL;
    }

    if (window->win32.bigIcon) DestroyIcon(window->win32.bigIcon);

    if (window->win32.smallIcon) DestroyIcon(window->win32.smallIcon);
}

void
_glfwPlatformSetWindowTitle(_GLFWwindow *window, const char *title) {
    WCHAR *wideTitle = _glfwCreateWideStringFromUTF8Win32(title);
    if (!wideTitle) return;

    SetWindowTextW(window->win32.handle, wideTitle);
    free(wideTitle);
}

void
_glfwPlatformSetWindowIcon(_GLFWwindow *window, int count, const GLFWimage *images) {
    HICON bigIcon = NULL, smallIcon = NULL;

    if (count) {
        const GLFWimage *bigImage = chooseImage(count, images, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        const GLFWimage *smallImage = chooseImage(count, images, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

        bigIcon = createIcon(bigImage, 0, 0, true);
        smallIcon = createIcon(smallImage, 0, 0, true);
    } else {
        bigIcon = (HICON)GetClassLongPtrW(window->win32.handle, GCLP_HICON);
        smallIcon = (HICON)GetClassLongPtrW(window->win32.handle, GCLP_HICONSM);
    }

    SendMessageW(window->win32.handle, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    SendMessageW(window->win32.handle, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);

    if (window->win32.bigIcon) DestroyIcon(window->win32.bigIcon);

    if (window->win32.smallIcon) DestroyIcon(window->win32.smallIcon);

    if (count) {
        window->win32.bigIcon = bigIcon;
        window->win32.smallIcon = smallIcon;
    }
}

void
_glfwPlatformGetWindowPos(_GLFWwindow *window, int *xpos, int *ypos) {
    POINT pos = {0, 0};
    ClientToScreen(window->win32.handle, &pos);

    if (xpos) *xpos = pos.x;
    if (ypos) *ypos = pos.y;
}

void
_glfwPlatformSetWindowPos(_GLFWwindow *window, int xpos, int ypos) {
    RECT rect = {xpos, ypos, xpos, ypos};

    if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
        AdjustWindowRectExForDpi(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
    } else {
        AdjustWindowRectEx(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window));
    }

    SetWindowPos(window->win32.handle, NULL, rect.left, rect.top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
}

void
_glfwPlatformGetWindowSize(_GLFWwindow *window, int *width, int *height) {
    RECT area;
    GetClientRect(window->win32.handle, &area);

    if (width) *width = area.right;
    if (height) *height = area.bottom;
}

void
_glfwPlatformSetWindowSize(_GLFWwindow *window, int width, int height) {
    if (window->monitor) {
        if (window->monitor->window == window) {
            acquireMonitor(window);
            fitToMonitor(window);
        }
    } else {
        RECT rect = {0, 0, width, height};

        if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
            AdjustWindowRectExForDpi(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
        } else {
            AdjustWindowRectEx(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window));
        }

        SetWindowPos(
            window->win32.handle,
            HWND_TOP,
            0,
            0,
            rect.right - rect.left,
            rect.bottom - rect.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOZORDER);
    }
}

void
_glfwPlatformSetWindowSizeLimits(_GLFWwindow *window, int minwidth, int minheight, int maxwidth, int maxheight) {
    RECT area;

    if ((minwidth == GLFW_DONT_CARE || minheight == GLFW_DONT_CARE) && (maxwidth == GLFW_DONT_CARE || maxheight == GLFW_DONT_CARE)) { return; }

    GetWindowRect(window->win32.handle, &area);
    MoveWindow(window->win32.handle, area.left, area.top, area.right - area.left, area.bottom - area.top, TRUE);
}

void
_glfwPlatformSetWindowAspectRatio(_GLFWwindow *window, int numer, int denom) {
    RECT area;

    if (numer == GLFW_DONT_CARE || denom == GLFW_DONT_CARE) return;

    GetWindowRect(window->win32.handle, &area);
    applyAspectRatio(window, WMSZ_BOTTOMRIGHT, &area);
    MoveWindow(window->win32.handle, area.left, area.top, area.right - area.left, area.bottom - area.top, TRUE);
}

void
_glfwPlatformGetFramebufferSize(_GLFWwindow *window, int *width, int *height) {
    _glfwPlatformGetWindowSize(window, width, height);
}

void
_glfwPlatformGetWindowFrameSize(_GLFWwindow *window, int *left, int *top, int *right, int *bottom) {
    RECT rect;
    int width, height;

    _glfwPlatformGetWindowSize(window, &width, &height);
    SetRect(&rect, 0, 0, width, height);

    if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
        AdjustWindowRectExForDpi(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
    } else {
        AdjustWindowRectEx(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window));
    }

    if (left) *left = -rect.left;
    if (top) *top = -rect.top;
    if (right) *right = rect.right - width;
    if (bottom) *bottom = rect.bottom - height;
}

void
_glfwPlatformGetWindowContentScale(_GLFWwindow *window, float *xscale, float *yscale) {
    if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
        const UINT dpi = GetDpiForWindow(window->win32.handle);
        if (dpi) {
            *xscale = *yscale = dpi / (float)USER_DEFAULT_SCREEN_DPI;
            return;
        }
    }
    const HANDLE handle = MonitorFromWindow(window->win32.handle, MONITOR_DEFAULTTONEAREST);
    _glfwGetMonitorContentScaleWin32(handle, xscale, yscale);
}

void
_glfwPlatformIconifyWindow(_GLFWwindow *window) {
    ShowWindow(window->win32.handle, SW_MINIMIZE);
}

void
_glfwPlatformRestoreWindow(_GLFWwindow *window) {
    ShowWindow(window->win32.handle, SW_RESTORE);
}

void
_glfwPlatformMaximizeWindow(_GLFWwindow *window) {
    if (IsWindowVisible(window->win32.handle)) ShowWindow(window->win32.handle, SW_MAXIMIZE);
    else maximizeWindowManually(window);
}

void
_glfwPlatformShowWindow(_GLFWwindow *window, bool move_to_active_screen UNUSED) {
    ShowWindow(window->win32.handle, SW_SHOWNA);
}

void
_glfwPlatformHideWindow(_GLFWwindow *window) {
    ShowWindow(window->win32.handle, SW_HIDE);
}

static void
flashWindow(_GLFWwindow *window) {
    // FLASHW_TIMERNOFG flashes until the window is activated
    FLASHWINFO fi = {.cbSize = sizeof(fi), .hwnd = window->win32.handle, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
    FlashWindowEx(&fi);
}

void
_glfwPlatformRequestWindowAttention(_GLFWwindow *window) {
    flashWindow(window);
}

void
_glfwPlatformFocusWindow(_GLFWwindow *window) {
    BringWindowToTop(window->win32.handle);
    // Windows refuses this unless we are allowed to take the foreground, for
    // example via AllowSetForegroundWindow() in the process requesting focus
    if (!SetForegroundWindow(window->win32.handle)) flashWindow(window);
    SetFocus(window->win32.handle);
}

void
_glfwPlatformSetWindowMonitor(_GLFWwindow *window, _GLFWmonitor *monitor, int xpos, int ypos, int width, int height, int refreshRate UNUSED) {
    if (window->monitor == monitor) {
        if (monitor) {
            if (monitor->window == window) {
                acquireMonitor(window);
                fitToMonitor(window);
            }
        } else {
            RECT rect = {xpos, ypos, xpos + width, ypos + height};

            if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
                AdjustWindowRectExForDpi(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
            } else {
                AdjustWindowRectEx(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window));
            }

            SetWindowPos(
                window->win32.handle,
                HWND_TOP,
                rect.left,
                rect.top,
                rect.right - rect.left,
                rect.bottom - rect.top,
                SWP_NOCOPYBITS | SWP_NOACTIVATE | SWP_NOZORDER);
        }

        return;
    }

    if (window->monitor) releaseMonitor(window);

    _glfwInputWindowMonitor(window, monitor);

    if (window->monitor) {
        MONITORINFO mi = {sizeof(mi)};
        UINT flags = SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOCOPYBITS;

        if (window->decorated) {
            DWORD style = GetWindowLongW(window->win32.handle, GWL_STYLE);
            style &= ~WS_OVERLAPPEDWINDOW;
            style |= getWindowStyle(window);
            SetWindowLongW(window->win32.handle, GWL_STYLE, style);
            flags |= SWP_FRAMECHANGED;
        }

        acquireMonitor(window);

        GetMonitorInfoW(window->monitor->win32.handle, &mi);
        SetWindowPos(
            window->win32.handle,
            HWND_TOPMOST,
            mi.rcMonitor.left,
            mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            flags);
    } else {
        HWND after;
        RECT rect = {xpos, ypos, xpos + width, ypos + height};
        DWORD style = GetWindowLongW(window->win32.handle, GWL_STYLE);
        UINT flags = SWP_NOACTIVATE | SWP_NOCOPYBITS;

        if (window->decorated) {
            style &= ~WS_POPUP;
            style |= getWindowStyle(window);
            SetWindowLongW(window->win32.handle, GWL_STYLE, style);

            flags |= SWP_FRAMECHANGED;
        }

        if (window->floating) after = HWND_TOPMOST;
        else after = HWND_NOTOPMOST;

        if (_glfwIsWindows10AnniversaryUpdateOrGreaterWin32()) {
            AdjustWindowRectExForDpi(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window), GetDpiForWindow(window->win32.handle));
        } else {
            AdjustWindowRectEx(&rect, getWindowStyle(window), FALSE, getWindowExStyle(window));
        }

        SetWindowPos(window->win32.handle, after, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
    }
}

int
_glfwPlatformWindowFocused(_GLFWwindow *window) {
    return window->win32.handle == GetActiveWindow();
}

int
_glfwPlatformWindowIconified(_GLFWwindow *window) {
    return IsIconic(window->win32.handle);
}

int
_glfwPlatformWindowVisible(_GLFWwindow *window) {
    return IsWindowVisible(window->win32.handle);
}

int
_glfwPlatformWindowMaximized(_GLFWwindow *window) {
    return IsZoomed(window->win32.handle);
}

int
_glfwPlatformWindowHovered(_GLFWwindow *window) {
    return cursorInContentArea(window);
}

int
_glfwPlatformFramebufferTransparent(_GLFWwindow *window) {
    BOOL composition, opaque;
    DWORD color;

    if (!window->win32.transparent) return false;

    if (!IsWindowsVistaOrGreater()) return false;

    if (FAILED(DwmIsCompositionEnabled(&composition)) || !composition) return false;

    if (!IsWindows8OrGreater()) {
        // HACK: Disable framebuffer transparency on Windows 7 when the
        //       colorization color is opaque, because otherwise the window
        //       contents is blended additively with the previous frame instead
        //       of replacing it
        if (FAILED(DwmGetColorizationColor(&color, &opaque)) || opaque) return false;
    }

    return true;
}

void
_glfwPlatformSetWindowResizable(_GLFWwindow *window, bool enabled UNUSED) {
    updateWindowStyles(window);
}

void
_glfwPlatformSetWindowDecorated(_GLFWwindow *window, bool enabled UNUSED) {
    updateWindowStyles(window);
}

void
_glfwPlatformSetWindowFloating(_GLFWwindow *window, bool enabled) {
    const HWND after = enabled ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(window->win32.handle, after, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
}

float
_glfwPlatformGetWindowOpacity(_GLFWwindow *window) {
    BYTE alpha;
    DWORD flags;

    if ((GetWindowLongW(window->win32.handle, GWL_EXSTYLE) & WS_EX_LAYERED) && GetLayeredWindowAttributes(window->win32.handle, NULL, &alpha, &flags)) {
        if (flags & LWA_ALPHA) return alpha / 255.f;
    }

    return 1.f;
}

void
_glfwPlatformSetWindowOpacity(_GLFWwindow *window, float opacity) {
    if (opacity < 1.f) {
        const BYTE alpha = (BYTE)(255 * opacity);
        DWORD style = GetWindowLongW(window->win32.handle, GWL_EXSTYLE);
        style |= WS_EX_LAYERED;
        SetWindowLongW(window->win32.handle, GWL_EXSTYLE, style);
        SetLayeredWindowAttributes(window->win32.handle, 0, alpha, LWA_ALPHA);
    } else {
        DWORD style = GetWindowLongW(window->win32.handle, GWL_EXSTYLE);
        style &= ~WS_EX_LAYERED;
        SetWindowLongW(window->win32.handle, GWL_EXSTYLE, style);
    }
}

void
_glfwPlatformSetRawMouseMotion(_GLFWwindow *window, bool enabled) {
    if (_glfw.win32.disabledCursorWindow != window) return;

    if (enabled) enableRawMouseMotion(window);
    else disableRawMouseMotion(window);
}

bool
_glfwPlatformRawMouseMotionSupported(void) {
    return true;
}

void
_glfwPlatformPollEvents(void) {
    MSG msg;
    HWND handle;
    _GLFWwindow *window;
    // A mouse with a very high polling rate can keep the queue from ever running dry, which
    // would hold off timers and the main loop tick, and so rendering, until it stops moving.
    // Leave the rest for the next loop iteration, which does not wait while input is queued.
    const monotonic_t drain_until = monotonic() + ms_to_monotonic_t(2ll);

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            // NOTE: While GLFW does not itself post WM_QUIT, other processes
            //       may post it to this one, for example Task Manager
            // HACK: Treat WM_QUIT as a close on all windows

            window = _glfw.windowListHead;
            while (window) {
                _glfwInputWindowCloseRequest(window);
                window = window->next;
            }
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (monotonic() >= drain_until) break;
    }

    // HACK: Release modifier keys that the system did not emit KEYUP for
    // NOTE: Shift keys on Windows tend to "stick" when both are pressed as
    //       no key up message is generated by the first key release
    // NOTE: Windows key is not reported as released by the Win+V hotkey
    //       Other Win hotkeys are handled implicitly by _glfwInputWindowFocus
    //       because they change the input focus
    // NOTE: The other half of this is in the WM_*KEY* handler in windowProc
    handle = GetActiveWindow();
    if (handle) {
        window = GetPropW(handle, L"GLFW");
        if (window) {
            const struct {
                int vk;
                uint32_t key;
                int scancode;
            } keys[4] = {
                {VK_LSHIFT, GLFW_FKEY_LEFT_SHIFT, 0x2a},
                {VK_RSHIFT, GLFW_FKEY_RIGHT_SHIFT, 0x36},
                {VK_LWIN, GLFW_FKEY_LEFT_SUPER, 0x15b},
                {VK_RWIN, GLFW_FKEY_RIGHT_SUPER, 0x15c}};

            for (int i = 0; i < 4; i++) {
                if ((GetKeyState(keys[i].vk) & 0x8000)) continue;
                bool pressed = false;
                for (unsigned k = 0; k < arraysz(window->activated_keys); k++) {
                    if (window->activated_keys[k].key == keys[i].key && window->activated_keys[k].action != GLFW_RELEASE) {
                        pressed = true;
                        break;
                    }
                }
                if (!pressed) continue;

                GLFWkeyevent ev = {
                    .key = keys[i].key, .native_key = keys[i].scancode, .native_key_id = keys[i].scancode, .action = GLFW_RELEASE, .mods = getKeyMods()};
                ev.win32_vk = keys[i].vk == VK_LSHIFT || keys[i].vk == VK_RSHIFT ? VK_SHIFT : keys[i].vk;
                ev.win32_control_state = win32_control_key_state((keys[i].scancode & 0x100) != 0);
                _glfwInputKeyboard(window, &ev);
            }
        }
    }

    window = _glfw.win32.disabledCursorWindow;
    if (window) {
        int width, height;
        _glfwPlatformGetWindowSize(window, &width, &height);

        // NOTE: Re-center the cursor only if it has moved since the last call,
        //       to avoid breaking glfwWaitEvents with WM_MOUSEMOVE
        if (window->win32.lastCursorPosX != width / 2 || window->win32.lastCursorPosY != height / 2) {
            _glfwPlatformSetCursorPos(window, width / 2, height / 2);
        }
    }
}

void
_glfwPlatformWaitEvents(void) {
    MsgWaitForMultipleObjectsEx(0, NULL, _glfwTimerWaitTimeoutWin32(), QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    _glfwPlatformPollEvents();
    _glfwDispatchTimersWin32();
}

// Rounds up, so that waiting for something due in under a millisecond does not
// become a zero timeout that spins the main loop until it is due
static DWORD
wait_timeout_ms(monotonic_t timeout) {
    return (DWORD)((timeout + MONOTONIC_T_1e6 - 1) / MONOTONIC_T_1e6);
}

void
_glfwPlatformWaitEventsTimeout(monotonic_t timeout) {
    DWORD ms = _glfwTimerWaitTimeoutWin32();
    if (timeout >= 0) {
        DWORD t = wait_timeout_ms(timeout);
        if (t < ms) ms = t;
    }
    MsgWaitForMultipleObjectsEx(0, NULL, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    _glfwPlatformPollEvents();
    _glfwDispatchTimersWin32();
}

void
_glfwPlatformPostEmptyEvent(void) {
    PostMessageW(_glfw.win32.helperWindowHandle, WM_KITTY_WAKEUP, 0, 0);
}

void
_glfwPlatformGetCursorPos(_GLFWwindow *window, double *xpos, double *ypos) {
    POINT pos;

    if (GetCursorPos(&pos)) {
        ScreenToClient(window->win32.handle, &pos);

        if (xpos) *xpos = pos.x;
        if (ypos) *ypos = pos.y;
    }
}

void
_glfwPlatformSetCursorPos(_GLFWwindow *window, double xpos, double ypos) {
    POINT pos = {(int)xpos, (int)ypos};

    // Store the new position so it can be recognized later
    window->win32.lastCursorPosX = pos.x;
    window->win32.lastCursorPosY = pos.y;

    ClientToScreen(window->win32.handle, &pos);
    SetCursorPos(pos.x, pos.y);
}

void
_glfwPlatformSetCursorMode(_GLFWwindow *window, int mode) {
    if (_glfwPlatformWindowFocused(window)) {
        if (mode == GLFW_CURSOR_DISABLED) {
            _glfwPlatformGetCursorPos(window, &_glfw.win32.restoreCursorPosX, &_glfw.win32.restoreCursorPosY);
            _glfwCenterCursorInContentArea(window);
            if (window->rawMouseMotion) enableRawMouseMotion(window);
        } else if (_glfw.win32.disabledCursorWindow == window) {
            if (window->rawMouseMotion) disableRawMouseMotion(window);
        }

        if (mode == GLFW_CURSOR_DISABLED) captureCursor(window);
        else releaseCursor();

        if (mode == GLFW_CURSOR_DISABLED) _glfw.win32.disabledCursorWindow = window;
        else if (_glfw.win32.disabledCursorWindow == window) {
            _glfw.win32.disabledCursorWindow = NULL;
            _glfwPlatformSetCursorPos(window, _glfw.win32.restoreCursorPosX, _glfw.win32.restoreCursorPosY);
        }
    }

    if (cursorInContentArea(window)) updateCursorImage(window);
}

const char *
_glfwPlatformGetNativeKeyName(int scancode) {
    if (scancode < 0 || scancode > (KF_EXTENDED | 0xff)) return NULL;
    if (_glfw.win32.keynames[scancode][0]) return _glfw.win32.keynames[scancode];
    const uint32_t key = _glfw.win32.keycodes[scancode];
    if (key >= GLFW_FKEY_FIRST) return _glfwGetKeyName(key);
    return NULL;
}

int
_glfwPlatformGetNativeKeyForKey(uint32_t key) {
    if (!key) return 0;
    if (key >= GLFW_FKEY_FIRST) {
        UINT vk = vk_for_functional_key(key);
        if (vk) {
            UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
            if (sc) return (int)((sc & 0xff) | ((sc & 0xe000) ? KF_EXTENDED : 0));
        }
    } else {
        // Search the current layout first
        for (int scancode = 1; scancode < 512; scancode++) {
            UINT vk = MapVirtualKeyW((UINT)scancode, MAPVK_VSC_TO_VK_EX);
            if (!vk) continue;
            if (to_lower_codepoint(vk_to_unicode(vk, (UINT)scancode, false, false)) == key) return scancode;
        }
    }
    for (int scancode = 0; scancode < 512; scancode++) {
        if (_glfw.win32.keycodes[scancode] == key) return scancode;
    }
    return 0;
}

int
_glfwPlatformCreateCursor(_GLFWcursor *cursor, const GLFWimage *image, int xhot, int yhot, int count UNUSED) {
    cursor->win32.handle = (HCURSOR)createIcon(image, xhot, yhot, false);
    if (!cursor->win32.handle) return false;

    return true;
}

int
_glfwPlatformCreateStandardCursor(_GLFWcursor *cursor, GLFWCursorShape shape) {
    int id = 0;

    switch (shape) {
        case GLFW_DEFAULT_CURSOR: id = OCR_NORMAL; break;
        case GLFW_TEXT_CURSOR: id = OCR_IBEAM; break;
        case GLFW_VERTICAL_TEXT_CURSOR: id = OCR_IBEAM; break;
        case GLFW_POINTER_CURSOR: id = OCR_HAND; break;
        case GLFW_HELP_CURSOR: id = 32651; break; // OCR_HELP
        case GLFW_WAIT_CURSOR: id = OCR_WAIT; break;
        case GLFW_PROGRESS_CURSOR: id = OCR_APPSTARTING; break;
        case GLFW_CROSSHAIR_CURSOR: id = OCR_CROSS; break;
        case GLFW_CELL_CURSOR: id = OCR_CROSS; break;
        case GLFW_MOVE_CURSOR: id = OCR_SIZEALL; break;
        case GLFW_E_RESIZE_CURSOR:
        case GLFW_W_RESIZE_CURSOR:
        case GLFW_EW_RESIZE_CURSOR: id = OCR_SIZEWE; break;
        case GLFW_N_RESIZE_CURSOR:
        case GLFW_S_RESIZE_CURSOR:
        case GLFW_NS_RESIZE_CURSOR: id = OCR_SIZENS; break;
        case GLFW_NE_RESIZE_CURSOR:
        case GLFW_SW_RESIZE_CURSOR:
        case GLFW_NESW_RESIZE_CURSOR: id = OCR_SIZENESW; break;
        case GLFW_NW_RESIZE_CURSOR:
        case GLFW_SE_RESIZE_CURSOR:
        case GLFW_NWSE_RESIZE_CURSOR: id = OCR_SIZENWSE; break;
        case GLFW_NOT_ALLOWED_CURSOR:
        case GLFW_NO_DROP_CURSOR: id = OCR_NO; break;
        case GLFW_GRAB_CURSOR:
        case GLFW_GRABBING_CURSOR: id = OCR_SIZEALL; break;
        case GLFW_ZOOM_IN_CURSOR:
        case GLFW_ZOOM_OUT_CURSOR:
        case GLFW_ALIAS_CURSOR:
        case GLFW_COPY_CURSOR: id = OCR_NORMAL; break;
        case GLFW_INVALID_CURSOR: return false;
    }
    if (!id) return false;

    cursor->win32.handle = LoadImageW(NULL, MAKEINTRESOURCEW(id), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!cursor->win32.handle) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create standard cursor");
        return false;
    }

    return true;
}

void
_glfwPlatformDestroyCursor(_GLFWcursor *cursor) {
    if (cursor->win32.handle) DestroyIcon((HICON)cursor->win32.handle);
}

void
_glfwPlatformSetCursor(_GLFWwindow *window, _GLFWcursor *cursor UNUSED) {
    if (cursorInContentArea(window)) updateCursorImage(window);
}

// Clipboard {{{

static bool
get_clipboard_data(const _GLFWClipboardData *cd, const char *mime, char **out, size_t *out_sz) {
    size_t cap = 8192, len = 0;
    char *ans = malloc(cap);
    if (!ans) return false;
    GLFWDataChunk chunk = cd->get_data(mime, NULL, cd->ctype);
    void *iter = chunk.iter;
    if (!iter) {
        *out = ans;
        *out_sz = 0;
        return true;
    }
    while (true) {
        chunk = cd->get_data(mime, iter, cd->ctype);
        if (!chunk.sz) break;
        if (len + chunk.sz > cap) {
            cap = (len + chunk.sz) * 2;
            char *n = realloc(ans, cap);
            if (!n) {
                free(ans);
                if (chunk.free) chunk.free(chunk.free_data);
                cd->get_data(NULL, iter, cd->ctype);
                return false;
            }
            ans = n;
        }
        memcpy(ans + len, chunk.data, chunk.sz);
        len += chunk.sz;
        if (chunk.free) chunk.free((void *)chunk.free_data);
    }
    cd->get_data(NULL, iter, cd->ctype);
    *out = ans;
    *out_sz = len;
    return true;
}

// CF_UNICODETEXT uses CRLF line endings and terminal programs expect LF, so
// convert where text crosses between them, as other Windows terminals do.
// Lone CRs are converted as well. Works in place as the text only shrinks.
static size_t
crlf_to_lf(char *text) {
    char *dest = text;
    for (const char *src = text; *src; src++) {
        if (*src == '\r') {
            if (src[1] != '\n') *dest++ = '\n';
        } else *dest++ = *src;
    }
    *dest = 0;
    return (size_t)(dest - text);
}

// Expands bare LFs to CRLF, returns a new NUL terminated buffer
static char*
lf_to_crlf(const char *text, size_t sz, size_t *out_sz) {
    size_t bare = 0;
    for (size_t i = 0; i < sz; i++) if (text[i] == '\n' && (!i || text[i-1] != '\r')) bare++;
    char *ans = malloc(sz + bare + 1);
    if (!ans) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < sz; i++) {
        if (text[i] == '\n' && (!i || text[i-1] != '\r')) ans[n++] = '\r';
        ans[n++] = text[i];
    }
    ans[n] = 0;
    *out_sz = n;
    return ans;
}

static UINT
clipboard_format_for_mime(const char *mime) {
    if (strcmp(mime, "text/plain") == 0 || strncmp(mime, "text/plain;", 11) == 0) return CF_UNICODETEXT;
    WCHAR *w = _glfwCreateWideStringFromUTF8Win32(mime);
    if (!w) return 0;
    UINT ans = RegisterClipboardFormatW(w);
    free(w);
    return ans;
}

static bool
mime_for_clipboard_format(UINT fmt, char *buf, size_t sz) {
    if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT) {
        snprintf(buf, sz, "text/plain");
        return true;
    }
    if (fmt == CF_HDROP) {
        snprintf(buf, sz, "text/uri-list");
        return true;
    }
    if (fmt == CF_DIB || fmt == CF_DIBV5 || fmt == CF_BITMAP) {
        snprintf(buf, sz, "image/bmp");
        return true;
    }
    WCHAR name[256];
    int n = GetClipboardFormatNameW(fmt, name, sizeof(name) / sizeof(WCHAR));
    if (n <= 0) return false;
    char *u = _glfwCreateUTF8FromWideStringWin32(name);
    if (!u) return false;
    bool ans = strchr(u, '/') != NULL;
    if (ans) snprintf(buf, sz, "%s", u);
    free(u);
    return ans;
}

void
_glfwPlatformSetClipboard(GLFWClipboardType t) {
    if (t != GLFW_CLIPBOARD) return;
    if (!OpenClipboard(_glfw.win32.helperWindowHandle)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to open clipboard");
        return;
    }
    EmptyClipboard();
    for (size_t i = 0; i < _glfw.clipboard.num_mime_types; i++) {
        const char *mime = _glfw.clipboard.mime_types[i];
        UINT fmt = clipboard_format_for_mime(mime);
        if (!fmt) continue;
        char *data = NULL;
        size_t sz = 0;
        if (!get_clipboard_data(&_glfw.clipboard, mime, &data, &sz)) continue;
        HANDLE object = NULL;
        if (fmt == CF_UNICODETEXT) {
            char *crlf = lf_to_crlf(data, sz, &sz);
            free(data);
            if (!(data = crlf)) continue;
            int count = MultiByteToWideChar(CP_UTF8, 0, data, (int)sz, NULL, 0);
            object = GlobalAlloc(GMEM_MOVEABLE, ((size_t)count + 1) * sizeof(WCHAR));
            if (object) {
                WCHAR *buffer = GlobalLock(object);
                if (buffer) {
                    MultiByteToWideChar(CP_UTF8, 0, data, (int)sz, buffer, count);
                    buffer[count] = 0;
                    GlobalUnlock(object);
                }
            }
        } else {
            object = GlobalAlloc(GMEM_MOVEABLE, sz + 1);
            if (object) {
                char *buffer = GlobalLock(object);
                if (buffer) {
                    memcpy(buffer, data, sz);
                    buffer[sz] = 0;
                    GlobalUnlock(object);
                }
            }
        }
        free(data);
        if (object) {
            if (!SetClipboardData(fmt, object)) GlobalFree(object);
        }
    }
    CloseClipboard();
}

void
_glfwPlatformGetClipboard(GLFWClipboardType clipboard_type, const char *mime_type, GLFWclipboardwritedatafun write_data, void *object) {
    if (clipboard_type == GLFW_PRIMARY_SELECTION) {
        // Windows has no system primary selection; use the process-local self offer, including its MIME types.
        write_data(object, NULL, 0);
        return;
    }
    if (clipboard_type != GLFW_CLIPBOARD) return;
    if (!OpenClipboard(_glfw.win32.helperWindowHandle)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to open clipboard");
        return;
    }
    if (mime_type == NULL) {
        char buf[256];
        UINT fmt = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0) {
            if (mime_for_clipboard_format(fmt, buf, sizeof(buf))) write_data(object, buf, strlen(buf));
        }
        CloseClipboard();
        return;
    }
    UINT fmt = clipboard_format_for_mime(mime_type);
    if (fmt == CF_UNICODETEXT) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const WCHAR *buffer = GlobalLock(h);
            if (buffer) {
                char *u = _glfwCreateUTF8FromWideStringWin32(buffer);
                GlobalUnlock(h);
                if (u) {
                    write_data(object, u, crlf_to_lf(u));
                    free(u);
                }
            }
        } else if ((h = GetClipboardData(CF_HDROP)) != NULL) {
            HDROP drop = (HDROP)h;
            const int count = DragQueryFileW(drop, 0xffffffff, NULL, 0);
            for (int i = 0; i < count; i++) {
                const UINT length = DragQueryFileW(drop, i, NULL, 0);
                WCHAR *w = calloc((size_t)length + 1, sizeof(WCHAR));
                if (!w) continue;
                DragQueryFileW(drop, i, w, length + 1);
                char *u = _glfwCreateUTF8FromWideStringWin32(w);
                free(w);
                if (u) {
                    if (i) write_data(object, "\n", 1);
                    write_data(object, u, strlen(u));
                    free(u);
                }
            }
        }
    } else if (fmt) {
        HANDLE h = GetClipboardData(fmt);
        if (h) {
            const char *buffer = GlobalLock(h);
            if (buffer) {
                SIZE_T sz = GlobalSize(h);
                write_data(object, buffer, sz);
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
}
// }}}

void
_glfwPlatformGetRequiredInstanceExtensions(char **extensions) {
    if (!_glfw.vk.KHR_surface || !_glfw.vk.KHR_win32_surface) return;

    extensions[0] = "VK_KHR_surface";
    extensions[1] = "VK_KHR_win32_surface";
}

int
_glfwPlatformGetPhysicalDevicePresentationSupport(VkInstance instance, VkPhysicalDevice device, uint32_t queuefamily) {
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR vkGetPhysicalDeviceWin32PresentationSupportKHR =
        (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    if (!vkGetPhysicalDeviceWin32PresentationSupportKHR) {
        _glfwInputError(GLFW_API_UNAVAILABLE, "Win32: Vulkan instance missing VK_KHR_win32_surface extension");
        return false;
    }

    return vkGetPhysicalDeviceWin32PresentationSupportKHR(device, queuefamily);
}

VkResult
_glfwPlatformCreateWindowSurface(VkInstance instance, _GLFWwindow *window, const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    VkResult err;
    VkWin32SurfaceCreateInfoKHR sci;
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;

    vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
    if (!vkCreateWin32SurfaceKHR) {
        _glfwInputError(GLFW_API_UNAVAILABLE, "Win32: Vulkan instance missing VK_KHR_win32_surface extension");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = _glfw.win32.instance;
    sci.hwnd = window->win32.handle;

    err = vkCreateWin32SurfaceKHR(instance, &sci, allocator, surface);
    if (err) { _glfwInputError(GLFW_PLATFORM_ERROR, "Win32: Failed to create Vulkan surface: %s", _glfwGetVulkanResultString(err)); }

    return err;
}


//////////////////////////////////////////////////////////////////////////
//////                        GLFW native API                       //////
//////////////////////////////////////////////////////////////////////////

GLFWAPI HWND
glfwGetWin32Window(GLFWwindow *handle) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    _GLFW_REQUIRE_INIT_OR_RETURN(NULL);
    return window->win32.handle;
}

//////////////////////////////////////////////////////////////////////////
//////                     kitty platform API                       //////
//////////////////////////////////////////////////////////////////////////

void
_glfwPlatformSetWindowSizeIncrements(_GLFWwindow *window UNUSED, int widthincr UNUSED, int heightincr UNUSED) {}

void
_glfwPlatformSetWindowMousePassthrough(_GLFWwindow *window, bool enabled) {
    COLORREF key = 0;
    BYTE alpha = 0;
    DWORD flags = 0;
    DWORD exStyle = GetWindowLongW(window->win32.handle, GWL_EXSTYLE);

    if (exStyle & WS_EX_LAYERED) GetLayeredWindowAttributes(window->win32.handle, &key, &alpha, &flags);

    if (enabled) exStyle |= (WS_EX_TRANSPARENT | WS_EX_LAYERED);
    else {
        exStyle &= ~WS_EX_TRANSPARENT;
        // NOTE: Window opacity also needs the layered window style so do not
        //       remove it if the window is alpha blended
        if (exStyle & WS_EX_LAYERED) {
            if (!(flags & LWA_ALPHA)) exStyle &= ~WS_EX_LAYERED;
        }
    }

    SetWindowLongW(window->win32.handle, GWL_EXSTYLE, exStyle);

    if (enabled) SetLayeredWindowAttributes(window->win32.handle, key, alpha, flags);
    window->win32.mousePassthrough = enabled;
}

// Undocumented but stable since Windows 10 1803; unlike the documented
// DWMWA_SYSTEMBACKDROP_TYPE acrylic backdrop, this is NOT disabled by DWM
// when the window loses focus.
typedef struct {
    int AccentState;
    int AccentFlags;
    DWORD GradientColor;
    int AnimationId;
} GLFW_ACCENT_POLICY;
typedef struct {
    int Attribute;
    GLFW_ACCENT_POLICY *Data;
    ULONG SizeOfData;
} GLFW_WINCOMPATTRDATA;
typedef BOOL(WINAPI *PFN_SetWindowCompositionAttribute)(HWND, GLFW_WINCOMPATTRDATA *);

int
_glfwPlatformSetWindowBlur(_GLFWwindow *window, int blur_radius) {
    window->win32.blur_radius = blur_radius;
    const bool want_blur = blur_radius > 0;
    const bool acrylic = want_blur && window->win32.blur_mode == GLFW_WIN32_BLUR_ACRYLIC;
    // The DWM system backdrop (acrylic) and the SetWindowCompositionAttribute
    // accent policy (blur-behind) fight each other, so always disable the
    // mode that is not in use. DWMWA_SYSTEMBACKDROP_TYPE = 38,
    // DWMSBT_NONE = 1, DWMSBT_TRANSIENTWINDOW (acrylic) = 3. Note that the
    // acrylic backdrop is stripped by DWM whenever the window loses focus.
    if (_glfw.win32.dwmapi.SetWindowAttribute) {
        DWORD backdrop = acrylic ? 3 : 1;
        DwmSetWindowAttribute(window->win32.handle, 38, &backdrop, sizeof(backdrop));
    }
    PFN_SetWindowCompositionAttribute swca = NULL;
    glfw_dlsym(swca, GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!swca) return acrylic ? blur_radius : 0;
    // ACCENT_DISABLED = 0, ACCENT_ENABLE_BLURBEHIND = 3. Acrylic (4) renders
    // opaque over the app's per-pixel alpha on Windows 11 22H2+, while plain
    // blur-behind composes correctly with background_opacity. AccentFlags
    // must be 0: flag 2 draws a solid GradientColor overlay that makes the
    // window opaque.
    GLFW_ACCENT_POLICY policy = {(want_blur && !acrylic) ? 3 : 0, 0, 0, 0};
    GLFW_WINCOMPATTRDATA data = {19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy)};
    if (!swca(window->win32.handle, &data)) return acrylic ? blur_radius : 0;
    return want_blur ? blur_radius : 0;
}

bool
_glfwPlatformIsFullscreen(_GLFWwindow *window, unsigned int flags UNUSED) {
    return window->win32.fullscreen.active;
}

bool
_glfwPlatformToggleFullscreen(_GLFWwindow *window, unsigned int flags UNUSED) {
    HWND hwnd = window->win32.handle;
    if (window->win32.fullscreen.active) {
        SetWindowLongW(hwnd, GWL_STYLE, window->win32.fullscreen.style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, window->win32.fullscreen.exStyle);
        SetWindowPlacement(hwnd, &window->win32.fullscreen.placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        window->win32.fullscreen.active = false;
        return false;
    }
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    window->win32.fullscreen.placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &window->win32.fullscreen.placement);
    window->win32.fullscreen.style = GetWindowLongW(hwnd, GWL_STYLE);
    window->win32.fullscreen.exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_STYLE, (window->win32.fullscreen.style & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
    SetWindowLongW(hwnd, GWL_EXSTYLE, window->win32.fullscreen.exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
    SetWindowPos(
        hwnd,
        HWND_TOP,
        mi.rcMonitor.left,
        mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    window->win32.fullscreen.active = true;
    return true;
}

bool
_glfwPlatformGrabKeyboard(bool grab UNUSED) {
    return false;
}

int
_glfwPlatformWindowOccluded(_GLFWwindow *window UNUSED) {
    return false;
}

int
_glfwPlatformWindowBell(_GLFWwindow *window UNUSED) {
    return MessageBeep(MB_OK) ? true : false;
}

void
_glfwPlatformUpdateIMEState(_GLFWwindow *window, const GLFWIMEUpdateEvent *ev) {
    HIMC himc = ImmGetContext(window->win32.handle);
    if (!himc) return;
    switch (ev->type) {
        case GLFW_IME_UPDATE_FOCUS:
            if (ev->focused) ImmAssociateContextEx(window->win32.handle, NULL, IACE_DEFAULT);
            break;
        case GLFW_IME_UPDATE_CURSOR_POSITION: {
            COMPOSITIONFORM cf;
            ZeroMemory(&cf, sizeof(cf));
            cf.dwStyle = CFS_POINT;
            cf.ptCurrentPos.x = ev->cursor.left;
            cf.ptCurrentPos.y = ev->cursor.top + ev->cursor.height;
            ImmSetCompositionWindow(himc, &cf);
            CANDIDATEFORM cand;
            ZeroMemory(&cand, sizeof(cand));
            cand.dwStyle = CFS_EXCLUDE;
            cand.ptCurrentPos.x = ev->cursor.left;
            cand.ptCurrentPos.y = ev->cursor.top + ev->cursor.height;
            cand.rcArea.left = ev->cursor.left;
            cand.rcArea.top = ev->cursor.top;
            cand.rcArea.right = ev->cursor.left + ev->cursor.width;
            cand.rcArea.bottom = ev->cursor.top + ev->cursor.height;
            ImmSetCandidateWindow(himc, &cand);
            break;
        }
    }
    ImmReleaseContext(window->win32.handle, himc);
}

void
_glfwPlatformChangeCursorTheme(void) {}

monotonic_t
_glfwPlatformGetDoubleClickInterval(_GLFWwindow *window UNUSED) {
    return ms_to_monotonic_t((long long)GetDoubleClickTime());
}

void
_glfwPlatformGetKeyboardRepeatDelay(monotonic_t *delay, monotonic_t *interval) {
    // SPI_GETKEYBOARDDELAY: 0..3 -> 250ms..1000ms, SPI_GETKEYBOARDSPEED: 0..31 -> 2.5..30 repeats/sec
    DWORD d = 1, sp = 31;
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &d, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &sp, 0);
    if (delay) *delay = ms_to_monotonic_t(250ll * (long long)(d + 1));
    if (interval) *interval = ms_to_monotonic_t((long long)(1000.0 / (2.5 + (27.5 * sp) / 31.0)));
}

bool
_glfwPlatformSetLayerShellConfig(_GLFWwindow *window, const GLFWLayerShellConfig *value) {
    if (value) window->win32.layer_shell_config = *value;
    return false;
}

const GLFWLayerShellConfig *
_glfwPlatformGetLayerShellConfig(_GLFWwindow *window) {
    return &window->win32.layer_shell_config;
}

// Drop target {{{
void
_glfwPlatformRequestDropUpdate(_GLFWwindow *window UNUSED) {}

int
_glfwPlatformRequestDropData(_GLFWwindow *window, const char *mime) {
    if (!window->win32.drop.active) return ENOENT;
    for (unsigned i = 0; i < 2; i++) {
        if (strcmp(mime, drop_mimes[i]) == 0) {
            window->win32.drop.data_requested[i] = true;
            PostMessageW(window->win32.handle, WM_KITTY_DROP_DATA, 0, 0);
            return 0;
        }
    }
    return ENOENT;
}

ssize_t
_glfwPlatformReadAvailableDropData(GLFWwindow *w, GLFWDropEvent *ev, char *buffer, size_t sz) {
    _GLFWwindow *window = (_GLFWwindow *)w;
    if (!window->win32.drop.active) return -ENOENT;
    const char *mime = ev->mimes[0];
    const char *data;
    size_t data_len;
    size_t *pos;
    if (strcmp(mime, drop_mimes[0]) == 0) {
        data = window->win32.drop.uri_list;
        data_len = window->win32.drop.uri_list_len;
        pos = &window->win32.drop.read_pos[0];
    } else if (strcmp(mime, drop_mimes[1]) == 0) {
        data = window->win32.drop.plain_text;
        data_len = window->win32.drop.plain_text_len;
        pos = &window->win32.drop.read_pos[1];
    } else return -ENOENT;
    if (*pos >= data_len) return 0;
    size_t n = data_len - *pos;
    if (n > sz) n = sz;
    memcpy(buffer, data + *pos, n);
    *pos += n;
    _glfwPlatformRequestDropData(window, mime);
    return (ssize_t)n;
}

void
_glfwPlatformEndDrop(GLFWwindow *w, GLFWDragOperationType op UNUSED) {
    free_drop_data((_GLFWwindow *)w);
}
// }}}

// Drag source (not yet implemented on Windows) {{{
int
_glfwPlatformStartDrag(_GLFWwindow *window UNUSED, const GLFWimage *thumbnail UNUSED) {
    return ENOTSUP;
}

void
_glfwPlatformCancelDrag(_GLFWwindow *window UNUSED) {}

void
_glfwPlatformFreeDragSourceData(void) {}

int
_glfwPlatformDragDataReady(const char *mime_type UNUSED, const char *data UNUSED, size_t sz UNUSED, int type UNUSED) {
    return ENOTSUP;
}

int
_glfwPlatformChangeDragImage(const GLFWimage *thumbnail UNUSED) {
    return ENOTSUP;
}
// }}}

// Color scheme {{{
static GLFWColorScheme
query_system_color_theme(void) {
    HKEY key;
    GLFWColorScheme ans = GLFW_COLOR_SCHEME_NO_PREFERENCE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD value = 1, size = sizeof(value), type = REG_DWORD;
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD) {
            ans = value ? GLFW_COLOR_SCHEME_LIGHT : GLFW_COLOR_SCHEME_DARK;
        }
        RegCloseKey(key);
    }
    return ans;
}

GLFWAPI GLFWColorScheme
glfwGetCurrentSystemColorTheme(bool query_if_unintialized UNUSED) {
    return query_system_color_theme();
}

// Ask DWM to draw the non-client area (title bar and borders) using the dark
// theme when the user has selected the dark app mode in Windows settings
static void
applyTitlebarTheme(_GLFWwindow *window, GLFWColorScheme scheme) {
    if (!window->win32.handle || !_glfw.win32.dwmapi.SetWindowAttribute) return;
    DWORD attribute;
    if (_glfwIsWindows10BuildOrGreaterWin32(18985)) attribute = DWMWA_USE_IMMERSIVE_DARK_MODE;
    else if (_glfwIsWindows10BuildOrGreaterWin32(17763)) attribute = DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1;
    else return;
    BOOL dark = scheme == GLFW_COLOR_SCHEME_DARK;
    if (FAILED(DwmSetWindowAttribute(window->win32.handle, attribute, &dark, sizeof(dark)))) return;
    // Windows 10 does not repaint the frame of a visible window until its
    // activation state changes, so nudge it
    if (!_glfwIsWindows11OrGreaterWin32() && IsWindowVisible(window->win32.handle)) {
        BOOL active = GetActiveWindow() == window->win32.handle;
        SendMessageW(window->win32.handle, WM_NCACTIVATE, !active, 0);
        SendMessageW(window->win32.handle, WM_NCACTIVATE, active, 0);
    }
}

void
_glfwPlatformInputColorScheme(GLFWColorScheme appearance) {
    for (_GLFWwindow *window = _glfw.windowListHead; window; window = window->next) applyTitlebarTheme(window, appearance);
}
// }}}

// Timers and main loop {{{
static void
timer_compact(void) {
    size_t w = 0;
    for (size_t i = 0; i < _glfw.win32.timers.count; i++) {
        if (_glfw.win32.timers.items[i].id) {
            if (w != i) _glfw.win32.timers.items[w] = _glfw.win32.timers.items[i];
            w++;
        }
    }
    _glfw.win32.timers.count = w;
}

unsigned long long
_glfwPlatformAddTimer(monotonic_t interval, bool repeats, GLFWuserdatafun callback, void *callback_data, GLFWuserdatafun free_callback) {
    if (_glfw.win32.timers.count >= _glfw.win32.timers.capacity) {
        size_t cap = _glfw.win32.timers.capacity ? _glfw.win32.timers.capacity * 2 : 8;
        _GLFWtimerWin32 *n = realloc(_glfw.win32.timers.items, cap * sizeof(_GLFWtimerWin32));
        if (!n) return 0;
        _glfw.win32.timers.items = n;
        _glfw.win32.timers.capacity = cap;
    }
    _GLFWtimerWin32 *t = _glfw.win32.timers.items + _glfw.win32.timers.count++;
    memset(t, 0, sizeof(*t));
    t->id = ++_glfw.win32.timers.next_id;
    if (!t->id) t->id = ++_glfw.win32.timers.next_id;
    t->interval = interval;
    t->trigger_at = monotonic() + interval;
    t->repeats = repeats;
    t->enabled = true;
    t->callback = callback;
    t->callback_data = callback_data;
    t->free_callback = free_callback;
    return t->id;
}

void
_glfwPlatformRemoveTimer(unsigned long long timer_id) {
    for (size_t i = 0; i < _glfw.win32.timers.count; i++) {
        _GLFWtimerWin32 *t = _glfw.win32.timers.items + i;
        if (t->id == timer_id) {
            if (t->free_callback) t->free_callback(t->id, t->callback_data);
            t->id = 0;
            t->enabled = false;
        }
    }
}

void
_glfwPlatformUpdateTimer(unsigned long long timer_id, monotonic_t interval, bool enabled) {
    for (size_t i = 0; i < _glfw.win32.timers.count; i++) {
        _GLFWtimerWin32 *t = _glfw.win32.timers.items + i;
        if (t->id == timer_id) {
            t->interval = interval;
            t->trigger_at = monotonic() + interval;
            t->enabled = enabled;
        }
    }
}

DWORD
_glfwTimerWaitTimeoutWin32(void) {
    monotonic_t now = monotonic(), soonest = -1;
    for (size_t i = 0; i < _glfw.win32.timers.count; i++) {
        _GLFWtimerWin32 *t = _glfw.win32.timers.items + i;
        if (!t->id || !t->enabled) continue;
        monotonic_t d = t->trigger_at - now;
        if (d < 0) d = 0;
        if (soonest < 0 || d < soonest) soonest = d;
    }
    if (soonest < 0) return INFINITE;
    return wait_timeout_ms(soonest);
}

void
_glfwDispatchTimersWin32(void) {
    if (_glfw.win32.dispatching) return;
    _glfw.win32.dispatching = true;
    monotonic_t now = monotonic();
    bool need_compact = false;
    // callbacks may add timers, so iterate by index over the current count
    size_t count = _glfw.win32.timers.count;
    for (size_t i = 0; i < count; i++) {
        _GLFWtimerWin32 *t = _glfw.win32.timers.items + i;
        if (!t->id || !t->enabled || t->trigger_at > now) continue;
        unsigned long long id = t->id;
        GLFWuserdatafun cb = t->callback;
        void *cbd = t->callback_data;
        if (t->repeats) t->trigger_at = now + t->interval;
        else t->enabled = false;
        if (cb) cb(id, cbd);
        t = _glfw.win32.timers.items + i; // items may have been reallocated
        if (t->id == id && !t->repeats && !t->enabled) {
            if (t->free_callback) t->free_callback(t->id, t->callback_data);
            t->id = 0;
            need_compact = true;
        }
    }
    for (size_t i = 0; i < _glfw.win32.timers.count; i++)
        if (!_glfw.win32.timers.items[i].id) need_compact = true;
    if (need_compact) timer_compact();
    _glfw.win32.dispatching = false;
}

void
_glfwFreeTimersWin32(void) {
    for (size_t i = 0; i < _glfw.win32.timers.count; i++) {
        _GLFWtimerWin32 *t = _glfw.win32.timers.items + i;
        if (t->id && t->free_callback) t->free_callback(t->id, t->callback_data);
    }
    free(_glfw.win32.timers.items);
    _glfw.win32.timers.items = NULL;
    _glfw.win32.timers.count = _glfw.win32.timers.capacity = 0;
}

void
_glfwPlatformStopMainLoop(void) {
    if (_glfw.win32.keep_going) {
        _glfw.win32.keep_going = false;
        _glfwPlatformPostEmptyEvent();
    }
}

void
_glfwPlatformRunMainLoop(GLFWtickcallback tick_callback, void *data) {
    _glfw.win32.tick_callback = tick_callback;
    _glfw.win32.tick_callback_data = data;
    _glfw.win32.keep_going = true;
    while (_glfw.win32.keep_going) {
        _glfwPlatformWaitEvents();
        dispatchPendingTick();
    }
    _glfw.win32.tick_callback = NULL;
    _glfw.win32.tick_callback_data = NULL;
}
// }}}

// EGL {{{
EGLenum
_glfwPlatformGetEGLPlatform(EGLint **attribs) {
    if (_glfw.egl.ANGLE_platform_angle) {
        int type = 0;

        if (_glfw.egl.ANGLE_platform_angle_opengl) {
            if (_glfw.hints.init.angleType == GLFW_ANGLE_PLATFORM_TYPE_OPENGL) type = EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE;
            else if (_glfw.hints.init.angleType == GLFW_ANGLE_PLATFORM_TYPE_OPENGLES) type = EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE;
        }

        if (_glfw.egl.ANGLE_platform_angle_d3d) {
            if (_glfw.hints.init.angleType == GLFW_ANGLE_PLATFORM_TYPE_D3D9) type = EGL_PLATFORM_ANGLE_TYPE_D3D9_ANGLE;
            else if (_glfw.hints.init.angleType == GLFW_ANGLE_PLATFORM_TYPE_D3D11) type = EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE;
        }

        if (_glfw.egl.ANGLE_platform_angle_vulkan) {
            if (_glfw.hints.init.angleType == GLFW_ANGLE_PLATFORM_TYPE_VULKAN) type = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
        }

        if (type) {
            *attribs = calloc(3, sizeof(EGLint));
            (*attribs)[0] = EGL_PLATFORM_ANGLE_TYPE_ANGLE;
            (*attribs)[1] = type;
            (*attribs)[2] = EGL_NONE;
            return EGL_PLATFORM_ANGLE_ANGLE;
        }
    }

    return 0;
}

EGLNativeDisplayType
_glfwPlatformGetEGLNativeDisplay(void) {
    return GetDC(_glfw.win32.helperWindowHandle);
}

EGLNativeWindowType
_glfwPlatformGetEGLNativeWindow(_GLFWwindow *window) {
    return window->win32.handle;
}
// }}}

GLFWAPI int
glfwGetNativeKeyForName(const char *key_name, bool case_sensitive UNUSED) {
    if (!key_name || !key_name[0]) return 0;
    WCHAR *w = _glfwCreateWideStringFromUTF8Win32(key_name);
    if (!w) return 0;
    uint32_t key = first_codepoint(w, (int)wcslen(w));
    free(w);
    return _glfwPlatformGetNativeKeyForKey(to_lower_codepoint(key));
}

GLFWAPI bool
glfwIsLayerShellSupported(void) {
    return false;
}

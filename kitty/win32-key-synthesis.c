/* Distributed under terms of the GPL3 license. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include "monotonic.h"
#define GLFW_INCLUDE_NONE
#include "../glfw/glfw3.h"
#include "../glfw/win32_keys.h"

bool
synthesize_win32_key(GLFWkeyevent *ev) {
    UINT vk = vk_for_functional_key(ev->key);
    unsigned mods = ev->mods;
    if (!vk && ev->key && ev->key < 0x10000 && !(GLFW_FKEY_FIRST <= ev->key && ev->key <= GLFW_FKEY_LAST)) {
        SHORT mapped = VkKeyScanW((WCHAR)ev->key);
        if (mapped != -1) {
            vk = mapped & 0xff;
            if (mapped & 0x100) mods |= GLFW_MOD_SHIFT;
            if (mapped & 0x200) mods |= GLFW_MOD_CONTROL;
            if (mapped & 0x400) mods |= GLFW_MOD_ALT;
        }
    }
    if (!vk && GLFW_FKEY_FIRST <= ev->key && ev->key <= GLFW_FKEY_LAST && !(ev->text && *ev->text)) return false;
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    if (vk == VK_SNAPSHOT) sc = 0x37;
    if (vk == VK_PAUSE) sc = 0x45;
    bool extended = (sc & 0xff00) == 0xe000 || ev->key == GLFW_FKEY_KP_ENTER;
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_SNAPSHOT:
        case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
            extended = true;
    }
    ev->native_key = (sc & 0xff) | (extended ? 0x100 : 0);
    ev->win32_vk = vk;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) ev->win32_vk = VK_SHIFT;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) ev->win32_vk = VK_CONTROL;
    if (vk == VK_LMENU || vk == VK_RMENU) ev->win32_vk = VK_MENU;
    ev->win32_control_state = (mods & GLFW_MOD_SHIFT ? SHIFT_PRESSED : 0) |
                              (mods & GLFW_MOD_CONTROL ? LEFT_CTRL_PRESSED : 0) |
                              (mods & GLFW_MOD_ALT ? LEFT_ALT_PRESSED : 0) |
                              (mods & GLFW_MOD_CAPS_LOCK ? CAPSLOCK_ON : 0) |
                              (mods & GLFW_MOD_NUM_LOCK ? NUMLOCK_ON : 0) | (extended ? ENHANCED_KEY : 0);
    if (!vk) {
        ev->win32_char = ev->key;
    } else {
        BYTE state[256] = {0};
        state[VK_SHIFT] = mods & GLFW_MOD_SHIFT ? 0x80 : 0;
        state[VK_CONTROL] = mods & GLFW_MOD_CONTROL ? 0x80 : 0;
        state[VK_MENU] = mods & GLFW_MOD_ALT ? 0x80 : 0;
        state[VK_CAPITAL] = mods & GLFW_MOD_CAPS_LOCK ? 1 : 0;
        state[VK_NUMLOCK] = mods & GLFW_MOD_NUM_LOCK ? 1 : 0;
        WCHAR chars[8];
        // Do not change the keyboard's dead-key state when synthesizing a key.
        int n = ToUnicode(vk, sc & 0xff, state, chars, sizeof(chars) / sizeof(chars[0]), 4);
        if (n > 0) ev->win32_char = chars[0];
        else if (n < 0) ev->win32_char = ev->key;
    }
    return true;
}
#endif

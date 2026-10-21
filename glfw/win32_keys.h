#pragma once

static inline UINT
vk_for_functional_key(uint32_t key) {
    switch (key) {
        case GLFW_FKEY_ESCAPE: return VK_ESCAPE;
        case GLFW_FKEY_ENTER: return VK_RETURN;
        case GLFW_FKEY_TAB: return VK_TAB;
        case GLFW_FKEY_BACKSPACE: return VK_BACK;
        case GLFW_FKEY_INSERT: return VK_INSERT;
        case GLFW_FKEY_DELETE: return VK_DELETE;
        case GLFW_FKEY_LEFT: return VK_LEFT;
        case GLFW_FKEY_RIGHT: return VK_RIGHT;
        case GLFW_FKEY_UP: return VK_UP;
        case GLFW_FKEY_DOWN: return VK_DOWN;
        case GLFW_FKEY_PAGE_UP: return VK_PRIOR;
        case GLFW_FKEY_PAGE_DOWN: return VK_NEXT;
        case GLFW_FKEY_HOME: return VK_HOME;
        case GLFW_FKEY_END: return VK_END;
        case GLFW_FKEY_CAPS_LOCK: return VK_CAPITAL;
        case GLFW_FKEY_SCROLL_LOCK: return VK_SCROLL;
        case GLFW_FKEY_NUM_LOCK: return VK_NUMLOCK;
        case GLFW_FKEY_PRINT_SCREEN: return VK_SNAPSHOT;
        case GLFW_FKEY_PAUSE: return VK_PAUSE;
        case GLFW_FKEY_MENU: return VK_APPS;
        case GLFW_FKEY_KP_0: return VK_NUMPAD0;
        case GLFW_FKEY_KP_1: return VK_NUMPAD1;
        case GLFW_FKEY_KP_2: return VK_NUMPAD2;
        case GLFW_FKEY_KP_3: return VK_NUMPAD3;
        case GLFW_FKEY_KP_4: return VK_NUMPAD4;
        case GLFW_FKEY_KP_5: return VK_NUMPAD5;
        case GLFW_FKEY_KP_6: return VK_NUMPAD6;
        case GLFW_FKEY_KP_7: return VK_NUMPAD7;
        case GLFW_FKEY_KP_8: return VK_NUMPAD8;
        case GLFW_FKEY_KP_9: return VK_NUMPAD9;
        case GLFW_FKEY_KP_DECIMAL: return VK_DECIMAL;
        case GLFW_FKEY_KP_DIVIDE: return VK_DIVIDE;
        case GLFW_FKEY_KP_MULTIPLY: return VK_MULTIPLY;
        case GLFW_FKEY_KP_SUBTRACT: return VK_SUBTRACT;
        case GLFW_FKEY_KP_ADD: return VK_ADD;
        case GLFW_FKEY_KP_ENTER: return VK_RETURN;
        case GLFW_FKEY_LEFT_SHIFT: return VK_LSHIFT;
        case GLFW_FKEY_RIGHT_SHIFT: return VK_RSHIFT;
        case GLFW_FKEY_LEFT_CONTROL: return VK_LCONTROL;
        case GLFW_FKEY_RIGHT_CONTROL: return VK_RCONTROL;
        case GLFW_FKEY_LEFT_ALT: return VK_LMENU;
        case GLFW_FKEY_RIGHT_ALT: return VK_RMENU;
        case GLFW_FKEY_LEFT_SUPER: return VK_LWIN;
        case GLFW_FKEY_RIGHT_SUPER: return VK_RWIN;
        case GLFW_FKEY_MEDIA_PLAY_PAUSE: return VK_MEDIA_PLAY_PAUSE;
        case GLFW_FKEY_MEDIA_STOP: return VK_MEDIA_STOP;
        case GLFW_FKEY_MEDIA_TRACK_NEXT: return VK_MEDIA_NEXT_TRACK;
        case GLFW_FKEY_MEDIA_TRACK_PREVIOUS: return VK_MEDIA_PREV_TRACK;
        case GLFW_FKEY_LOWER_VOLUME: return VK_VOLUME_DOWN;
        case GLFW_FKEY_RAISE_VOLUME: return VK_VOLUME_UP;
        case GLFW_FKEY_MUTE_VOLUME: return VK_VOLUME_MUTE;
        default:
            if (GLFW_FKEY_F1 <= key && key <= GLFW_FKEY_F24) return VK_F1 + (key - GLFW_FKEY_F1);
            return 0;
    }
}

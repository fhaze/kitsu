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

#include <stdlib.h>
#include <malloc.h>
#include <mmsystem.h>

#if defined(_GLFW_USE_HYBRID_HPG) || defined(_GLFW_USE_OPTIMUS_HPG)

#if defined(_GLFW_BUILD_DLL)
#pragma message("These symbols must be exported by the executable and have no effect in a DLL")
#endif

// Executables (but not DLLs) exporting this symbol with this value will be
// automatically directed to the high-performance GPU on Nvidia Optimus systems
// with up-to-date drivers
//
__declspec(dllexport) DWORD NvOptimusEnablement = 1;

// Executables (but not DLLs) exporting this symbol with this value will be
// automatically directed to the high-performance GPU on AMD PowerXpress systems
// with up-to-date drivers
//
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#endif // _GLFW_USE_HYBRID_HPG

#if defined(_GLFW_BUILD_DLL)

// GLFW DLL entry point
//
BOOL WINAPI
DllMain(HINSTANCE instance UNUSED, DWORD reason UNUSED, LPVOID reserved UNUSED) {
    return TRUE;
}

#endif // _GLFW_BUILD_DLL

// Load necessary libraries (DLLs)
//
static bool
loadLibraries(void) {
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const WCHAR *)&_glfw, (HMODULE *)&_glfw.win32.instance)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to retrieve own module handle");
        return false;
    }

    _glfw.win32.user32.instance = LoadLibraryA("user32.dll");
    if (!_glfw.win32.user32.instance) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to load user32.dll");
        return false;
    }

    glfw_dlsym(_glfw.win32.user32.SetProcessDPIAware_, _glfw.win32.user32.instance, "SetProcessDPIAware");
    glfw_dlsym(_glfw.win32.user32.ChangeWindowMessageFilterEx_, _glfw.win32.user32.instance, "ChangeWindowMessageFilterEx");
    glfw_dlsym(_glfw.win32.user32.EnableNonClientDpiScaling_, _glfw.win32.user32.instance, "EnableNonClientDpiScaling");
    glfw_dlsym(_glfw.win32.user32.SetProcessDpiAwarenessContext_, _glfw.win32.user32.instance, "SetProcessDpiAwarenessContext");
    glfw_dlsym(_glfw.win32.user32.GetDpiForWindow_, _glfw.win32.user32.instance, "GetDpiForWindow");
    glfw_dlsym(_glfw.win32.user32.AdjustWindowRectExForDpi_, _glfw.win32.user32.instance, "AdjustWindowRectExForDpi");
    glfw_dlsym(_glfw.win32.user32.GetSystemMetricsForDpi_, _glfw.win32.user32.instance, "GetSystemMetricsForDpi");

    _glfw.win32.dwmapi.instance = LoadLibraryA("dwmapi.dll");
    if (_glfw.win32.dwmapi.instance) {
        glfw_dlsym(_glfw.win32.dwmapi.IsCompositionEnabled, _glfw.win32.dwmapi.instance, "DwmIsCompositionEnabled");
        glfw_dlsym(_glfw.win32.dwmapi.Flush, _glfw.win32.dwmapi.instance, "DwmFlush");
        glfw_dlsym(_glfw.win32.dwmapi.EnableBlurBehindWindow, _glfw.win32.dwmapi.instance, "DwmEnableBlurBehindWindow");
        glfw_dlsym(_glfw.win32.dwmapi.GetColorizationColor, _glfw.win32.dwmapi.instance, "DwmGetColorizationColor");
        glfw_dlsym(_glfw.win32.dwmapi.SetWindowAttribute, _glfw.win32.dwmapi.instance, "DwmSetWindowAttribute");
    }

    _glfw.win32.shcore.instance = LoadLibraryA("shcore.dll");
    if (_glfw.win32.shcore.instance) {
        glfw_dlsym(_glfw.win32.shcore.SetProcessDpiAwareness_, _glfw.win32.shcore.instance, "SetProcessDpiAwareness");
        glfw_dlsym(_glfw.win32.shcore.GetDpiForMonitor_, _glfw.win32.shcore.instance, "GetDpiForMonitor");
    }

    _glfw.win32.ntdll.instance = LoadLibraryA("ntdll.dll");
    if (_glfw.win32.ntdll.instance) { glfw_dlsym(_glfw.win32.ntdll.RtlVerifyVersionInfo_, _glfw.win32.ntdll.instance, "RtlVerifyVersionInfo"); }

    return true;
}

// Unload used libraries (DLLs)
//
static void
freeLibraries(void) {
    if (_glfw.win32.user32.instance) FreeLibrary(_glfw.win32.user32.instance);

    if (_glfw.win32.dwmapi.instance) FreeLibrary(_glfw.win32.dwmapi.instance);

    if (_glfw.win32.shcore.instance) FreeLibrary(_glfw.win32.shcore.instance);

    if (_glfw.win32.ntdll.instance) FreeLibrary(_glfw.win32.ntdll.instance);
}

// Create key code translation tables
//
static void
createKeyTables(void) {
    memset(_glfw.win32.keycodes, 0, sizeof(_glfw.win32.keycodes));

    _glfw.win32.keycodes[0x00B] = '0';
    _glfw.win32.keycodes[0x002] = '1';
    _glfw.win32.keycodes[0x003] = '2';
    _glfw.win32.keycodes[0x004] = '3';
    _glfw.win32.keycodes[0x005] = '4';
    _glfw.win32.keycodes[0x006] = '5';
    _glfw.win32.keycodes[0x007] = '6';
    _glfw.win32.keycodes[0x008] = '7';
    _glfw.win32.keycodes[0x009] = '8';
    _glfw.win32.keycodes[0x00A] = '9';
    _glfw.win32.keycodes[0x01E] = 'a';
    _glfw.win32.keycodes[0x030] = 'b';
    _glfw.win32.keycodes[0x02E] = 'c';
    _glfw.win32.keycodes[0x020] = 'd';
    _glfw.win32.keycodes[0x012] = 'e';
    _glfw.win32.keycodes[0x021] = 'f';
    _glfw.win32.keycodes[0x022] = 'g';
    _glfw.win32.keycodes[0x023] = 'h';
    _glfw.win32.keycodes[0x017] = 'i';
    _glfw.win32.keycodes[0x024] = 'j';
    _glfw.win32.keycodes[0x025] = 'k';
    _glfw.win32.keycodes[0x026] = 'l';
    _glfw.win32.keycodes[0x032] = 'm';
    _glfw.win32.keycodes[0x031] = 'n';
    _glfw.win32.keycodes[0x018] = 'o';
    _glfw.win32.keycodes[0x019] = 'p';
    _glfw.win32.keycodes[0x010] = 'q';
    _glfw.win32.keycodes[0x013] = 'r';
    _glfw.win32.keycodes[0x01F] = 's';
    _glfw.win32.keycodes[0x014] = 't';
    _glfw.win32.keycodes[0x016] = 'u';
    _glfw.win32.keycodes[0x02F] = 'v';
    _glfw.win32.keycodes[0x011] = 'w';
    _glfw.win32.keycodes[0x02D] = 'x';
    _glfw.win32.keycodes[0x015] = 'y';
    _glfw.win32.keycodes[0x02C] = 'z';

    _glfw.win32.keycodes[0x028] = 0x27;
    _glfw.win32.keycodes[0x02B] = 0x5c;
    _glfw.win32.keycodes[0x033] = ',';
    _glfw.win32.keycodes[0x00D] = '=';
    _glfw.win32.keycodes[0x029] = '`';
    _glfw.win32.keycodes[0x01A] = '[';
    _glfw.win32.keycodes[0x00C] = '-';
    _glfw.win32.keycodes[0x034] = '.';
    _glfw.win32.keycodes[0x01B] = ']';
    _glfw.win32.keycodes[0x027] = ';';
    _glfw.win32.keycodes[0x035] = '/';

    _glfw.win32.keycodes[0x00E] = GLFW_FKEY_BACKSPACE;
    _glfw.win32.keycodes[0x153] = GLFW_FKEY_DELETE;
    _glfw.win32.keycodes[0x14F] = GLFW_FKEY_END;
    _glfw.win32.keycodes[0x01C] = GLFW_FKEY_ENTER;
    _glfw.win32.keycodes[0x001] = GLFW_FKEY_ESCAPE;
    _glfw.win32.keycodes[0x147] = GLFW_FKEY_HOME;
    _glfw.win32.keycodes[0x152] = GLFW_FKEY_INSERT;
    _glfw.win32.keycodes[0x15D] = GLFW_FKEY_MENU;
    _glfw.win32.keycodes[0x151] = GLFW_FKEY_PAGE_DOWN;
    _glfw.win32.keycodes[0x149] = GLFW_FKEY_PAGE_UP;
    _glfw.win32.keycodes[0x045] = GLFW_FKEY_PAUSE;
    _glfw.win32.keycodes[0x039] = ' ';
    _glfw.win32.keycodes[0x00F] = GLFW_FKEY_TAB;
    _glfw.win32.keycodes[0x03A] = GLFW_FKEY_CAPS_LOCK;
    _glfw.win32.keycodes[0x145] = GLFW_FKEY_NUM_LOCK;
    _glfw.win32.keycodes[0x046] = GLFW_FKEY_SCROLL_LOCK;
    _glfw.win32.keycodes[0x03B] = GLFW_FKEY_F1;
    _glfw.win32.keycodes[0x03C] = GLFW_FKEY_F2;
    _glfw.win32.keycodes[0x03D] = GLFW_FKEY_F3;
    _glfw.win32.keycodes[0x03E] = GLFW_FKEY_F4;
    _glfw.win32.keycodes[0x03F] = GLFW_FKEY_F5;
    _glfw.win32.keycodes[0x040] = GLFW_FKEY_F6;
    _glfw.win32.keycodes[0x041] = GLFW_FKEY_F7;
    _glfw.win32.keycodes[0x042] = GLFW_FKEY_F8;
    _glfw.win32.keycodes[0x043] = GLFW_FKEY_F9;
    _glfw.win32.keycodes[0x044] = GLFW_FKEY_F10;
    _glfw.win32.keycodes[0x057] = GLFW_FKEY_F11;
    _glfw.win32.keycodes[0x058] = GLFW_FKEY_F12;
    _glfw.win32.keycodes[0x064] = GLFW_FKEY_F13;
    _glfw.win32.keycodes[0x065] = GLFW_FKEY_F14;
    _glfw.win32.keycodes[0x066] = GLFW_FKEY_F15;
    _glfw.win32.keycodes[0x067] = GLFW_FKEY_F16;
    _glfw.win32.keycodes[0x068] = GLFW_FKEY_F17;
    _glfw.win32.keycodes[0x069] = GLFW_FKEY_F18;
    _glfw.win32.keycodes[0x06A] = GLFW_FKEY_F19;
    _glfw.win32.keycodes[0x06B] = GLFW_FKEY_F20;
    _glfw.win32.keycodes[0x06C] = GLFW_FKEY_F21;
    _glfw.win32.keycodes[0x06D] = GLFW_FKEY_F22;
    _glfw.win32.keycodes[0x06E] = GLFW_FKEY_F23;
    _glfw.win32.keycodes[0x076] = GLFW_FKEY_F24;
    _glfw.win32.keycodes[0x038] = GLFW_FKEY_LEFT_ALT;
    _glfw.win32.keycodes[0x01D] = GLFW_FKEY_LEFT_CONTROL;
    _glfw.win32.keycodes[0x02A] = GLFW_FKEY_LEFT_SHIFT;
    _glfw.win32.keycodes[0x15B] = GLFW_FKEY_LEFT_SUPER;
    _glfw.win32.keycodes[0x137] = GLFW_FKEY_PRINT_SCREEN;
    _glfw.win32.keycodes[0x138] = GLFW_FKEY_RIGHT_ALT;
    _glfw.win32.keycodes[0x11D] = GLFW_FKEY_RIGHT_CONTROL;
    _glfw.win32.keycodes[0x036] = GLFW_FKEY_RIGHT_SHIFT;
    _glfw.win32.keycodes[0x15C] = GLFW_FKEY_RIGHT_SUPER;
    _glfw.win32.keycodes[0x150] = GLFW_FKEY_DOWN;
    _glfw.win32.keycodes[0x14B] = GLFW_FKEY_LEFT;
    _glfw.win32.keycodes[0x14D] = GLFW_FKEY_RIGHT;
    _glfw.win32.keycodes[0x148] = GLFW_FKEY_UP;

    _glfw.win32.keycodes[0x052] = GLFW_FKEY_KP_0;
    _glfw.win32.keycodes[0x04F] = GLFW_FKEY_KP_1;
    _glfw.win32.keycodes[0x050] = GLFW_FKEY_KP_2;
    _glfw.win32.keycodes[0x051] = GLFW_FKEY_KP_3;
    _glfw.win32.keycodes[0x04B] = GLFW_FKEY_KP_4;
    _glfw.win32.keycodes[0x04C] = GLFW_FKEY_KP_5;
    _glfw.win32.keycodes[0x04D] = GLFW_FKEY_KP_6;
    _glfw.win32.keycodes[0x047] = GLFW_FKEY_KP_7;
    _glfw.win32.keycodes[0x048] = GLFW_FKEY_KP_8;
    _glfw.win32.keycodes[0x049] = GLFW_FKEY_KP_9;
    _glfw.win32.keycodes[0x04E] = GLFW_FKEY_KP_ADD;
    _glfw.win32.keycodes[0x053] = GLFW_FKEY_KP_DECIMAL;
    _glfw.win32.keycodes[0x135] = GLFW_FKEY_KP_DIVIDE;
    _glfw.win32.keycodes[0x11C] = GLFW_FKEY_KP_ENTER;
    _glfw.win32.keycodes[0x059] = GLFW_FKEY_KP_EQUAL;
    _glfw.win32.keycodes[0x037] = GLFW_FKEY_KP_MULTIPLY;
    _glfw.win32.keycodes[0x04A] = GLFW_FKEY_KP_SUBTRACT;
}

// Creates a dummy window for behind-the-scenes work
//
static bool
createHelperWindow(void) {
    MSG msg;

    _glfw.win32.helperWindowHandle = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW,
        _GLFW_WNDCLASSNAME,
        L"GLFW message window",
        WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0,
        0,
        1,
        1,
        NULL,
        NULL,
        _glfw.win32.instance,
        NULL);

    if (!_glfw.win32.helperWindowHandle) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create helper window");
        return false;
    }

    // HACK: The command to the first ShowWindow call is ignored if the parent
    //       process passed along a STARTUPINFO, so clear that with a no-op call
    ShowWindow(_glfw.win32.helperWindowHandle, SW_HIDE);

    while (PeekMessageW(&msg, _glfw.win32.helperWindowHandle, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return true;
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW internal API                      //////
//////////////////////////////////////////////////////////////////////////

// Returns a wide string version of the specified UTF-8 string
//
WCHAR *
_glfwCreateWideStringFromUTF8Win32(const char *source) {
    WCHAR *target;
    int count;

    count = MultiByteToWideChar(CP_UTF8, 0, source, -1, NULL, 0);
    if (!count) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to convert string from UTF-8");
        return NULL;
    }

    target = calloc(count, sizeof(WCHAR));

    if (!MultiByteToWideChar(CP_UTF8, 0, source, -1, target, count)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to convert string from UTF-8");
        free(target);
        return NULL;
    }

    return target;
}

// Returns a UTF-8 string version of the specified wide string
//
char *
_glfwCreateUTF8FromWideStringWin32(const WCHAR *source) {
    char *target;
    int size;

    size = WideCharToMultiByte(CP_UTF8, 0, source, -1, NULL, 0, NULL, NULL);
    if (!size) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to convert string to UTF-8");
        return NULL;
    }

    target = calloc(size, 1);

    if (!WideCharToMultiByte(CP_UTF8, 0, source, -1, target, size, NULL, NULL)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to convert string to UTF-8");
        free(target);
        return NULL;
    }

    return target;
}

// Reports the specified error, appending information about the last Win32 error
//
void
_glfwInputErrorWin32(int error, const char *description) {
    WCHAR buffer[_GLFW_MESSAGE_SIZE] = L"";
    char message[_GLFW_MESSAGE_SIZE] = "";

    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
        NULL,
        GetLastError() & 0xffff,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        sizeof(buffer) / sizeof(WCHAR),
        NULL);
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, message, sizeof(message), NULL, NULL);

    _glfwInputError(error, "%s: %s", description, message);
}

// Updates key names according to the current keyboard layout
//
void
_glfwUpdateKeyNamesWin32(void) {
    BYTE state[256] = {0};
    memset(_glfw.win32.keynames, 0, sizeof(_glfw.win32.keynames));

    for (int scancode = 0; scancode < 512; scancode++) {
        WCHAR chars[16];
        const uint32_t key = _glfw.win32.keycodes[scancode];
        if (!key || key >= GLFW_FKEY_FIRST) continue;
        UINT vk = MapVirtualKeyW((UINT)scancode, MAPVK_VSC_TO_VK_EX);
        if (!vk) continue;
        int length = ToUnicode(vk, scancode, state, chars, sizeof(chars) / sizeof(WCHAR), 0x4);
        if (length == -1) {
            // This is a dead key, so we need a second simulated key press
            // to make it output its own character (usually a diacritic)
            length = ToUnicode(vk, scancode, state, chars, sizeof(chars) / sizeof(WCHAR), 0x4);
        }
        if (length < 1) continue;
        WideCharToMultiByte(CP_UTF8, 0, chars, 1, _glfw.win32.keynames[scancode], sizeof(_glfw.win32.keynames[scancode]) - 1, NULL, NULL);
    }
}

// Replacement for IsWindowsVersionOrGreater, as we cannot rely on the
// application having a correct embedded manifest
//
BOOL
_glfwIsWindowsVersionOrGreaterWin32(WORD major, WORD minor, WORD sp) {
    OSVERSIONINFOEXW osvi = {sizeof(osvi), major, minor, 0, 0, {0}, sp};
    DWORD mask = VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR;
    ULONGLONG cond = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    cond = VerSetConditionMask(cond, VER_MINORVERSION, VER_GREATER_EQUAL);
    cond = VerSetConditionMask(cond, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    // HACK: Use RtlVerifyVersionInfo instead of VerifyVersionInfoW as the
    //       latter lies unless the user knew to embed a non-default manifest
    //       announcing support for Windows 10 via supportedOS GUID
    return RtlVerifyVersionInfo(&osvi, mask, cond) == 0;
}

// Checks whether we are on at least the specified build of Windows 10
//
BOOL
_glfwIsWindows10BuildOrGreaterWin32(WORD build) {
    OSVERSIONINFOEXW osvi = {sizeof(osvi), 10, 0, build};
    DWORD mask = VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER;
    ULONGLONG cond = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    cond = VerSetConditionMask(cond, VER_MINORVERSION, VER_GREATER_EQUAL);
    cond = VerSetConditionMask(cond, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    // HACK: Use RtlVerifyVersionInfo instead of VerifyVersionInfoW as the
    //       latter lies unless the user knew to embed a non-default manifest
    //       announcing support for Windows 10 via supportedOS GUID
    return RtlVerifyVersionInfo(&osvi, mask, cond) == 0;
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

int
_glfwPlatformInit(bool *supports_window_occlusion) {
    *supports_window_occlusion = false;
    _glfw.win32.mainThreadId = GetCurrentThreadId();
    if (!loadLibraries()) return false;

    // Without this, main loop timeouts (repaint_delay, input_delay, cursor blink,
    // animations) are rounded up to the default 15.6ms system timer tick
    _glfw.win32.timer_period_raised = timeBeginPeriod(1) == TIMERR_NOERROR;

    createKeyTables();
    _glfwUpdateKeyNamesWin32();

    if (_glfwIsWindows10CreatorsUpdateOrGreaterWin32()) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    else if (IsWindows8Point1OrGreater()) SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    else if (IsWindowsVistaOrGreater()) SetProcessDPIAware();

    if (!_glfwRegisterWindowClassWin32()) return false;

    if (!createHelperWindow()) return false;

    _glfwPollMonitorsWin32();
    return true;
}

void
_glfwPlatformTerminate(void) {
    _glfwTerminateVSyncWin32();
    if (_glfw.win32.helperWindowHandle) DestroyWindow(_glfw.win32.helperWindowHandle);

    _glfwUnregisterWindowClassWin32();

    free(_glfw.win32.clipboardString);
    free(_glfw.win32.rawInput);

    _glfwTerminateWGL();
    _glfwTerminateEGL();
    _glfwTerminateOSMesa();

    _glfwFreeTimersWin32();

    if (_glfw.win32.timer_period_raised) timeEndPeriod(1);
    _glfw.win32.timer_period_raised = false;

    freeLibraries();
}

const char *
_glfwPlatformGetVersionString(void) {
    return _GLFW_VERSION_NUMBER " Win32 WGL EGL OSMesa"
#if defined(__MINGW32__)
                                " MinGW"
#elif defined(_MSC_VER)
                                " VisualC"
#endif
#if defined(_GLFW_USE_HYBRID_HPG) || defined(_GLFW_USE_OPTIMUS_HPG)
                                " hybrid-GPU"
#endif
#if defined(_GLFW_BUILD_DLL)
                                " DLL"
#endif
        ;
}

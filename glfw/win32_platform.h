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

#pragma once

// We don't need all the fancy stuff
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// This is a workaround for the fact that glfw3.h needs to export APIENTRY (for
// example to allow applications to correctly declare a GL_KHR_debug callback)
// but windows.h assumes no one will define APIENTRY before it does
#undef APIENTRY

// GLFW on Windows is Unicode only and does not work in MBCS mode
#ifndef UNICODE
#define UNICODE
#endif

// We require Windows 10 or later
#if !defined(WINVER) || WINVER < 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

// GLFW uses OEM cursor resources
#define OEMRESOURCE

#include <wctype.h>
#include <windows.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <imm.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

#include "../kitty/monotonic.h"

// HACK: Define macros that some windows.h variants don't
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef WM_DWMCOMPOSITIONCHANGED
#define WM_DWMCOMPOSITIONCHANGED 0x031E
#endif
#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif
#ifndef WM_COPYGLOBALDATA
#define WM_COPYGLOBALDATA 0x0049
#endif
#ifndef WM_UNICHAR
#define WM_UNICHAR 0x0109
#endif
#ifndef UNICODE_NOCHAR
#define UNICODE_NOCHAR 0xFFFF
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef GET_XBUTTON_WPARAM
#define GET_XBUTTON_WPARAM(w) (HIWORD(w))
#endif
#ifndef EDS_ROTATEDMODE
#define EDS_ROTATEDMODE 0x00000004
#endif
#ifndef DISPLAY_DEVICE_ACTIVE
#define DISPLAY_DEVICE_ACTIVE 0x00000001
#endif
#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02e4
#endif
#ifndef USER_DEFAULT_SCREEN_DPI
#define USER_DEFAULT_SCREEN_DPI 96
#endif
#ifndef OCR_HAND
#define OCR_HAND 32649
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE) - 4)
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
// Undocumented attribute id used by Windows 10 builds 17763 through 18985
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

// Private window messages used by the kitty main loop implementation
#define _GLFW_WM_WAKEUP (WM_APP + 1)
#define _GLFW_WM_TIMER_CHANGED (WM_APP + 2)
// Posted to the helper window by a vertical blank thread, wParam is the source index
#define _GLFW_WM_VBLANK (WM_APP + 5)

// Replacement for versionhelpers.h macros, as we cannot rely on the
// application having a correct embedded manifest
//
#define IsWindowsVistaOrGreater() _glfwIsWindowsVersionOrGreaterWin32(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0)
#define IsWindowsXPOrGreater() _glfwIsWindowsVersionOrGreaterWin32(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0)
#define IsWindows7OrGreater() _glfwIsWindowsVersionOrGreaterWin32(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0)
#define IsWindows8OrGreater() _glfwIsWindowsVersionOrGreaterWin32(HIBYTE(_WIN32_WINNT_WIN8), LOBYTE(_WIN32_WINNT_WIN8), 0)
#define IsWindows8Point1OrGreater() _glfwIsWindowsVersionOrGreaterWin32(HIBYTE(_WIN32_WINNT_WINBLUE), LOBYTE(_WIN32_WINNT_WINBLUE), 0)

#define _glfwIsWindows10AnniversaryUpdateOrGreaterWin32() _glfwIsWindows10BuildOrGreaterWin32(14393)
#define _glfwIsWindows10CreatorsUpdateOrGreaterWin32() _glfwIsWindows10BuildOrGreaterWin32(15063)
#define _glfw_min(a, b) ((a) < (b) ? (a) : (b))
#define _glfw_max(a, b) ((a) > (b) ? (a) : (b))
#define _glfwIsWindows11OrGreaterWin32() _glfwIsWindows10BuildOrGreaterWin32(22000)

// user32.dll function pointer typedefs
typedef BOOL(WINAPI *PFN_SetProcessDPIAware)(void);
typedef BOOL(WINAPI *PFN_ChangeWindowMessageFilterEx)(HWND, UINT, DWORD, CHANGEFILTERSTRUCT *);
typedef BOOL(WINAPI *PFN_EnableNonClientDpiScaling)(HWND);
typedef BOOL(WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef UINT(WINAPI *PFN_GetDpiForWindow)(HWND);
typedef BOOL(WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef int(WINAPI *PFN_GetSystemMetricsForDpi)(int, UINT);
#define SetProcessDPIAware _glfw.win32.user32.SetProcessDPIAware_
#define ChangeWindowMessageFilterEx _glfw.win32.user32.ChangeWindowMessageFilterEx_
#define EnableNonClientDpiScaling _glfw.win32.user32.EnableNonClientDpiScaling_
#define SetProcessDpiAwarenessContext _glfw.win32.user32.SetProcessDpiAwarenessContext_
#define GetDpiForWindow _glfw.win32.user32.GetDpiForWindow_
#define AdjustWindowRectExForDpi _glfw.win32.user32.AdjustWindowRectExForDpi_
#define GetSystemMetricsForDpi _glfw.win32.user32.GetSystemMetricsForDpi_

// dwmapi.dll function pointer typedefs
typedef HRESULT(WINAPI *PFN_DwmIsCompositionEnabled)(BOOL *);
typedef HRESULT(WINAPI *PFN_DwmFlush)(VOID);
typedef HRESULT(WINAPI *PFN_DwmEnableBlurBehindWindow)(HWND, const DWM_BLURBEHIND *);
typedef HRESULT(WINAPI *PFN_DwmGetColorizationColor)(DWORD *, BOOL *);
typedef HRESULT(WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
#define DwmIsCompositionEnabled _glfw.win32.dwmapi.IsCompositionEnabled
#define DwmFlush _glfw.win32.dwmapi.Flush
#define DwmEnableBlurBehindWindow _glfw.win32.dwmapi.EnableBlurBehindWindow
#define DwmGetColorizationColor _glfw.win32.dwmapi.GetColorizationColor
#define DwmSetWindowAttribute _glfw.win32.dwmapi.SetWindowAttribute

// shcore.dll function pointer typedefs
typedef HRESULT(WINAPI *PFN_SetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);
typedef HRESULT(WINAPI *PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT *, UINT *);
#define SetProcessDpiAwareness _glfw.win32.shcore.SetProcessDpiAwareness_
#define GetDpiForMonitor _glfw.win32.shcore.GetDpiForMonitor_

// ntdll.dll function pointer typedefs
typedef LONG(WINAPI *PFN_RtlVerifyVersionInfo)(OSVERSIONINFOEXW *, ULONG, ULONGLONG);
#define RtlVerifyVersionInfo _glfw.win32.ntdll.RtlVerifyVersionInfo_

typedef VkFlags VkWin32SurfaceCreateFlagsKHR;

typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkWin32SurfaceCreateFlagsKHR flags;
    HINSTANCE hinstance;
    HWND hwnd;
} VkWin32SurfaceCreateInfoKHR;

typedef VkResult(APIENTRY *PFN_vkCreateWin32SurfaceKHR)(VkInstance, const VkWin32SurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef VkBool32(APIENTRY *PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)(VkPhysicalDevice, uint32_t);

#include "null_joystick.h"
#include "wgl_context.h"

typedef void (*GLFWwin32renderframefun)(GLFWwindow *);

// A request to render on a later vertical blank of the window's monitor, see win32_vsync.c
typedef struct _GLFWframeRequestWin32 {
    GLFWwin32renderframefun callback;
    // The vertical blank sequence number of the source to fire on
    uint64_t fire_at;
    // When the watchdog fires the request if the vertical blank never arrived
    monotonic_t deadline;
    // How far apart the last request spaced frames, 0 if render frames were never requested
    monotonic_t spacing;
    size_t source;
    bool pending;
} _GLFWframeRequestWin32;

#if !defined(_GLFW_WNDCLASSNAME)
#define _GLFW_WNDCLASSNAME L"Kitsu"
#endif

#define _glfw_dlopen(name) LoadLibraryA(name)
#define _glfw_dlclose(handle) FreeLibrary((HMODULE)handle)
// ISO C forbids casting between function and object pointers, go via a union
static inline void *
_glfw_win32_proc_to_ptr(FARPROC f) {
    union {
        FARPROC f;
        void *p;
    } u = {.f = f};
    return u.p;
}
static inline void *
_glfw_dlsym(void *handle, const char *name) {
    return _glfw_win32_proc_to_ptr(GetProcAddress((HMODULE)handle, name));
}

#define _GLFW_PLATFORM_WINDOW_STATE _GLFWwindowWin32 win32
#define _GLFW_PLATFORM_LIBRARY_WINDOW_STATE _GLFWlibraryWin32 win32
#define _GLFW_PLATFORM_MONITOR_STATE _GLFWmonitorWin32 win32
#define _GLFW_PLATFORM_CURSOR_STATE _GLFWcursorWin32 win32
#define _GLFW_PLATFORM_TLS_STATE _GLFWtlsWin32 win32
#define _GLFW_PLATFORM_MUTEX_STATE _GLFWmutexWin32 win32

// Win32-specific per-window data
//
typedef struct _GLFWwindowWin32 {
    HWND handle;
    HICON bigIcon;
    HICON smallIcon;

    bool cursorTracked;
    bool frameAction;
    bool iconified;
    bool maximized;
    // Whether to enable framebuffer transparency on DWM
    bool transparent;
    bool scaleToMonitor;
    bool mousePassthrough;
    bool keymenu;
    bool live_resize_in_progress;
    int blur_radius;
    int blur_mode;

    // Cached size used to filter out duplicate events
    int width, height;

    // The last received cursor position, regardless of source
    int lastCursorPosX, lastCursorPosY;
    // Mouse moves are delivered at most once per frame, see deliverCursorPos()
    struct {
        bool pending;
        int x, y;
        // When the next mouse move may be delivered straight away
        monotonic_t next_at;
    } motion;
    // The refresh interval of the monitor the window was last on, for when it does not use render frames
    struct {
        HMONITOR monitor;
        monotonic_t interval;
    } refresh;
    // The last received high surrogate when decoding pairs of UTF-16 messages
    WCHAR highSurrogate;

    // Saved window placement for toggling fullscreen
    struct {
        bool active;
        WINDOWPLACEMENT placement;
        DWORD style, exStyle;
    } fullscreen;

    struct {
        bool preedit_active, dead_key_active;
    } ime;

    struct {
        bool active;
        char *uri_list, *plain_text;
        size_t uri_list_len, plain_text_len;
        size_t read_pos[2];
        bool data_requested[2];
    } drop;

    GLFWLayerShellConfig layer_shell_config;
    _GLFWframeRequestWin32 frame_request;
} _GLFWwindowWin32;

typedef struct _GLFWtimerWin32 {
    unsigned long long id;
    monotonic_t interval, trigger_at;
    bool repeats, enabled;
    GLFWuserdatafun callback;
    void *callback_data;
    GLFWuserdatafun free_callback;
} _GLFWtimerWin32;

// Win32-specific global data
//
typedef struct _GLFWlibraryWin32 {
    HINSTANCE instance;
    HWND helperWindowHandle;
    DWORD foregroundLockTimeout;
    int acquiredMonitorCount;
    char *clipboardString;
    // scancode -> kitty key (unicode codepoint or GLFW_FKEY_*), 0 if unmapped
    uint32_t keycodes[512];
    char keynames[512][8];
    // Where to place the cursor when re-enabled
    double restoreCursorPosX, restoreCursorPosY;
    // The window whose disabled cursor mode is active
    _GLFWwindow *disabledCursorWindow;
    // The window the cursor is captured in
    _GLFWwindow *capturedCursorWindow;
    RAWINPUT *rawInput;
    int rawInputSize;
    UINT mouseTrailSize;
    DWORD mainThreadId;

    struct {
        _GLFWtimerWin32 *items;
        size_t count, capacity;
        unsigned long long next_id;
    } timers;
    bool keep_going, wakeup_pending;
    // Whether timeBeginPeriod(1) succeeded and must be undone at exit
    bool timer_period_raised;
    // Delivers mouse moves that arrived too soon after the previous one
    struct {
        unsigned long long id;
        monotonic_t fires_at;
        bool enabled;
    } motion_timer;

    struct {
        HINSTANCE instance;
        PFN_SetProcessDPIAware SetProcessDPIAware_;
        PFN_ChangeWindowMessageFilterEx ChangeWindowMessageFilterEx_;
        PFN_EnableNonClientDpiScaling EnableNonClientDpiScaling_;
        PFN_SetProcessDpiAwarenessContext SetProcessDpiAwarenessContext_;
        PFN_GetDpiForWindow GetDpiForWindow_;
        PFN_AdjustWindowRectExForDpi AdjustWindowRectExForDpi_;
        PFN_GetSystemMetricsForDpi GetSystemMetricsForDpi_;
    } user32;

    struct {
        HINSTANCE instance;
        PFN_DwmIsCompositionEnabled IsCompositionEnabled;
        PFN_DwmFlush Flush;
        PFN_DwmEnableBlurBehindWindow EnableBlurBehindWindow;
        PFN_DwmGetColorizationColor GetColorizationColor;
        PFN_DwmSetWindowAttribute SetWindowAttribute;
    } dwmapi;

    struct {
        HINSTANCE instance;
        PFN_SetProcessDpiAwareness SetProcessDpiAwareness_;
        PFN_GetDpiForMonitor GetDpiForMonitor_;
    } shcore;

    struct {
        HINSTANCE instance;
        PFN_RtlVerifyVersionInfo RtlVerifyVersionInfo_;
    } ntdll;
} _GLFWlibraryWin32;

// Win32-specific per-monitor data
//
typedef struct _GLFWmonitorWin32 {
    HMONITOR handle;
    // This size matches the static size of DISPLAY_DEVICE.DeviceName
    WCHAR adapterName[32];
    WCHAR displayName[32];
    char publicAdapterName[32];
    char publicDisplayName[32];
    bool modesPruned;
    bool modeChanged;
} _GLFWmonitorWin32;

// Win32-specific per-cursor data
//
typedef struct _GLFWcursorWin32 {
    HCURSOR handle;
} _GLFWcursorWin32;

// Win32-specific thread local storage data
//
typedef struct _GLFWtlsWin32 {
    bool allocated;
    DWORD index;
} _GLFWtlsWin32;

// Win32-specific mutex data
//
typedef struct _GLFWmutexWin32 {
    bool allocated;
    CRITICAL_SECTION section;
} _GLFWmutexWin32;


bool _glfwRegisterWindowClassWin32(void);
void _glfwUnregisterWindowClassWin32(void);

WCHAR *_glfwCreateWideStringFromUTF8Win32(const char *source);
char *_glfwCreateUTF8FromWideStringWin32(const WCHAR *source);
BOOL _glfwIsWindowsVersionOrGreaterWin32(WORD major, WORD minor, WORD sp);
BOOL _glfwIsWindows10BuildOrGreaterWin32(WORD build);
void _glfwInputErrorWin32(int error, const char *description);
void _glfwUpdateKeyNamesWin32(void);

void _glfwPollMonitorsWin32(void);
void _glfwSetVideoModeWin32(_GLFWmonitor *monitor, const GLFWvidmode *desired);
void _glfwRestoreVideoModeWin32(_GLFWmonitor *monitor);
void _glfwGetMonitorContentScaleWin32(HMONITOR handle, float *xscale, float *yscale);

void _glfwDispatchTimersWin32(void);
DWORD _glfwTimerWaitTimeoutWin32(void);
void _glfwFreeTimersWin32(void);

void _glfwDispatchVSyncWin32(WPARAM which);
monotonic_t _glfwFrameIntervalWin32(_GLFWwindow *window);
void _glfwVSyncDisplayChangedWin32(void);
void _glfwTerminateVSyncWin32(void);

//========================================================================
// Kitsu Win32 - vertical blank driven render frames
//------------------------------------------------------------------------
// One thread per monitor waits for that monitor's vertical blank using
// D3DKMTWaitForVerticalBlankEvent and tells the main thread about it, so
// that kitty renders at the display's refresh rate instead of on a timer,
// the same way it uses CVDisplayLink on macOS and frame callbacks on Wayland.
//
// When the kernel thunks are unavailable or a wait fails (Remote Desktop,
// display asleep, adapter reset) the thread sleeps for one refresh period
// instead. A main thread watchdog fires any request that is still pending
// well after it was due, so a stuck wait can never stop rendering.
//========================================================================

#include "internal.h"

#include <string.h>

#define VSYNC_MAX_SOURCES 16
#define VSYNC_NO_TARGET UINT64_MAX
#define VSYNC_DEFAULT_INTERVAL (MONOTONIC_T_1e9 / 60)
#define VSYNC_WATCHDOG_INTERVAL ms_to_monotonic_t(50ll)
#define VSYNC_WATCHDOG_GRACE ms_to_monotonic_t(100ll)
#define VSYNC_REOPEN_INTERVAL ms_to_monotonic_t(1000ll)

// From d3dkmthk.h, declared here to avoid depending on the DDK headers
typedef UINT KMT_HANDLE;
typedef struct {
    HDC hDc;
    KMT_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} KMT_OPENADAPTERFROMHDC;
typedef struct {
    KMT_HANDLE hAdapter;
} KMT_CLOSEADAPTER;
typedef struct {
    KMT_HANDLE hAdapter;
    KMT_HANDLE hDevice;
    UINT VidPnSourceId;
} KMT_WAITFORVERTICALBLANKEVENT;
typedef LONG(WINAPI *PFN_KMTOpenAdapterFromHdc)(KMT_OPENADAPTERFROMHDC *);
typedef LONG(WINAPI *PFN_KMTCloseAdapter)(const KMT_CLOSEADAPTER *);
typedef LONG(WINAPI *PFN_KMTWaitForVerticalBlankEvent)(const KMT_WAITFORVERTICALBLANKEVENT *);

typedef struct {
    // Immutable once the thread is started
    WCHAR device[CCHDEVICENAME];
    size_t index;
    HANDLE thread;
    // Main thread only cache, cleared on display changes
    HMONITOR monitor;
    // Protected by vs.lock
    CONDITION_VARIABLE wake;
    uint64_t sequence, target;
    monotonic_t interval;
    bool quit;
} VSyncSource;

static struct {
    SRWLOCK lock;
    VSyncSource sources[VSYNC_MAX_SOURCES];
    size_t count;
    // Bumped on display changes so that threads re-open their adapter, protected by lock
    unsigned generation;

    bool kmt_loaded;
    PFN_KMTOpenAdapterFromHdc OpenAdapterFromHdc;
    PFN_KMTCloseAdapter CloseAdapter;
    PFN_KMTWaitForVerticalBlankEvent WaitForVerticalBlankEvent;

    unsigned long long watchdog_timer;
    bool watchdog_enabled;
} vs = {.lock = SRWLOCK_INIT};

static void
load_kmt(void) {
    if (vs.kmt_loaded) return;
    vs.kmt_loaded = true;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return;
    glfw_dlsym(vs.OpenAdapterFromHdc, gdi, "D3DKMTOpenAdapterFromHdc");
    glfw_dlsym(vs.CloseAdapter, gdi, "D3DKMTCloseAdapter");
    glfw_dlsym(vs.WaitForVerticalBlankEvent, gdi, "D3DKMTWaitForVerticalBlankEvent");
    if (!vs.OpenAdapterFromHdc || !vs.CloseAdapter || !vs.WaitForVerticalBlankEvent) {
        vs.OpenAdapterFromHdc = NULL;
        vs.CloseAdapter = NULL;
        vs.WaitForVerticalBlankEvent = NULL;
    }
}

static monotonic_t
refresh_interval(const WCHAR *device) {
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    // 0 and 1 mean "hardware default" rather than an actual rate
    if (EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return MONOTONIC_T_1e9 / dm.dmDisplayFrequency;
    return VSYNC_DEFAULT_INTERVAL;
}

static KMT_HANDLE
open_adapter(const WCHAR *device, UINT *source_id) {
    if (!vs.OpenAdapterFromHdc) return 0;
    HDC dc = CreateDCW(L"DISPLAY", device, NULL, NULL);
    if (!dc) return 0;
    KMT_OPENADAPTERFROMHDC oa = {.hDc = dc};
    LONG status = vs.OpenAdapterFromHdc(&oa);
    DeleteDC(dc);
    if (status != 0) return 0;
    *source_id = oa.VidPnSourceId;
    return oa.hAdapter;
}

static void
close_adapter(KMT_HANDLE adapter) {
    if (!adapter) return;
    KMT_CLOSEADAPTER ca = {.hAdapter = adapter};
    vs.CloseAdapter(&ca);
}

typedef enum { VBLANK_OK, VBLANK_FAILED, VBLANK_SPURIOUS } VBlankWait;

static VBlankWait
wait_for_vblank(KMT_HANDLE adapter, UINT source_id, monotonic_t interval, monotonic_t last_vblank_at) {
    KMT_WAITFORVERTICALBLANKEVENT wv = {.hAdapter = adapter, .VidPnSourceId = source_id};
    // Some drivers return straight away rather than failing while the display is off,
    // which would render as fast as the main thread can, so a vertical blank that
    // comes far too soon after the previous one does not count
    for (int i = 0; i < 4; i++) {
        if (vs.WaitForVerticalBlankEvent(&wv) != 0) return VBLANK_FAILED;
        if (monotonic() - last_vblank_at >= interval / 2) return VBLANK_OK;
    }
    return VBLANK_SPURIOUS;
}

static DWORD WINAPI
vsync_thread(LPVOID data) {
    VSyncSource *s = data;
    KMT_HANDLE adapter = 0;
    UINT source_id = 0;
    unsigned generation = 0;
    bool opened = false;
    monotonic_t interval = VSYNC_DEFAULT_INTERVAL, reopen_at = 0, last_vblank_at = 0;

    for (;;) {
        AcquireSRWLockExclusive(&vs.lock);
        // Sleep until a window wants a frame from this monitor
        while (!s->quit && s->target == VSYNC_NO_TARGET) SleepConditionVariableSRW(&s->wake, &vs.lock, INFINITE, 0);
        const bool quit = s->quit;
        const unsigned current_generation = vs.generation;
        ReleaseSRWLockExclusive(&vs.lock);
        if (quit) break;

        if (!opened || current_generation != generation) {
            opened = true;
            generation = current_generation;
            close_adapter(adapter);
            adapter = open_adapter(s->device, &source_id);
            reopen_at = monotonic() + VSYNC_REOPEN_INTERVAL;
            interval = refresh_interval(s->device);
            AcquireSRWLockExclusive(&vs.lock);
            s->interval = interval;
            ReleaseSRWLockExclusive(&vs.lock);
        }

        bool waited = false;
        if (adapter) {
            switch (wait_for_vblank(adapter, source_id, interval, last_vblank_at)) {
                case VBLANK_OK: waited = true; break;
                case VBLANK_SPURIOUS: break;
                case VBLANK_FAILED:
                    close_adapter(adapter);
                    adapter = 0;
                    reopen_at = monotonic() + VSYNC_REOPEN_INTERVAL;
                    break;
            }
        }
        if (!waited) {
            const DWORD ms = (DWORD)((interval + MONOTONIC_T_1e6 / 2) / MONOTONIC_T_1e6);
            Sleep(ms ? ms : 1);
            if (!adapter && monotonic() >= reopen_at) {
                adapter = open_adapter(s->device, &source_id);
                reopen_at = monotonic() + VSYNC_REOPEN_INTERVAL;
            }
        }

        last_vblank_at = monotonic();
        AcquireSRWLockExclusive(&vs.lock);
        s->sequence++;
        const bool due = s->sequence >= s->target;
        if (due) s->target = VSYNC_NO_TARGET;
        ReleaseSRWLockExclusive(&vs.lock);
        if (due) PostMessageW(_glfw.win32.helperWindowHandle, _GLFW_WM_VBLANK, (WPARAM)s->index, 0);
    }
    close_adapter(adapter);
    return 0;
}

// Returns the source for the monitor the window is mostly on, starting its thread
// on first use, or NULL if one could not be created
static VSyncSource *
source_for_window(_GLFWwindow *window) {
    HMONITOR monitor = MonitorFromWindow(window->win32.handle, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return NULL;
    for (size_t i = 0; i < vs.count; i++) {
        if (vs.sources[i].monitor == monitor) return vs.sources + i;
    }
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&mi)) return NULL;
    for (size_t i = 0; i < vs.count; i++) {
        if (wcscmp(vs.sources[i].device, mi.szDevice) == 0) {
            vs.sources[i].monitor = monitor;
            return vs.sources + i;
        }
    }
    if (vs.count >= VSYNC_MAX_SOURCES) return NULL;

    load_kmt();
    VSyncSource *s = vs.sources + vs.count;
    memset(s, 0, sizeof(*s));
    wcsncpy(s->device, mi.szDevice, CCHDEVICENAME - 1);
    s->index = vs.count;
    s->monitor = monitor;
    s->target = VSYNC_NO_TARGET;
    s->interval = refresh_interval(s->device);
    InitializeConditionVariable(&s->wake);
    s->thread = CreateThread(NULL, 0, vsync_thread, s, 0, NULL);
    if (!s->thread) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "Win32: Failed to create vertical blank thread");
        return NULL;
    }
    // Waking up late shifts every frame by the same amount, so do not let other work delay it
    SetThreadPriority(s->thread, THREAD_PRIORITY_HIGHEST);
    vs.count++;
    return s;
}

static void
fire_request(_GLFWwindow *window) {
    window->win32.frame_request.pending = false;
    GLFWwin32renderframefun callback = window->win32.frame_request.callback;
    if (callback) callback((GLFWwindow *)window);
}

static bool
any_request_pending(void) {
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) {
        if (w->win32.frame_request.pending) return true;
    }
    return false;
}

static void
watchdog(unsigned long long timer_id UNUSED, void *data UNUSED) {
    const monotonic_t now = monotonic();
    for (_GLFWwindow *w = _glfw.windowListHead, *next; w; w = next) {
        next = w->next;
        if (w->win32.frame_request.pending && now >= w->win32.frame_request.deadline) fire_request(w);
    }
    if (!any_request_pending()) {
        _glfwPlatformUpdateTimer(vs.watchdog_timer, VSYNC_WATCHDOG_INTERVAL, false);
        vs.watchdog_enabled = false;
    }
}

static void
ensure_watchdog(void) {
    if (!vs.watchdog_timer) {
        vs.watchdog_timer = _glfwPlatformAddTimer(VSYNC_WATCHDOG_INTERVAL, true, watchdog, NULL, NULL);
        vs.watchdog_enabled = vs.watchdog_timer != 0;
    } else if (!vs.watchdog_enabled) {
        _glfwPlatformUpdateTimer(vs.watchdog_timer, VSYNC_WATCHDOG_INTERVAL, true);
        vs.watchdog_enabled = true;
    }
}

static void
arm_source(VSyncSource *s, uint64_t fire_at) {
    AcquireSRWLockExclusive(&vs.lock);
    if (fire_at < s->target) {
        s->target = fire_at;
        WakeConditionVariable(&s->wake);
    }
    ReleaseSRWLockExclusive(&vs.lock);
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW internal API                      //////
//////////////////////////////////////////////////////////////////////////

void
_glfwDispatchVSyncWin32(WPARAM which) {
    if (which >= vs.count) return;
    VSyncSource *s = vs.sources + which;
    AcquireSRWLockShared(&vs.lock);
    const uint64_t sequence = s->sequence;
    ReleaseSRWLockShared(&vs.lock);
    uint64_t next_target = VSYNC_NO_TARGET;
    for (_GLFWwindow *w = _glfw.windowListHead, *next; w; w = next) {
        next = w->next;
        const _GLFWframeRequestWin32 *r = &w->win32.frame_request;
        if (!r->pending || r->source != which) continue;
        if (r->fire_at <= sequence) fire_request(w);
        else if (r->fire_at < next_target) next_target = r->fire_at;
    }
    // The thread disarms itself when it posts, re-arm for windows that want a later frame
    if (next_target != VSYNC_NO_TARGET) arm_source(s, next_target);
}

void
_glfwVSyncDisplayChangedWin32(void) {
    AcquireSRWLockExclusive(&vs.lock);
    vs.generation++;
    ReleaseSRWLockExclusive(&vs.lock);
    // HMONITOR values can be reused for a different display after a change
    for (size_t i = 0; i < vs.count; i++) vs.sources[i].monitor = NULL;
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) w->win32.refresh.monitor = NULL;
}

// How often the window renders: the spacing of its render frames, or the refresh
// interval of its monitor when it does not use them
monotonic_t
_glfwFrameIntervalWin32(_GLFWwindow *window) {
    if (window->win32.frame_request.spacing > 0) return window->win32.frame_request.spacing;
    HMONITOR monitor = MonitorFromWindow(window->win32.handle, MONITOR_DEFAULTTONEAREST);
    if (monitor != window->win32.refresh.monitor) {
        window->win32.refresh.monitor = monitor;
        window->win32.refresh.interval = VSYNC_DEFAULT_INTERVAL;
        MONITORINFOEXW mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (monitor && GetMonitorInfoW(monitor, (MONITORINFO *)&mi)) window->win32.refresh.interval = refresh_interval(mi.szDevice);
    }
    return window->win32.refresh.interval;
}

void
_glfwTerminateVSyncWin32(void) {
    HANDLE threads[VSYNC_MAX_SOURCES];
    DWORD num_threads = 0;
    AcquireSRWLockExclusive(&vs.lock);
    for (size_t i = 0; i < vs.count; i++) {
        vs.sources[i].quit = true;
        WakeConditionVariable(&vs.sources[i].wake);
        threads[num_threads++] = vs.sources[i].thread;
    }
    ReleaseSRWLockExclusive(&vs.lock);
    // A thread blocked on a vertical blank returns within one refresh, but a display
    // that went away must not hang shutdown, so the threads are not freed if they time out
    if (num_threads && WaitForMultipleObjects(num_threads, threads, TRUE, 250) != WAIT_TIMEOUT) vs.count = 0;
    for (DWORD i = 0; i < num_threads; i++) CloseHandle(threads[i]);
    // The timer itself is freed along with the rest of the main loop timers
    vs.watchdog_timer = 0;
    vs.watchdog_enabled = false;
}


//////////////////////////////////////////////////////////////////////////
//////                        GLFW native API                       //////
//////////////////////////////////////////////////////////////////////////

GLFWAPI void
glfwWin32RequestRenderFrame(GLFWwindow *handle, monotonic_t min_interval, GLFWwin32renderframefun callback) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    _GLFW_REQUIRE_INIT();
    _GLFWframeRequestWin32 *r = &window->win32.frame_request;
    const monotonic_t now = monotonic();
    r->callback = callback;
    r->pending = true;
    ensure_watchdog();

    VSyncSource *s = source_for_window(window);
    if (!s) {
        // No vertical blank source, so pace on the watchdog alone
        r->source = VSYNC_MAX_SOURCES;
        r->deadline = now + min_interval;
        r->spacing = min_interval;
        return;
    }
    AcquireSRWLockShared(&vs.lock);
    const uint64_t sequence = s->sequence;
    const monotonic_t interval = s->interval > 0 ? s->interval : VSYNC_DEFAULT_INTERVAL;
    ReleaseSRWLockShared(&vs.lock);
    // Render on the first vertical blank that is at least min_interval away, rounded down to a
    // whole number of refreshes (with a little slack for rounding in the reported refresh rate)
    uint64_t refreshes = (uint64_t)((min_interval + interval / 20) / interval);
    if (refreshes < 1) refreshes = 1;
    r->source = s->index;
    r->fire_at = sequence + refreshes;
    r->spacing = (monotonic_t)refreshes * interval;
    r->deadline = now + r->spacing + VSYNC_WATCHDOG_GRACE;
    arm_source(s, r->fire_at);
}

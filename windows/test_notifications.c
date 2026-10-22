/* Native, non-interactive regression tests. No toasts are displayed. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

static QUERY_USER_NOTIFICATION_STATE user_state;
static HRESULT query_result = S_OK;
HRESULT WINAPI
test_query_user_notification_state(QUERY_USER_NOTIFICATION_STATE *state) {
    *state = user_state;
    return query_result;
}
#define SHQueryUserNotificationState test_query_user_notification_state
#include "../kitty/win32-notifications.c"
#undef SHQueryUserNotificationState
#include <assert.h>

void wakeup_main_loop(void) {}
void register_at_exit_cleanup_func(AtExitCleanupFunc which, kitty_cleanup_at_exit_func func) { (void)which; (void)func; }

static HRESULT setting_result = S_OK;
static NotificationSetting setting_value = NotificationSetting_Enabled;
static HRESULT STDMETHODCALLTYPE
test_setting(IToastNotifier *self, NotificationSetting *value) {
    (void)self;
    *value = setting_value;
    return setting_result;
}

static void
test_first_notification_settings(void) {
    IToastNotifierVtbl vtable = {.get_Setting = test_setting};
    IToastNotifier fake = {&vtable};
    notifier = &fake;
    bool recheck = false;
    assert(SUCCEEDED(check_notification_setting(&recheck)) && !recheck);
    setting_result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    assert(SUCCEEDED(check_notification_setting(&recheck)) && recheck);
    assert(check_notification_setting(NULL) == setting_result);
    setting_result = S_OK;
    assert(SUCCEEDED(check_notification_setting(NULL)));
    for (int i = NotificationSetting_DisabledForApplication; i <= NotificationSetting_DisabledByManifest; i++) {
        setting_value = (NotificationSetting)i;
        recheck = false;
        assert(check_notification_setting(&recheck) == E_ACCESSDENIED && !recheck);
        assert(check_notification_setting(NULL) == E_ACCESSDENIED);
    }
    setting_result = E_FAIL;
    assert(check_notification_setting(&recheck) == E_FAIL);
    notifier = NULL;
}

static void
python(const char *code) { assert(PyRun_SimpleString(code) == 0); }

static unsigned show_count;
static HRESULT show_result = S_OK;
static HRESULT STDMETHODCALLTYPE
test_show(IToastNotifier *self, IToastNotification *toast) {
    (void)self;
    assert(toast);
    show_count++;
    return show_result;
}

static void
test_notification_delivery(void) {
    IToastNotifierVtbl vtable = {.Show = test_show, .get_Setting = test_setting};
    IToastNotifier fake = {&vtable};
    notifier = &fake;
    setting_result = S_OK;
    setting_value = NotificationSetting_Enabled;
    PyObject *args = Py_BuildValue("(sss)", "73", "Build finished", "Ready");
    assert(args);
    const QUERY_USER_NOTIFICATION_STATE states[] = {
        QUNS_NOT_PRESENT, QUNS_BUSY, QUNS_RUNNING_D3D_FULL_SCREEN,
        QUNS_PRESENTATION_MODE, QUNS_QUIET_TIME, QUNS_ACCEPTS_NOTIFICATIONS, QUNS_APP,
    };
    for (unsigned i = 0; i <= sizeof(states) / sizeof(states[0]); i++) {
        if (i < sizeof(states) / sizeof(states[0])) user_state = states[i];
        else query_result = E_FAIL;
        unsigned before = show_count;
        PyObject *result = win32_send_notification(NULL, args, NULL);
        assert(result && !PyErr_Occurred());
        Py_DECREF(result);
        assert(show_count == before + 1);
        drain_win32_notification_events();
        python("assert seen == [('created', '73', '')], seen\nseen.clear()");
        LiveToast *t = take_toast(L"73");
        assert(t);
        release_toast(t);
    }
    query_result = S_OK;
    user_state = QUNS_NOT_PRESENT;
    for (int i = NotificationSetting_DisabledForApplication; i <= NotificationSetting_DisabledByManifest; i++) {
        setting_value = (NotificationSetting)i;
        unsigned before = show_count;
        PyObject *result = win32_send_notification(NULL, args, NULL);
        assert(result && !PyErr_Occurred());
        Py_DECREF(result);
        assert(show_count == before);
        drain_win32_notification_events();
        python("assert seen == [('creation_failed', '73', '')], seen\nseen.clear()");
        assert(!live_toasts);
    }
    setting_value = NotificationSetting_Enabled;
    show_result = E_FAIL;
    unsigned before = show_count;
    PyObject *result = win32_send_notification(NULL, args, NULL);
    assert(result && !PyErr_Occurred());
    Py_DECREF(result);
    assert(show_count == before + 1);
    drain_win32_notification_events();
    python("assert seen == [('creation_failed', '73', '')], seen\nseen.clear()");
    assert(!live_toasts);
    show_result = S_OK;
    Py_DECREF(args);
    notifier = NULL;
}

static void
remember_toast(const wchar_t *ident, IToastNotification *toast) {
    LiveToast *t = calloc(1, sizeof(*t));
    assert(t);
    t->ident = _wcsdup(ident);
    t->toast = toast;
    IToastNotification_AddRef(toast);
    assert(SUCCEEDED(attach_handlers(t)));
    t->next = live_toasts;
    live_toasts = t;
}

static DWORD WINAPI
foreign_thread(void *unused) {
    (void)unused;
    queue_event(ACTIVATED, L"42", L"0");
    return 0;
}

int
main(void) {
    test_first_notification_settings();
    Py_Initialize();
    assert(SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED)));
    ro_initialized = true;
    python("import threading\nseen = []\ndef cb(*args):\n assert threading.current_thread() is threading.main_thread()\n seen.append(args)\n");
    callback = Py_NewRef(PyDict_GetItemString(PyModule_GetDict(PyImport_AddModule("__main__")), "cb"));
    test_notification_delivery();
    PyObject *buttons = Py_BuildValue("(ss)", "&Yes", "<No>");
    assert(buttons);
    wchar_t *xml = build_xml(L"42", L"<&\"' title\x01", L"body > end", L"", L"C:\\a b\\icon.png", buttons, true);
    assert(xml);
    assert(wcsstr(xml, L"&lt;&amp;&quot;&apos; title</text>"));
    assert(wcsstr(xml, L"body &gt; end"));
    assert(wcsstr(xml, L"content=\"&amp;Yes\""));
    assert(wcsstr(xml, L"content=\"&lt;No&gt;\""));
    assert(wcsstr(xml, L":42:2\""));
    assert(wcsstr(xml, L"file:///C:/a%20b/icon.png"));
    assert(wcsstr(xml, L"<audio silent=\"true\"/>"));
    IToastNotification *toast = NULL;
    assert(SUCCEEDED(create_toast(xml, &toast)));
    free(xml);
    xml = build_xml(L"1", L"Update", L"Ready", L"https://example.org/?x=1&y=2", L"", buttons, false);
    assert(xml);
    assert(wcsstr(xml, L"activationType=\"protocol\" launch=\"https://example.org/?x=1&amp;y=2\""));
    assert(!wcsstr(xml, L"<actions>"));
    assert(!wcsstr(xml, L"<audio"));
    free(xml);
    Py_DECREF(buttons);

    remember_toast(L"42", toast);
    queue_event(CREATED, L"42", L"");
    drain_win32_notification_events();
    python("assert seen == [('created', '42', '')], seen\nseen.clear()");
    HANDLE thread = CreateThread(NULL, 0, foreign_thread, NULL, 0, NULL);
    assert(thread);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    queue_event(ACTIVATED, L"42", L"0");
    python("assert not seen");
    drain_win32_notification_events();
    python("assert seen == [('activated', '42', '0')], seen\nseen.clear()");
    assert(!live_toasts);

    remember_toast(L"42", toast);
    queue_event(CREATION_FAILED, L"42", L"");
    queue_event(CREATED, L"42", L"");
    drain_win32_notification_events();
    python("assert seen == [('creation_failed', '42', '')], seen\nseen.clear()");

    remember_toast(L"42", toast);
    queue_event(CREATED, L"42", L"");
    queue_event(CREATION_FAILED, L"42", L"");
    drain_win32_notification_events();
    python("assert seen == [('created', '42', ''), ('creation_failed', '42', '')], seen\nseen.clear()");

    WNDCLASSW wc = {.lpfnWndProc = activation_wndproc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = WINDOW_CLASS};
    assert(RegisterClassW(&wc));
    wchar_t name[64], args[64];
    swprintf(name, 64, L"KitsuNotifications-%lu", GetCurrentProcessId());
    activation_window = CreateWindowExW(0, WINDOW_CLASS, name, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    assert(activation_window);
    remember_toast(L"42", toast);
    swprintf(args, 64, L"%lu:42:2", GetCurrentProcessId());
    assert(SUCCEEDED(activator_activate(&activator, AUMID, args, NULL, 0)));
    drain_win32_notification_events();
    python("assert seen == [('button', '42', '2')], seen\nseen.clear()");

    remember_toast(L"42", toast);
    queue_event(CLOSED, L"42", L"");
    queue_event(CLOSED, L"42", L"");
    drain_win32_notification_events();
    python("assert seen == [('closed', '42', '')], seen");
    IToastNotification_Release(toast);
    cleanup_notifications();
    Py_Finalize();
    puts("Native notification delivery, XML, main-thread dispatch, failure ordering, activation routing and close tests passed");
    return 0;
}

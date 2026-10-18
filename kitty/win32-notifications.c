/*
 * Native desktop toasts for unpackaged Kitsu builds.
 * Distributed under terms of the GPL3 license.
 */
#ifdef _WIN32
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#define WIDL_using_Windows_UI_Notifications
#define WIDL_using_Windows_Data_Xml_Dom
#define WIDL_using_Windows_Foundation
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <notificationactivationcallback.h>
#include <stdbool.h>
#include "cleanup.h"

extern void wakeup_main_loop(void);

#define AUMID L"fhaze.Kitsu"
#define ACTIVATOR L"{03BBCE7E-E7AF-4CA4-A8A7-526597F30652}"
#define WINDOW_CLASS L"KitsuNotificationActivation"
DEFINE_GUID(CLSID_KitsuToastActivator, 0x03bbce7e, 0xe7af, 0x4ca4, 0xa8, 0xa7, 0x52, 0x65, 0x97, 0xf3, 0x06, 0x52);

typedef enum { CREATED, CREATION_FAILED, ACTIVATED, BUTTON, CLOSED } EventKind;
static const char *event_names[] = {"created", "creation_failed", "activated", "button", "closed"};
typedef struct Event {
    struct Event *next;
    EventKind kind;
    wchar_t *ident, *button;
} Event;
static SRWLOCK event_lock = SRWLOCK_INIT;
static Event *events = NULL, *last_event = NULL;
static bool shutting_down = false, ro_initialized = false;
static PyObject *callback = NULL;
static IToastNotifier *notifier = NULL;
static DWORD class_cookie = 0;
static HWND activation_window = NULL;

static void
queue_event(EventKind kind, const wchar_t *ident, const wchar_t *button) {
    Event *e = calloc(1, sizeof(*e));
    if (!e) return;
    e->kind = kind;
    e->ident = _wcsdup(ident);
    e->button = _wcsdup(button ? button : L"");
    if (!e->ident || !e->button) { free(e->ident); free(e->button); free(e); return; }
    AcquireSRWLockExclusive(&event_lock);
    if (!shutting_down) {
        if (last_event) last_event->next = e;
        else events = e;
        last_event = e;
        wakeup_main_loop();
    } else { free(e->ident); free(e->button); free(e); }
    ReleaseSRWLockExclusive(&event_lock);
}

typedef ITypedEventHandler_ToastNotification_IInspectable ActivatedHandler;
typedef ITypedEventHandler_ToastNotification_ToastDismissedEventArgs DismissedHandler;
typedef ITypedEventHandler_ToastNotification_ToastFailedEventArgs FailedHandler;
typedef ITypedEventHandler_ToastNotification_IInspectableVtbl ActivatedHandlerVtbl;
typedef ITypedEventHandler_ToastNotification_ToastDismissedEventArgsVtbl DismissedHandlerVtbl;
typedef ITypedEventHandler_ToastNotification_ToastFailedEventArgsVtbl FailedHandlerVtbl;
typedef struct {
    union { ActivatedHandler activated; DismissedHandler dismissed; FailedHandler failed; } iface;
    LONG refs;
    const IID *iid;
    wchar_t *ident;
} Handler;

static HRESULT
handler_query(Handler *h, REFIID iid, void **out) {
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IAgileObject) && !IsEqualIID(iid, h->iid)) return E_NOINTERFACE;
    *out = h;
    InterlockedIncrement(&h->refs);
    return S_OK;
}

static ULONG
handler_release(Handler *h) {
    LONG refs = InterlockedDecrement(&h->refs);
    if (!refs) { free(h->ident); free(h); }
    return refs;
}

static HRESULT
on_activated(Handler *h, IInspectable *args) {
    IToastActivatedEventArgs *a = NULL;
    HSTRING text = NULL;
    const wchar_t *button = L"";
    if (args && SUCCEEDED(IInspectable_QueryInterface(args, &IID_IToastActivatedEventArgs, (void**)&a))) {
        if (SUCCEEDED(IToastActivatedEventArgs_get_Arguments(a, &text))) {
            const wchar_t *raw = WindowsGetStringRawBuffer(text, NULL);
            // Foreground arguments contain the owning process, identifier and button.
            const wchar_t *p = wcschr(raw, L':');
            if (p && (p = wcschr(p + 1, L':'))) button = p + 1;
        }
    }
    queue_event(*button && wcscmp(button, L"0") ? BUTTON : ACTIVATED, h->ident, button);
    WindowsDeleteString(text);
    if (a) IToastActivatedEventArgs_Release(a);
    return S_OK;
}

static HRESULT
on_dismissed(Handler *h, IToastDismissedEventArgs *args) {
    ToastDismissalReason reason;
    // A timed-out popup remains actionable in Notification Center.
    if (SUCCEEDED(IToastDismissedEventArgs_get_Reason(args, &reason)) && reason != ToastDismissalReason_TimedOut)
        queue_event(CLOSED, h->ident, L"");
    return S_OK;
}

static HRESULT
on_failed(Handler *h, IToastFailedEventArgs *args) {
    (void)args;
    queue_event(CREATION_FAILED, h->ident, L"");
    return S_OK;
}

#define HANDLER_FUNCS(prefix, interface_type, arg_type, invoke_func) \
static HRESULT STDMETHODCALLTYPE prefix##_query(interface_type *self, REFIID iid, void **out) { return handler_query((Handler*)self, iid, out); } \
static ULONG STDMETHODCALLTYPE prefix##_addref(interface_type *self) { return InterlockedIncrement(&((Handler*)self)->refs); } \
static ULONG STDMETHODCALLTYPE prefix##_release(interface_type *self) { return handler_release((Handler*)self); } \
static HRESULT STDMETHODCALLTYPE prefix##_invoke(interface_type *self, IToastNotification *sender, arg_type *args) { \
    (void)sender; return invoke_func((Handler*)self, args); \
} \
static interface_type##Vtbl prefix##_vtable = {prefix##_query, prefix##_addref, prefix##_release, prefix##_invoke};
HANDLER_FUNCS(activated, ActivatedHandler, IInspectable, on_activated)
HANDLER_FUNCS(dismissed, DismissedHandler, IToastDismissedEventArgs, on_dismissed)
HANDLER_FUNCS(failed, FailedHandler, IToastFailedEventArgs, on_failed)
#undef HANDLER_FUNCS

typedef struct LiveToast {
    struct LiveToast *next;
    wchar_t *ident;
    IToastNotification *toast;
    EventRegistrationToken activated, dismissed, failed;
} LiveToast;
static LiveToast *live_toasts = NULL;

static void
release_toast(LiveToast *t) {
    IToastNotification_remove_Activated(t->toast, t->activated);
    IToastNotification_remove_Dismissed(t->toast, t->dismissed);
    IToastNotification_remove_Failed(t->toast, t->failed);
    IToastNotification_Release(t->toast);
    free(t->ident);
    free(t);
}

static LiveToast *
take_toast(const wchar_t *ident) {
    for (LiveToast **p = &live_toasts; *p; p = &(*p)->next) {
        if (!wcscmp((*p)->ident, ident)) {
            LiveToast *t = *p;
            *p = t->next;
            return t;
        }
    }
    return NULL;
}

void
drain_win32_notification_events(void) {
    AcquireSRWLockExclusive(&event_lock);
    Event *e = events;
    events = last_event = NULL;
    ReleaseSRWLockExclusive(&event_lock);
    while (e) {
        Event *next = e->next;
        LiveToast *t = NULL;
        bool deliver = true;
        if (e->kind == CREATED) {
            LiveToast *live = live_toasts;
            while (live && wcscmp(live->ident, e->ident)) live = live->next;
            deliver = live != NULL;
        }
        if (e->kind == ACTIVATED || e->kind == BUTTON || e->kind == CLOSED || e->kind == CREATION_FAILED) {
            t = take_toast(e->ident);
            // Activated can be raised by both the COM activator and WinRT.
            if (!t && (e->kind == ACTIVATED || e->kind == BUTTON || e->kind == CLOSED)) deliver = false;
        }
        if (deliver && callback) {
            PyObject *ident = PyUnicode_FromWideChar(e->ident, -1), *button = PyUnicode_FromWideChar(e->button, -1);
            PyObject *result = ident && button ? PyObject_CallFunction(callback, "sOO", event_names[e->kind], ident, button) : NULL;
            if (!result) PyErr_WriteUnraisable(callback);
            Py_XDECREF(result);
            Py_XDECREF(ident); Py_XDECREF(button);
        }
        if (t) release_toast(t);
        free(e->ident); free(e->button); free(e);
        e = next;
    }
}

static LRESULT CALLBACK
activation_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *data = (const COPYDATASTRUCT*)lparam;
        if (!data || data->dwData != 1 || data->cbData < sizeof(wchar_t) || data->cbData > 512 || data->cbData % sizeof(wchar_t)) return FALSE;
        const wchar_t *args = data->lpData;
        if (!args || args[data->cbData / sizeof(wchar_t) - 1]) return FALSE;
        unsigned long pid = 0, button = 0;
        wchar_t ident[17] = {0}, button_id[16];
        if (swscanf(args, L"%lu:%16l[0123456789]:%lu", &pid, ident, &button) != 3 || pid != GetCurrentProcessId() || button > 5) return FALSE;
        swprintf(button_id, 16, L"%lu", button);
        queue_event(button ? BUTTON : ACTIVATED, ident, button_id);
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HRESULT STDMETHODCALLTYPE
activator_query(INotificationActivationCallback *self, REFIID iid, void **out) {
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_INotificationActivationCallback) && !IsEqualIID(iid, &IID_IAgileObject))
        return E_NOINTERFACE;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE activator_ref(INotificationActivationCallback *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE
activator_activate(INotificationActivationCallback *self, LPCWSTR app, LPCWSTR args, const NOTIFICATION_USER_INPUT_DATA *data, ULONG count) {
    (void)self; (void)data; (void)count;
    if (wcscmp(app, AUMID) || wcslen(args) > 200) return E_INVALIDARG;
    unsigned long pid;
    if (swscanf(args, L"%lu:", &pid) != 1) return E_INVALIDARG;
    wchar_t name[64];
    swprintf(name, 64, L"KitsuNotifications-%lu", pid);
    HWND hwnd = FindWindowExW(HWND_MESSAGE, NULL, WINDOW_CLASS, name);
    if (hwnd) {
        DWORD actual_pid = 0;
        GetWindowThreadProcessId(hwnd, &actual_pid);
        if (actual_pid == pid) {
            AllowSetForegroundWindow(pid);
            COPYDATASTRUCT payload = {.dwData = 1, .cbData = (DWORD)((wcslen(args) + 1) * sizeof(wchar_t)), .lpData = (void*)args};
            DWORD_PTR result;
            SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, (LPARAM)&payload, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
        }
    }
    return S_OK;
}
static INotificationActivationCallbackVtbl activator_vtable = {activator_query, activator_ref, activator_ref, activator_activate};
static INotificationActivationCallback activator = {&activator_vtable};
static HRESULT STDMETHODCALLTYPE
factory_query(IClassFactory *self, REFIID iid, void **out) {
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IClassFactory)) return E_NOINTERFACE;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE factory_ref(IClassFactory *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE
factory_create(IClassFactory *self, IUnknown *outer, REFIID iid, void **out) {
    (void)self;
    if (outer) { *out = NULL; return CLASS_E_NOAGGREGATION; }
    return activator_query(&activator, iid, out);
}
static HRESULT STDMETHODCALLTYPE factory_lock(IClassFactory *self, BOOL lock) { (void)self; (void)lock; return S_OK; }
static IClassFactoryVtbl factory_vtable = {factory_query, factory_ref, factory_ref, factory_create, factory_lock};
static IClassFactory class_factory = {&factory_vtable};

static HRESULT
make_string(const wchar_t *value, HSTRING *out) { return WindowsCreateString(value, (UINT32)wcslen(value), out); }

static HRESULT
register_app(void) {
    HKEY key;
    LSTATUS err = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\AppUserModelId\\" AUMID, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    err = RegSetValueExW(key, L"DisplayName", 0, REG_SZ, (const BYTE*)L"Kitsu", sizeof(L"Kitsu"));
    if (!err) err = RegSetValueExW(key, L"CustomActivator", 0, REG_SZ, (const BYTE*)ACTIVATOR, sizeof(ACTIVATOR));
    PyObject *constants = PyImport_ImportModule("kitty.constants");
    PyObject *icon_obj = constants ? PyObject_GetAttrString(constants, "logo_png_file") : NULL;
    wchar_t *icon = icon_obj ? PyUnicode_AsWideCharString(icon_obj, NULL) : NULL;
    if (!icon) err = ERROR_FILE_NOT_FOUND;
    else if (!err) err = RegSetValueExW(key, L"IconUri", 0, REG_SZ, (const BYTE*)icon, (DWORD)((wcslen(icon) + 1) * sizeof(wchar_t)));
    PyMem_Free(icon); Py_XDECREF(icon_obj); Py_XDECREF(constants);
    RegCloseKey(key);
    if (err) return HRESULT_FROM_WIN32(err);
    WNDCLASSW wc = {.lpfnWndProc = activation_wndproc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = WINDOW_CLASS};
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t name[64];
    swprintf(name, 64, L"KitsuNotifications-%lu", GetCurrentProcessId());
    activation_window = CreateWindowExW(0, WINDOW_CLASS, name, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!activation_window) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = CoRegisterClassObject(&CLSID_KitsuToastActivator, (IUnknown*)&class_factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &class_cookie);
    if (FAILED(hr)) { DestroyWindow(activation_window); activation_window = NULL; }
    return hr;
}

static HRESULT
ensure_notifier(void) {
    if (notifier) return S_OK;
    HRESULT hr = S_OK;
    if (!ro_initialized) {
        hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;
        ro_initialized = SUCCEEDED(hr);
    }
    if (!activation_window) { hr = register_app(); if (FAILED(hr)) return hr; }
    HSTRING klass = NULL, app = NULL;
    IToastNotificationManagerStatics *manager = NULL;
    hr = make_string(L"Windows.UI.Notifications.ToastNotificationManager", &klass);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(klass, &IID_IToastNotificationManagerStatics, (void**)&manager);
    if (SUCCEEDED(hr)) hr = make_string(AUMID, &app);
    if (SUCCEEDED(hr)) hr = IToastNotificationManagerStatics_CreateToastNotifierWithId(manager, app, &notifier);
    if (manager) IToastNotificationManagerStatics_Release(manager);
    WindowsDeleteString(app); WindowsDeleteString(klass);
    return hr;
}

typedef struct { wchar_t *text; size_t used, capacity; } Xml;
static bool
append(Xml *xml, const wchar_t *text, bool escape) {
    while (*text) {
        wchar_t c = *text++;
        const wchar_t *replacement = NULL;
        if (escape) {
            switch (c) {
                case L'&': replacement = L"&amp;"; break;
                case L'<': replacement = L"&lt;"; break;
                case L'>': replacement = L"&gt;"; break;
                case L'"': replacement = L"&quot;"; break;
                case L'\'': replacement = L"&apos;"; break;
            }
            if ((c < 32 && c != L'\n' && c != L'\r' && c != L'\t') || c == 0xfffe || c == 0xffff) continue;
        }
        size_t count = replacement ? wcslen(replacement) : 1;
        if (xml->used + count + 1 > xml->capacity) {
            size_t capacity = (xml->used + count + 1) * 2;
            wchar_t *n = realloc(xml->text, capacity * sizeof(wchar_t));
            if (!n) return false;
            xml->text = n; xml->capacity = capacity;
        }
        if (replacement) memcpy(xml->text + xml->used, replacement, count * sizeof(wchar_t));
        else xml->text[xml->used] = c;
        xml->used += count;
        xml->text[xml->used] = 0;
    }
    return true;
}

static wchar_t *
build_xml(const wchar_t *ident, const wchar_t *title, const wchar_t *body, const wchar_t *url, const wchar_t *image, PyObject *buttons, bool silent) {
    Xml xml = {0};
    wchar_t args[128];
    swprintf(args, 128, L"%lu:%ls:0", GetCurrentProcessId(), ident);
#define A(text) do { if (!append(&xml, text, false)) goto fail; } while (0)
#define E(text) do { if (!append(&xml, text, true)) goto fail; } while (0)
    A(*url ? L"<toast activationType=\"protocol\" launch=\"" : L"<toast activationType=\"foreground\" launch=\"");
    E(*url ? url : args);
    A(L"\"><visual><binding template=\"ToastGeneric\"><text>"); E(title); A(L"</text><text>"); E(body); A(L"</text>");
    if (*image) {
        wchar_t uri[32768]; DWORD length = 32768;
        if (SUCCEEDED(UrlCreateFromPathW(image, uri, &length, 0))) {
            A(L"<image placement=\"appLogoOverride\" src=\""); E(uri); A(L"\"/>");
        }
    }
    A(L"</binding></visual>");
    if (!*url && PyTuple_GET_SIZE(buttons)) {
        A(L"<actions>");
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(buttons) && i < 5; i++) {
            wchar_t *label = PyUnicode_AsWideCharString(PyTuple_GET_ITEM(buttons, i), NULL);
            if (!label) goto fail;
            bool ok = append(&xml, L"<action activationType=\"foreground\" content=\"", false) && append(&xml, label, true);
            PyMem_Free(label);
            if (!ok) goto fail;
            swprintf(args, 128, L"%lu:%ls:%u", GetCurrentProcessId(), ident, (unsigned)i + 1);
            A(L"\" arguments=\""); E(args); A(L"\"/>");
        }
        A(L"</actions>");
    }
    if (silent) A(L"<audio silent=\"true\"/>");
    A(L"</toast>");
#undef A
#undef E
    return xml.text;
fail:
    free(xml.text);
    if (!PyErr_Occurred()) PyErr_NoMemory();
    return NULL;
}

static HRESULT
create_toast(const wchar_t *xml, IToastNotification **toast) {
    HSTRING klass = NULL, content = NULL;
    IInspectable *instance = NULL;
    IXmlDocument *doc = NULL;
    IXmlDocumentIO *io = NULL;
    IToastNotificationFactory *factory = NULL;
    HRESULT hr = make_string(L"Windows.Data.Xml.Dom.XmlDocument", &klass);
    if (SUCCEEDED(hr)) hr = RoActivateInstance(klass, &instance);
    if (SUCCEEDED(hr)) hr = IInspectable_QueryInterface(instance, &IID_IXmlDocument, (void**)&doc);
    if (SUCCEEDED(hr)) hr = IInspectable_QueryInterface(instance, &IID_IXmlDocumentIO, (void**)&io);
    if (SUCCEEDED(hr)) hr = make_string(xml, &content);
    if (SUCCEEDED(hr)) hr = IXmlDocumentIO_LoadXml(io, content);
    WindowsDeleteString(klass); klass = NULL;
    if (SUCCEEDED(hr)) hr = make_string(L"Windows.UI.Notifications.ToastNotification", &klass);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(klass, &IID_IToastNotificationFactory, (void**)&factory);
    if (SUCCEEDED(hr)) hr = IToastNotificationFactory_CreateToastNotification(factory, doc, toast);
    if (factory) IToastNotificationFactory_Release(factory);
    if (io) IXmlDocumentIO_Release(io);
    if (doc) IXmlDocument_Release(doc);
    if (instance) IInspectable_Release(instance);
    WindowsDeleteString(content); WindowsDeleteString(klass);
    return hr;
}

static HRESULT
set_tag(IToastNotification *toast, const wchar_t *ident, bool protocol) {
    IToastNotification2 *toast2 = NULL;
    HSTRING tag = NULL, group = NULL;
    wchar_t pid[32]; swprintf(pid, 32, L"%lu", GetCurrentProcessId());
    HRESULT hr = IToastNotification_QueryInterface(toast, &IID_IToastNotification2, (void**)&toast2);
    if (SUCCEEDED(hr)) hr = make_string(protocol ? L"update" : ident, &tag);
    if (SUCCEEDED(hr)) hr = make_string(protocol ? L"updates" : pid, &group);
    if (SUCCEEDED(hr)) hr = IToastNotification2_put_Tag(toast2, tag);
    if (SUCCEEDED(hr)) hr = IToastNotification2_put_Group(toast2, group);
    if (toast2) IToastNotification2_Release(toast2);
    WindowsDeleteString(tag); WindowsDeleteString(group);
    return hr;
}

static HRESULT
attach_handlers(LiveToast *t) {
    HRESULT hr = S_OK;
    for (unsigned i = 0; i < 3 && SUCCEEDED(hr); i++) {
        Handler *h = calloc(1, sizeof(*h));
        if (!h) return E_OUTOFMEMORY;
        h->refs = 1; h->ident = _wcsdup(t->ident);
        if (!h->ident) { free(h); return E_OUTOFMEMORY; }
        if (i == 0) {
            h->iface.activated.lpVtbl = &activated_vtable;
            h->iid = &IID_ITypedEventHandler_ToastNotification_IInspectable;
            hr = IToastNotification_add_Activated(t->toast, &h->iface.activated, &t->activated);
        } else if (i == 1) {
            h->iface.dismissed.lpVtbl = &dismissed_vtable;
            h->iid = &IID_ITypedEventHandler_ToastNotification_ToastDismissedEventArgs;
            hr = IToastNotification_add_Dismissed(t->toast, &h->iface.dismissed, &t->dismissed);
        } else {
            h->iface.failed.lpVtbl = &failed_vtable;
            h->iid = &IID_ITypedEventHandler_ToastNotification_ToastFailedEventArgs;
            hr = IToastNotification_add_Failed(t->toast, &h->iface.failed, &t->failed);
        }
        handler_release(h);
    }
    return hr;
}

static PyObject *
win32_send_notification(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    PyObject *ident_obj, *title_obj, *body_obj, *url_obj = NULL, *image_obj = NULL, *buttons = NULL;
    int silent = 0;
    static char *names[] = {"ident", "title", "body", "launch_url", "image_path", "buttons", "silent", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "UUU|UUO!p", names, &ident_obj, &title_obj, &body_obj, &url_obj, &image_obj, &PyTuple_Type, &buttons, &silent)) return NULL;
    wchar_t *ident = PyUnicode_AsWideCharString(ident_obj, NULL), *title = PyUnicode_AsWideCharString(title_obj, NULL);
    wchar_t *body = PyUnicode_AsWideCharString(body_obj, NULL), *url = url_obj ? PyUnicode_AsWideCharString(url_obj, NULL) : NULL;
    wchar_t *image = image_obj ? PyUnicode_AsWideCharString(image_obj, NULL) : NULL;
    wchar_t *xml = NULL;
    if (!buttons) buttons = PyTuple_New(0); else Py_INCREF(buttons);
    if (!ident || !title || !body || (url_obj && !url) || (image_obj && !image) || !buttons) goto done;
    if (!*ident || wcslen(ident) > 16 || wcsspn(ident, L"0123456789") != wcslen(ident)) {
        PyErr_SetString(PyExc_ValueError, "Notification identifier must contain 1-16 decimal digits"); goto done;
    }
    xml = build_xml(ident, title, body, url ? url : L"", image ? image : L"", buttons, silent);
    if (!xml) goto done;
    HRESULT hr = ensure_notifier();
    NotificationSetting setting;
    if (SUCCEEDED(hr)) hr = IToastNotifier_get_Setting(notifier, &setting);
    if (SUCCEEDED(hr) && setting != NotificationSetting_Enabled) hr = E_ACCESSDENIED;
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(hr) && SUCCEEDED(SHQueryUserNotificationState(&state)) && state != QUNS_ACCEPTS_NOTIFICATIONS && state != QUNS_APP) hr = E_ACCESSDENIED;
    LiveToast *t = NULL;
    if (SUCCEEDED(hr)) {
        t = calloc(1, sizeof(*t));
        if (!t) hr = E_OUTOFMEMORY;
        else {
            t->ident = _wcsdup(ident);
            hr = t->ident ? create_toast(xml, &t->toast) : E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) hr = set_tag(t->toast, ident, url && *url);
    if (SUCCEEDED(hr)) hr = attach_handlers(t);
    if (SUCCEEDED(hr)) hr = IToastNotifier_Show(notifier, t->toast);
    if (SUCCEEDED(hr)) {
        LiveToast *old = take_toast(ident);
        if (old) release_toast(old);
        t->next = live_toasts; live_toasts = t; t = NULL;
        // Bound retained COM objects without removing persisted toasts.
        unsigned count = 0;
        for (LiveToast **p = &live_toasts; *p; p = &(*p)->next) {
            if (++count > 128) { LiveToast *oldest = *p; *p = NULL; release_toast(oldest); break; }
        }
        queue_event(CREATED, ident, L"");
    } else {
        PySys_WriteStderr("Failed to show Windows notification (HRESULT 0x%08lx)\n", (unsigned long)hr);
        queue_event(CREATION_FAILED, ident, L"");
    }
    if (t) { if (t->toast) release_toast(t); else { free(t->ident); free(t); } }
done:
    free(xml);
    PyMem_Free(ident); PyMem_Free(title); PyMem_Free(body); PyMem_Free(url); PyMem_Free(image);
    Py_XDECREF(buttons);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
win32_close_notification(PyObject *self, PyObject *arg) {
    (void)self;
    wchar_t *ident = PyUnicode_AsWideCharString(arg, NULL);
    if (!ident) return NULL;
    LiveToast *t = take_toast(ident);
    HRESULT hr = t && notifier ? IToastNotifier_Hide(notifier, t->toast) : E_FAIL;
    if (!t && notifier) {
        HSTRING klass = NULL, tag = NULL, group = NULL, app = NULL;
        IToastNotificationManagerStatics2 *manager = NULL;
        IToastNotificationHistory *history = NULL;
        wchar_t pid[32]; swprintf(pid, 32, L"%lu", GetCurrentProcessId());
        hr = make_string(L"Windows.UI.Notifications.ToastNotificationManager", &klass);
        if (SUCCEEDED(hr)) hr = RoGetActivationFactory(klass, &IID_IToastNotificationManagerStatics2, (void**)&manager);
        if (SUCCEEDED(hr)) hr = IToastNotificationManagerStatics2_get_History(manager, &history);
        if (SUCCEEDED(hr)) hr = make_string(ident, &tag);
        if (SUCCEEDED(hr)) hr = make_string(pid, &group);
        if (SUCCEEDED(hr)) hr = make_string(AUMID, &app);
        if (SUCCEEDED(hr)) hr = IToastNotificationHistory_RemoveGroupedTagWithId(history, tag, group, app);
        if (history) IToastNotificationHistory_Release(history);
        if (manager) IToastNotificationManagerStatics2_Release(manager);
        WindowsDeleteString(klass); WindowsDeleteString(tag); WindowsDeleteString(group); WindowsDeleteString(app);
    }
    if (t) {
        // Keep the record until the queued close event is delivered.
        t->next = live_toasts; live_toasts = t;
        if (SUCCEEDED(hr)) queue_event(CLOSED, ident, L"");
    }
    PyMem_Free(ident);
    return PyBool_FromLong(SUCCEEDED(hr));
}

static PyObject *
win32_set_notification_activated_callback(PyObject *self, PyObject *arg) {
    (void)self;
    if (arg != Py_None && !PyCallable_Check(arg)) { PyErr_SetString(PyExc_TypeError, "Expected a callable or None"); return NULL; }
    Py_XSETREF(callback, arg == Py_None ? NULL : Py_NewRef(arg));
    Py_RETURN_NONE;
}

static void
cleanup_notifications(void) {
    AcquireSRWLockExclusive(&event_lock);
    shutting_down = true;
    Event *e = events; events = last_event = NULL;
    ReleaseSRWLockExclusive(&event_lock);
    if (class_cookie) CoRevokeClassObject(class_cookie);
    if (activation_window) DestroyWindow(activation_window);
    while (live_toasts) { LiveToast *t = live_toasts; live_toasts = t->next; release_toast(t); }
    while (e) { Event *next = e->next; free(e->ident); free(e->button); free(e); e = next; }
    if (notifier) IToastNotifier_Release(notifier);
    if (ro_initialized) RoUninitialize();
    Py_CLEAR(callback);
}

static PyMethodDef module_methods[] = {
    {"win32_send_notification", _PyCFunction_CAST(win32_send_notification), METH_VARARGS | METH_KEYWORDS, "Send a native Windows toast."},
    {"win32_close_notification", win32_close_notification, METH_O, "Close a live Windows toast."},
    {"win32_set_notification_activated_callback", win32_set_notification_activated_callback, METH_O, "Set the main-thread notification callback."},
    {NULL, NULL, 0, NULL}
};

bool
init_win32_notifications(PyObject *module) {
    register_at_exit_cleanup_func(WIN32_NOTIFICATIONS_CLEANUP_FUNC, cleanup_notifications);
    return PyModule_AddFunctions(module, module_methods) == 0;
}
#endif

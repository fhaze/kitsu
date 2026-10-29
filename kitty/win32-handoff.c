/* License: GPLv3 Copyright: 2026, Kitsu contributors */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ABI from microsoft/terminal src/host/proxy/ITerminalHandoff.idl (MIT).
// The matching OpenConsoleProxy.dll marshals system handles across processes.
typedef struct {
    BSTR title, icon_path;
    LONG icon_index;
    DWORD x, y, width, height, columns, rows, fill_attribute, flags;
    WORD show_window;
} TerminalStartupInfo;

static const CLSID terminal_clsid = {0x75ab2a9b, 0x3128, 0x4777, {0x9d, 0x2b, 0x31, 0x26, 0xec, 0x26, 0x2b, 0x1d}};
static const IID handoff_iid = {0x6f23da90, 0x15c5, 0x4203, {0x9d, 0xb0, 0x64, 0xe7, 0x3f, 0x1b, 0x1b, 0x00}};
static const IID marker_iid = {0x746e6bc0, 0xab05, 0x4e38, {0xab, 0x14, 0x71, 0xe8, 0x67, 0x63, 0x14, 0x1f}};

HRESULT win32_pty_handoff(HANDLE *, HANDLE *, HANDLE, HANDLE, HANDLE, HANDLE, int *, DWORD *);
long win32_receive_handoff(int *, unsigned long *, wchar_t **, unsigned long);

typedef struct Handoff Handoff;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Handoff *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(Handoff *);
    ULONG (STDMETHODCALLTYPE *Release)(Handoff *);
    HRESULT (STDMETHODCALLTYPE *EstablishPtyHandoff)(Handoff *, HANDLE *, HANDLE *, HANDLE, HANDLE, HANDLE, HANDLE, const TerminalStartupInfo *);
} HandoffVtbl;
struct Handoff {
    const HandoffVtbl *lpVtbl;
    LONG refs;
    HANDLE done;
    int master_fd;
    DWORD pid;
    wchar_t *title;
    HRESULT result;
    bool received;
};

static HRESULT STDMETHODCALLTYPE
query_interface(Handoff *self, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &handoff_iid) && !IsEqualIID(iid, &marker_iid)) return E_NOINTERFACE;
    *out = self;
    InterlockedIncrement(&self->refs);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE add_ref(Handoff *self) { return InterlockedIncrement(&self->refs); }
static ULONG STDMETHODCALLTYPE release(Handoff *self) { return InterlockedDecrement(&self->refs); }

static HRESULT STDMETHODCALLTYPE
establish(Handoff *self, HANDLE *input, HANDLE *output, HANDLE signal, HANDLE reference, HANDLE server, HANDLE client, const TerminalStartupInfo *info) {
    if (!input || !output || !info) return E_POINTER;
    *input = *output = NULL;
    if (self->received) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    self->received = true;
    HRESULT hr = win32_pty_handoff(input, output, signal, reference, server, client, &self->master_fd, &self->pid);
    if (SUCCEEDED(hr) && info->title) {
        unsigned n = SysStringLen(info->title);
        self->title = calloc(n + 1, sizeof(wchar_t));
        if (self->title) memcpy(self->title, info->title, n * sizeof(wchar_t));
    }
    self->result = hr;
    SetEvent(self->done);
    return hr;
}

static const HandoffVtbl handoff_vtbl = {query_interface, add_ref, release, establish};
typedef struct {
    IClassFactory iface;
    Handoff *handoff;
} Factory;

static HRESULT STDMETHODCALLTYPE
factory_query(IClassFactory *self, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IClassFactory)) return E_NOINTERFACE;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE factory_ref(IClassFactory *self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE
factory_create(IClassFactory *self, IUnknown *outer, REFIID iid, void **out) {
    if (outer) return CLASS_E_NOAGGREGATION;
    return query_interface(((Factory *)self)->handoff, iid, out);
}
static HRESULT STDMETHODCALLTYPE factory_lock(IClassFactory *self, BOOL lock) { (void)self; (void)lock; return S_OK; }
static IClassFactoryVtbl factory_vtbl = {factory_query, factory_ref, factory_release, factory_create, factory_lock};

long
win32_receive_handoff(int *master_fd, unsigned long *pid, wchar_t **title, unsigned long timeout) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return hr;
    Handoff handoff = {.lpVtbl = &handoff_vtbl, .refs = 1, .master_fd = -1, .result = E_FAIL};
    Factory factory = {.iface.lpVtbl = &factory_vtbl, .handoff = &handoff};
    DWORD cookie = 0;
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_DISABLE_AAA, NULL);
    if (hr == RPC_E_TOO_LATE) hr = S_OK;
    if (FAILED(hr)) goto end;
    handoff.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!handoff.done) { hr = HRESULT_FROM_WIN32(GetLastError()); goto end; }
    // One process/window per console session, including concurrent activations.
    hr = CoRegisterClassObject(&terminal_clsid, (IUnknown *)&factory.iface, CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &cookie);
    if (SUCCEEDED(hr)) {
        DWORD index;
        hr = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, timeout, 1, &handoff.done, &index);
        if (SUCCEEDED(hr)) hr = handoff.result;
    }
    if (cookie) CoRevokeClassObject(cookie);
    if (SUCCEEDED(hr)) {
        *master_fd = handoff.master_fd;
        *pid = handoff.pid;
        *title = handoff.title;
    } else free(handoff.title);
end:
    if (handoff.done) CloseHandle(handoff.done);
    CoUninitialize();
    return hr;
}
#endif

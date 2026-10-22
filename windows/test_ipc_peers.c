/* Native Windows IPC tests, including fault injection for fail-closed paths. */
#define WIN32_LEAN_AND_MEAN
#define pollfd wsa_pollfd
#include <winsock2.h>
#undef pollfd
#undef POLLIN
#undef POLLOUT
#include <windows.h>
#include <iphlpapi.h>

static DWORD WINAPI test_tcp_table(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG);
static BOOL WINAPI test_token_information(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
#define GetExtendedTcpTable test_tcp_table
#define GetTokenInformation test_token_information
#include "../kitty/win32-compat.c"
#undef GetExtendedTcpTable
#undef GetTokenInformation

static unsigned checks, failures, token_queries, table_queries;
static bool fail_table, fail_token, missing_peer, missing_process, changed_pid, foreign_sid;
static BYTE other_sid[SECURITY_MAX_SID_SIZE];

#define CHECK(cond) do { checks++; if (!(cond)) { \
    failures++; fprintf(stderr, "%d: %s (errno=%d, winerror=%lu)\n", __LINE__, #cond, errno, GetLastError()); \
} } while (0)

static DWORD WINAPI
test_tcp_table(PVOID table, PDWORD size, BOOL order, ULONG family, TCP_TABLE_CLASS cls, ULONG reserved) {
    if (fail_table) return ERROR_ACCESS_DENIED;
    DWORD result = GetExtendedTcpTable(table, size, order, family, cls, reserved);
    if (result == NO_ERROR) {
        table_queries++;
        if (missing_peer) *(DWORD *)table = 0;
        if (missing_process || (changed_pid && table_queries == 2)) {
            if (family == AF_INET) {
                MIB_TCPTABLE_OWNER_PID *t = table;
                for (DWORD i = 0; i < t->dwNumEntries; i++) t->table[i].dwOwningPid = MAXDWORD;
            } else {
                MIB_TCP6TABLE_OWNER_PID *t = table;
                for (DWORD i = 0; i < t->dwNumEntries; i++) t->table[i].dwOwningPid = MAXDWORD;
            }
        }
    }
    return result;
}

static BOOL WINAPI
test_token_information(HANDLE token, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD size, PDWORD needed) {
    if (fail_token) { SetLastError(ERROR_ACCESS_DENIED); return false; }
    BOOL result = GetTokenInformation(token, cls, info, size, needed);
    if (result && cls == TokenUser && info && ++token_queries == 2 && foreign_sid) {
        ((TOKEN_USER *)info)->User.Sid = other_sid;
    }
    return result;
}

static SOCKET
listener(int family, bool dual_stack, unsigned short *port) {
    SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    if (family == AF_INET6) {
        DWORD only = !dual_stack;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&only, sizeof(only));
        struct sockaddr_in6 addr = {.sin6_family = AF_INET6};
        if (!dual_stack) addr.sin6_addr = in6addr_loopback;
        if (bind(s, (struct sockaddr *)&addr, sizeof(addr))) goto fail;
        int size = sizeof(addr);
        if (getsockname(s, (struct sockaddr *)&addr, &size)) goto fail;
        *port = ntohs(addr.sin6_port);
    } else {
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (bind(s, (struct sockaddr *)&addr, sizeof(addr))) goto fail;
        int size = sizeof(addr);
        if (getsockname(s, (struct sockaddr *)&addr, &size)) goto fail;
        *port = ntohs(addr.sin_port);
    }
    if (listen(s, 4)) goto fail;
    return s;
fail:
    closesocket(s);
    return INVALID_SOCKET;
}

static SOCKET
connect_client(int family, unsigned short port) {
    SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    int result;
    if (family == AF_INET6) {
        struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_addr = in6addr_loopback, .sin6_port = htons(port)};
        result = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    } else {
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(port)};
        result = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    }
    if (result) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static void
test_peer(int family, bool dual_stack) {
    unsigned short port;
    SOCKET server = listener(family, dual_stack, &port);
    CHECK(server != INVALID_SOCKET);
    if (server == INVALID_SOCKET) return;
    int fd = kitty_win32_fd_from_socket_handle(server);
    CHECK(kitty_win32_peer_credentials_are_available(fd));
    CHECK(!kitty_win32_peer_is_same_user(fd));  // not connected
    SOCKET client = connect_client(dual_stack ? AF_INET : family, port);
    CHECK(client != INVALID_SOCKET);
    if (client != INVALID_SOCKET) {
        SOCKET accepted = accept(server, NULL, NULL);
        CHECK(accepted != INVALID_SOCKET);
        if (accepted != INVALID_SOCKET) {
            int peer = kitty_win32_fd_from_socket_handle(accepted);
            CHECK(kitty_win32_peer_is_same_user(peer));
            CHECK(shutdown(client, SD_SEND) == 0);
            CHECK(kitty_win32_peer_is_same_user(peer));  // send-and-half-close clients
            fail_table = true;
            CHECK(!kitty_win32_peer_is_same_user(peer));
            fail_table = false;
            fail_token = true;
            CHECK(!kitty_win32_peer_is_same_user(peer));
            fail_token = false;
            missing_peer = true;
            CHECK(!kitty_win32_peer_is_same_user(peer));
            missing_peer = false;
            missing_process = true;
            CHECK(!kitty_win32_peer_is_same_user(peer));
            missing_process = false;
            foreign_sid = true;
            token_queries = 0;
            CHECK(!kitty_win32_peer_is_same_user(peer));
            foreign_sid = false;
            changed_pid = true;
            table_queries = 0;
            CHECK(!kitty_win32_peer_is_same_user(peer));
            changed_pid = false;
            CHECK(kitty_win32_peer_is_same_user(peer));
            kitty_win32_close(peer);
        }
        closesocket(client);
    }
    kitty_win32_close(fd);
}

static void
test_process(bool foreign) {
    wchar_t exe[32768];
    CHECK(GetModuleFileNameW(NULL, exe, sizeof(exe) / sizeof(*exe)) != 0);
    unsigned short port;
    SOCKET server = listener(AF_INET, false, &port);
    CHECK(server != INVALID_SOCKET);
    wchar_t command[32768 + 64];
    swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" --client %u", exe, port);
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    BOOL started = foreign || CreateProcessW(exe, command, NULL, NULL, false, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CHECK(started);
    if (foreign) {
        // Launch --client PORT as another user; no account/privilege changes in this test.
        printf("PORT=%u\n", port);
        fflush(stdout);
    }
    if (started) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(server, &reads);
        struct timeval timeout = {.tv_sec = foreign ? 90 : 15};
        int ready = select(0, &reads, NULL, NULL, &timeout);
        CHECK(ready == 1);
        if (ready == 1) {
            SOCKET accepted = accept(server, NULL, NULL);
            CHECK(accepted != INVALID_SOCKET);
            int peer = kitty_win32_fd_from_socket_handle(accepted);
            CHECK(kitty_win32_peer_is_same_user(peer) == !foreign);
            if (foreign) CHECK(errno == EACCES);
            kitty_win32_close(peer);
        }
    }
    if (pi.hProcess) {
        CHECK(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0);
        DWORD exit_code = 1;
        CHECK(GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0);
        if (exit_code) fprintf(stderr, "Client exit code: %lu\n", exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    closesocket(server);
}

int
main(int argc, char **argv) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    if (argc == 3 && !strcmp(argv[1], "--client")) {
        SOCKET client = connect_client(AF_INET, (unsigned short)atoi(argv[2]));
        if (client == INVALID_SOCKET) return WSAGetLastError();
        char byte;
        recv(client, &byte, 1, 0);
        closesocket(client);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--expect-foreign")) {
        test_process(true);
        goto done;
    }
    DWORD size = sizeof(other_sid);
    CHECK(CreateWellKnownSid(WinLocalServiceSid, NULL, other_sid, &size));
    CHECK(kitty_win32_peer_credentials_are_available(-1));
    CHECK(!kitty_win32_peer_is_same_user(-1));
    test_peer(AF_INET, false);
    test_peer(AF_INET6, false);
    test_peer(AF_INET6, true);
    test_process(false);
done:
    WSACleanup();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

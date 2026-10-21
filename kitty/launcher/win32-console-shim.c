/*
 * win32-console-shim.c
 * Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

// Built as kitty.com, a console subsystem executable that runs kitty.exe from
// its own directory with the same command line and waits for it to exit.
// The DLL-free PATH launchers instead run ..\bin\kitsu.exe.
//
// kitty.exe is a GUI subsystem executable so that starting it from Explorer,
// the start menu or Win+R does not flash a console window. The flip side is
// that cmd.exe and PowerShell do not wait for GUI executables, they return to
// the prompt right away. That breaks everything kitty runs in the terminal on
// behalf of the shell: kitty @ ..., kitty +kitten ssh/icat/..., --version and
// so on all end up fighting the shell for the console. Shells look up commands
// via PATHEXT, in which .COM precedes .EXE, so a plain "kitty" resolves to
// this shim from cmd.exe and PowerShell (and to kitty.exe in bash, which waits
// for its children anyway).

#include <windows.h>

static BOOL WINAPI
ignore_console_ctrl_event(DWORD type) {
    (void)type;
    return TRUE;
}

// Points *rest at whatever follows the first (program name) token of cmdline
static const wchar_t *
skip_program_name(const wchar_t *cmdline) {
    const wchar_t *p = cmdline;
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p == L'"') p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    return p;
}

static void
report_error(const wchar_t *msg) {
    DWORD err = GetLastError();
    wchar_t *desc = NULL;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&desc,
        0,
        NULL);
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written;
    WriteConsoleW(h, msg, (DWORD)wcslen(msg), &written, NULL);
    if (desc) {
        WriteConsoleW(h, desc, (DWORD)wcslen(desc), &written, NULL);
        LocalFree(desc);
    } else {
        WriteConsoleW(h, L"\n", 1, &written, NULL);
    }
}

int
wmain(void) {
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(NULL, exe, sizeof(exe) / sizeof(exe[0]));
    if (n == 0 || n >= sizeof(exe) / sizeof(exe[0])) {
        report_error(L"Failed to get path to kitty.com: ");
        return 1;
    }
    wchar_t *sep = wcsrchr(exe, L'\\');
#ifdef KITTY_PATH_LAUNCHER
    const wchar_t target[] = L"..\\bin\\kitsu.exe";
    if (!sep || (size_t)(sep + 1 - exe) + sizeof(target) / sizeof(target[0]) > sizeof(exe) / sizeof(exe[0])) {
        report_error(L"Unexpected path for Kitsu PATH launcher: ");
        return 1;
    }
    wcscpy(sep + 1, target);
#else
    wchar_t *dot = wcsrchr(exe, L'.');
    if (!dot || (sep && sep > dot) || n + 1 >= sizeof(exe) / sizeof(exe[0])) {
        report_error(L"Unexpected path for kitty.com: ");
        return 1;
    }
    wcscpy(dot, L".exe");
#endif

    const wchar_t *rest = skip_program_name(GetCommandLineW());
    size_t exe_len = wcslen(exe), rest_len = wcslen(rest);
    wchar_t *cmdline = HeapAlloc(GetProcessHeap(), 0, (exe_len + rest_len + 3) * sizeof(wchar_t));
    if (!cmdline) {
        report_error(L"Out of memory: ");
        return 1;
    }
    cmdline[0] = L'"';
    wcscpy(cmdline + 1, exe);
    cmdline[exe_len + 1] = L'"';
    wcscpy(cmdline + exe_len + 2, rest);

    // Pass our std handles on explicitly, GUI subsystem children do not
    // inherit them otherwise, so redirections like kitty @ ls > out would be
    // lost. kitty.exe keeps these and binds only the missing ones to the
    // console it attaches to.
    STARTUPINFOW si = {
        .cb = sizeof(si),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
        .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE),
        .hStdError = GetStdHandle(STD_ERROR_HANDLE),
    };
    PROCESS_INFORMATION pi = {0};
    // Ctrl+C is delivered to every process attached to the console, let
    // kitty.exe deal with it and keep waiting for its exit code.
    SetConsoleCtrlHandler(ignore_console_ctrl_event, TRUE);
    if (!CreateProcessW(exe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        report_error(L"Failed to run kitty.exe: ");
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

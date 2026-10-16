/*
 * win32-pty.h
 * Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "data-types.h"

// ConPTY based pseudo terminals. The "master" side is a loopback socket so
// that it can be used with poll()/read()/write() by the child monitor exactly
// like a PTY master fd on POSIX. Bridge threads shuttle data between the
// socket and the pipes attached to the pseudo console.

// Creates a pseudo console. master_fd is the socket, slave_fd is a placeholder
// fd that only exists so that callers can treat the pair like openpty()
bool win32_pty_open(int *master_fd, int *slave_fd, unsigned short rows, unsigned short cols);

// Spawns a process attached to the pseudo console associated with master_fd.
// argv and env are NULL terminated arrays of UTF-8 strings. If ready_read_fd
// is >= 0 the process is kept suspended until the fd hits EOF. If
// stdin_read_fd is >= 0 data read from it is fed into the pseudo console input.
// Returns the process id or -1 on failure (with errno set).
pid_t win32_pty_spawn(int master_fd, const char *exe, const char *cwd, char *const argv[], char *const env[], int ready_read_fd, int stdin_read_fd);

bool win32_pty_resize(int master_fd, unsigned short rows, unsigned short cols);

// Sends a console control signal (SIGINT -> Ctrl-C, etc.) to the process
// attached to the pseudo console associated with pid
bool win32_pty_signal_pid(pid_t pid, int sig);

void win32_pty_init(void);
const char *win32_pty_provider(void);

// License: GPLv3 Copyright: 2025, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package config

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

// SignalKittyToReloadConfig is the Windows equivalent of sending SIGUSR1.
func SignalKittyToReloadConfig(pid int) error {
	if pid <= 0 {
		return fmt.Errorf("invalid kitty PID: %d", pid)
	}
	name := windows.StringToUTF16Ptr(fmt.Sprintf(`Local\kitsu-reload-config-%d`, pid))
	event, err := windows.OpenEvent(windows.EVENT_MODIFY_STATE, false, name)
	if err != nil {
		return fmt.Errorf("open config reload event for PID %d: %w", pid, err)
	}
	defer windows.CloseHandle(event)
	return windows.SetEvent(event)
}

func ReloadConfigInKitty(in_parent_only bool) error {
	if in_parent_only {
		value := os.Getenv("KITTY_PID")
		if value == "" {
			return nil
		}
		pid, err := strconv.ParseUint(value, 10, 32)
		if err != nil {
			return err
		}
		return SignalKittyToReloadConfig(int(pid))
	}
	processes, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return err
	}
	defer windows.CloseHandle(processes)
	entry := windows.ProcessEntry32{Size: uint32(unsafe.Sizeof(windows.ProcessEntry32{}))}
	var reload_errors []error
	for err = windows.Process32First(processes, &entry); err == nil; err = windows.Process32Next(processes, &entry) {
		name := windows.UTF16ToString(entry.ExeFile[:])
		if strings.EqualFold(name, "kitty.exe") || strings.EqualFold(name, "kitsu.exe") {
			// Only GUI instances create the event. Helpers and exiting processes
			// have no event, so no command-line inspection is needed.
			if err := SignalKittyToReloadConfig(int(entry.ProcessID)); err != nil && !errors.Is(err, windows.ERROR_FILE_NOT_FOUND) {
				reload_errors = append(reload_errors, err)
			}
		}
	}
	if !errors.Is(err, windows.ERROR_NO_MORE_FILES) {
		reload_errors = append(reload_errors, err)
	}
	return errors.Join(reload_errors...)
}

// License: GPLv3 Copyright: 2026, Kitsu contributors

//go:build windows

package config

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"golang.org/x/sys/windows"
)

func TestReloadConfigInParent(t *testing.T) {
	name := windows.StringToUTF16Ptr(fmt.Sprintf(`Local\kitsu-reload-config-%d`, os.Getpid()))
	event, err := windows.CreateEvent(nil, 0, 0, name)
	if err != nil {
		t.Fatal(err)
	}
	defer windows.CloseHandle(event)
	t.Setenv("KITTY_PID", strconv.Itoa(os.Getpid()))
	for range 3 {
		if err := ReloadConfigInKitty(true); err != nil {
			t.Fatal(err)
		}
		if status, err := windows.WaitForSingleObject(event, 1000); err != nil || status != windows.WAIT_OBJECT_0 {
			t.Fatalf("reload event not signaled: status=%d err=%v", status, err)
		}
		if status, err := windows.WaitForSingleObject(event, 0); err != nil || status != uint32(windows.WAIT_TIMEOUT) {
			t.Fatalf("reload event did not auto-reset: status=%d err=%v", status, err)
		}
	}
}

func TestReloadConfigParentErrors(t *testing.T) {
	t.Setenv("KITTY_PID", "")
	if err := ReloadConfigInKitty(true); err != nil {
		t.Fatal(err)
	}
	for _, pid := range []string{"0", "-1", "not-a-pid", "4294967296"} {
		t.Setenv("KITTY_PID", pid)
		if err := ReloadConfigInKitty(true); err == nil {
			t.Fatalf("invalid PID %q was accepted", pid)
		}
	}
	t.Setenv("KITTY_PID", strconv.Itoa(os.Getpid()))
	if err := ReloadConfigInKitty(true); !errors.Is(err, windows.ERROR_FILE_NOT_FOUND) {
		t.Fatalf("missing event was not reported: %v", err)
	}
}

func TestReloadConfigProcessHelper(t *testing.T) {
	mode := os.Getenv("KITTY_RELOAD_TEST_HELPER")
	if mode == "" {
		return
	}
	if mode == "no-event" {
		fmt.Println("ready")
		time.Sleep(30 * time.Second)
		return
	}
	name := windows.StringToUTF16Ptr(fmt.Sprintf(`Local\kitsu-reload-config-%d`, os.Getpid()))
	event, err := windows.CreateEvent(nil, 0, 0, name)
	if err != nil {
		t.Fatal(err)
	}
	defer windows.CloseHandle(event)
	fmt.Println("ready")
	if status, err := windows.WaitForSingleObject(event, 15000); err != nil || status != windows.WAIT_OBJECT_0 {
		t.Fatalf("reload not received: status=%d err=%v", status, err)
	}
}

func TestReloadConfigInAll(t *testing.T) {
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(exe)
	if err != nil {
		t.Fatal(err)
	}
	start := func(name, mode string) *exec.Cmd {
		t.Helper()
		path := filepath.Join(t.TempDir(), name)
		if err := os.WriteFile(path, data, 0o700); err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
		t.Cleanup(cancel)
		cmd := exec.CommandContext(ctx, path, "-test.run=^TestReloadConfigProcessHelper$")
		cmd.Env = append(os.Environ(), "KITTY_RELOAD_TEST_HELPER="+mode)
		cmd.Stderr = os.Stderr
		output, err := cmd.StdoutPipe()
		if err != nil {
			t.Fatal(err)
		}
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = cmd.Process.Kill() })
		if line, err := bufio.NewReader(output).ReadString('\n'); err != nil || line != "ready\n" {
			t.Fatalf("helper not ready: %q %v", line, err)
		}
		return cmd
	}
	kitsu, kitty := start("kitsu.exe", "event"), start("KiTtY.exe", "event")
	helper := start("kitsu.exe", "no-event")
	defer helper.Wait()
	defer helper.Process.Kill()
	// This test executable is not a GUI name and must not be signaled.
	name := windows.StringToUTF16Ptr(fmt.Sprintf(`Local\kitsu-reload-config-%d`, os.Getpid()))
	event, err := windows.CreateEvent(nil, 0, 0, name)
	if err != nil {
		t.Fatal(err)
	}
	defer windows.CloseHandle(event)
	if err := ReloadConfigInKitty(false); err != nil {
		t.Fatal(err)
	}
	for _, cmd := range []*exec.Cmd{kitsu, kitty} {
		if err := cmd.Wait(); err != nil {
			t.Fatal(err)
		}
	}
	if status, err := windows.WaitForSingleObject(event, 0); err != nil || status != uint32(windows.WAIT_TIMEOUT) {
		t.Fatalf("unrelated process was signaled: status=%d err=%v", status, err)
	}
}

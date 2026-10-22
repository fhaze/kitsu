// License: GPLv3 Copyright: 2026, Kitsu contributors

//go:build windows

package watch

import (
	"fmt"
	"os"
	"testing"

	"golang.org/x/sys/windows"
)

func TestSignalKittyToReloadConfig(t *testing.T) {
	name := windows.StringToUTF16Ptr(fmt.Sprintf(`Local\kitsu-reload-config-%d`, os.Getpid()))
	event, err := windows.CreateEvent(nil, 0, 0, name)
	if err != nil {
		t.Fatal(err)
	}
	defer windows.CloseHandle(event)
	if err := signal_kitty_to_reload_config(os.Getpid()); err != nil {
		t.Fatal(err)
	}
	if status, err := windows.WaitForSingleObject(event, 1000); err != nil || status != windows.WAIT_OBJECT_0 {
		t.Fatalf("watcher did not signal reload: status=%d err=%v", status, err)
	}
}

// License: GPLv3 Copyright: 2026, Kitsu contributors

//go:build windows

package choose_fonts

import (
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/kovidgoyal/kitty/tools/tui/loop"
	"golang.org/x/sys/windows"
)

func TestChooseFontsReportsReloadFailure(t *testing.T) {
	t.Setenv("KITTY_PID", strconv.Itoa(os.Getpid()))
	pane := final_pane{handler: &handler{opts: &Options{
		Config_file_name: filepath.Join(t.TempDir(), "kitty.conf"), Reload_in: "parent",
	}}}
	event := loop.KeyEvent{Type: loop.PRESS, Key: "ENTER"}
	if err := pane.on_key_event(&event); !errors.Is(err, windows.ERROR_FILE_NOT_FOUND) {
		t.Fatalf("reload failure was not reported: %v", err)
	}
}

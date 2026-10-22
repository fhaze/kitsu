// License: GPLv3 Copyright: 2026, Kitsu contributors

//go:build windows

package themes

import (
	"errors"
	"os"
	"strconv"
	"testing"

	"golang.org/x/sys/windows"
)

func TestThemeReportsReloadFailure(t *testing.T) {
	t.Setenv("KITTY_PID", strconv.Itoa(os.Getpid()))
	theme := Theme{metadata: &ThemeMetadata{Name: "test"}, code: "background #123456\n"}
	if err := theme.SaveInConf(t.TempDir(), "parent", "kitty.conf"); !errors.Is(err, windows.ERROR_FILE_NOT_FOUND) {
		t.Fatalf("reload failure was not reported: %v", err)
	}
}

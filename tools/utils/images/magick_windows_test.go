// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package images

import (
	"testing"
)

func TestMagickBatchFilesAreNotUsed(t *testing.T) {
	for found, expected := range map[string]string{
		`C:\x\magick.exe`: `C:\x\magick.exe`,
		`C:\x\magick.cmd`: "magick",
		`C:\x\magick.BAT`: "magick",
		"magick":          "magick",
	} {
		if actual := batch_safe_magick_exe(func() string { return found })(); actual != expected {
			t.Errorf("magick found at %#v is used as %#v, expected %#v", found, actual, expected)
		}
	}
}

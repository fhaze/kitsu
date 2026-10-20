// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package images

import (
	"sync"

	"github.com/kovidgoyal/imaging/magick"
	"github.com/kovidgoyal/kitty/tools/utils"
)

// The imaging module runs magick with exec.Command and the image path as an
// argument. Should magick resolve to a batch file, cmd.exe would interpret
// metacharacters in file names (CVE-2024-24576, aka BatBadBut), so treat
// ImageMagick as not installed then. Its installer provides magick.exe.
func batch_safe_magick_exe(find func() string) func() string {
	return sync.OnceValue(func() string {
		if exe := find(); !utils.IsBatchFile(exe) {
			return exe
		}
		return "magick"
	})
}

func init() {
	// magick.HasMagick() and every run of magick go through magick.MagickExe()
	magick.MagickExe = batch_safe_magick_exe(magick.MagickExe)
}

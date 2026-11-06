// License: GPLv3 Copyright: 2026, Kitsu contributors

//go:build !windows

package transfer

import (
	"io/fs"
	"os"

	"golang.org/x/sys/unix"
)

func open_in_place_without_links(path string) (*os.File, error) {
	var st unix.Stat_t
	if unix.Lstat(path, &st) == nil && st.Mode&unix.S_IFMT == unix.S_IFREG && st.Nlink == 1 {
		return os.OpenFile(path, os.O_WRONLY|os.O_TRUNC|unix.O_NOFOLLOW, 0)
	}
	return nil, fs.ErrPermission
}

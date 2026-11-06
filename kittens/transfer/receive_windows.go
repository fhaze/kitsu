// License: GPLv3 Copyright: 2026, Kitsu contributors

package transfer

import (
	"io/fs"
	"os"
)

func open_in_place_without_links(path string) (*os.File, error) {
	// Without POSIX O_NOFOLLOW, require the atomic temporary-file path so
	// transfers cannot truncate a symlink or hardlink's target.
	return nil, fs.ErrPermission
}

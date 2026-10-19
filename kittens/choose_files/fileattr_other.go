//go:build !windows

package choose_files

import (
	"io/fs"
)

func has_hidden_attribute(e fs.DirEntry) bool { return false }

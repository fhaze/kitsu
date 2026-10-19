//go:build windows

package choose_files

import (
	"io/fs"
	"syscall"
)

// Windows hides files with an attribute rather than a leading dot. On Windows,
// Info() of an entry from os.ReadDir() needs no further system call.
func has_hidden_attribute(e fs.DirEntry) bool {
	if info, err := e.Info(); err == nil && info != nil {
		if a, ok := info.Sys().(*syscall.Win32FileAttributeData); ok {
			return a.FileAttributes&syscall.FILE_ATTRIBUTE_HIDDEN != 0
		}
	}
	return false
}

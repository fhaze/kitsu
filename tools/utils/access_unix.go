//go:build !windows

package utils

import (
	"io/fs"

	"golang.org/x/sys/unix"
)

const (
	R_OK AccessMode = unix.R_OK
	W_OK AccessMode = unix.W_OK
	X_OK AccessMode = unix.X_OK
)

// Access reports whether the calling process can access path in the
// specified modes, as with access(2).
func Access(path string, mode AccessMode) error {
	return unix.Access(path, uint32(mode))
}

// PosixPermissions returns the permission bits to send to a POSIX system for
// the file at path, which s describes.
func PosixPermissions(path string, s fs.FileInfo) fs.FileMode {
	return s.Mode().Perm()
}

// License: GPLv3 Copyright: 2026, Kitsu contributors

//go:build !windows

package utils

import (
	"os"

	"golang.org/x/sys/unix"
)

func map_mime_cache(f *os.File, size int) ([]byte, error) {
	return unix.Mmap(int(f.Fd()), 0, size, unix.PROT_READ, unix.MAP_PRIVATE)
}

func unmap_mime_cache(data []byte) error { return unix.Munmap(data) }

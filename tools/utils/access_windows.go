//go:build windows

package utils

import (
	"bytes"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
)

const (
	R_OK AccessMode = 4
	W_OK AccessMode = 2
	X_OK AccessMode = 1
)

var executable_extensions = sync.OnceValue(func() map[string]bool {
	pathext := os.Getenv("PATHEXT")
	if pathext == "" {
		pathext = ".COM;.EXE;.BAT;.CMD"
	}
	ans := make(map[string]bool)
	for ext := range strings.SplitSeq(pathext, ";") {
		if ext = strings.ToLower(strings.TrimSpace(ext)); ext != "" {
			ans[ext] = true
		}
	}
	return ans
})

// Access reports whether path can be accessed in the specified modes.
// Windows has no execute permission bit, so executability is determined by
// the file extension being listed in PATHEXT, as cmd.exe does.
func Access(path string, mode AccessMode) error {
	s, err := os.Stat(path)
	if err != nil {
		return err
	}
	if mode&W_OK != 0 && s.Mode().Perm()&0o200 == 0 {
		return &os.PathError{Op: "access", Path: path, Err: syscall.EACCES}
	}
	if mode&X_OK != 0 && !s.IsDir() && !executable_extensions()[strings.ToLower(filepath.Ext(path))] {
		return &os.PathError{Op: "access", Path: path, Err: syscall.EACCES}
	}
	return nil
}

func is_posix_executable(path string) bool {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".exe", ".com", ".bat", ".cmd": // what Python's os.stat() marks as executable
		return true
	}
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	head := make([]byte, 4)
	n, _ := io.ReadFull(f, head)
	head = head[:n]
	return bytes.HasPrefix(head, []byte("#!")) || bytes.Equal(head, []byte("\x7fELF"))
}

// PosixPermissions returns the permission bits to send to a POSIX system for
// the file at path, which s describes. Windows has no mode bits, Go reports
// 0o666 for every writable file and 0o777 for every directory, which would
// make them world writable on the other end. So use 0o755 for directories,
// scripts and executables and 0o644 for everything else, without the write
// bits for read-only files. Windows ignores the read-only attribute of
// directories, Explorer uses it to mark customized folders, so it is ignored
// here as well.
func PosixPermissions(path string, s fs.FileInfo) fs.FileMode {
	switch {
	case s.Mode()&fs.ModeSymlink != 0:
		return 0o777
	case s.IsDir():
		return 0o755
	}
	perm := fs.FileMode(0o644)
	if is_posix_executable(path) {
		perm = 0o755
	}
	if d, ok := s.Sys().(*syscall.Win32FileAttributeData); ok && d.FileAttributes&syscall.FILE_ATTRIBUTE_READONLY != 0 {
		perm &^= 0o222
	}
	return perm
}

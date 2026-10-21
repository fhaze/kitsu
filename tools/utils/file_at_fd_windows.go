//go:build windows

package utils

import (
	"errors"
	"fmt"
	"io/fs"
	"math/bits"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

var _ = fmt.Print

// Windows has no *at() family of system calls, so the directory relative
// operations are implemented in terms of the directory's path.

func joined(dir *os.File, name string) string {
	return filepath.Join(dir.Name(), name)
}

func OpenDir(path string) (*os.File, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	if s, err := f.Stat(); err != nil || !s.IsDir() {
		f.Close()
		if err == nil {
			err = &os.PathError{Op: "open", Path: path, Err: syscall.ENOTDIR}
		}
		return nil, err
	}
	return f, nil
}

// Device and inode numbers that uniquely identify a file. The path must refer
// to i; symlinks are followed only when i describes their target (os.Stat).
func FileIdentity(i os.FileInfo, path string) (dev, inode uint64, err error) {
	flags := uint32(windows.FILE_FLAG_BACKUP_SEMANTICS)
	if i.Mode()&(os.ModeSymlink|os.ModeIrregular) != 0 {
		flags |= windows.FILE_FLAG_OPEN_REPARSE_POINT
	}
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return 0, 0, &os.PathError{Op: "FileIdentity", Path: path, Err: err}
	}
	h, err := windows.CreateFile(p, windows.FILE_READ_ATTRIBUTES, windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE|windows.FILE_SHARE_DELETE, nil, windows.OPEN_EXISTING, flags, 0)
	if err != nil {
		return 0, 0, &os.PathError{Op: "FileIdentity", Path: path, Err: err}
	}
	defer windows.CloseHandle(h)
	// FileIdInfo supplies the full volume serial used by Python's os.stat.
	var id struct {
		volume, low, high uint64
	}
	err = windows.GetFileInformationByHandleEx(h, windows.FileIdInfo, (*byte)(unsafe.Pointer(&id)), uint32(unsafe.Sizeof(id)))
	if err == nil {
		// ReFS (including Dev Drives) uses 128-bit file IDs. Fold the high half in rather than
		// failing, callers only need a stable identity to detect hard links and copies.
		return id.volume, id.low ^ bits.RotateLeft64(id.high*0x9e3779b97f4a7c15, 32), nil
	}
	if !errors.Is(err, windows.ERROR_INVALID_PARAMETER) && !errors.Is(err, windows.ERROR_NOT_SUPPORTED) && !errors.Is(err, windows.ERROR_INVALID_FUNCTION) {
		return 0, 0, &os.PathError{Op: "FileIdentity", Path: path, Err: err}
	}
	var info windows.ByHandleFileInformation
	if err := windows.GetFileInformationByHandle(h, &info); err != nil {
		return 0, 0, &os.PathError{Op: "FileIdentity", Path: path, Err: err}
	}
	return uint64(info.VolumeSerialNumber), uint64(info.FileIndexHigh)<<32 | uint64(info.FileIndexLow), nil
}

// Windows has no POSIX ownership or mode bits, files in per-user locations
// such as the temp dir are protected by ACLs.
func CheckFilePrivateToCurrentUser(i os.FileInfo) error { return nil }

func DeviceNumber(i os.FileInfo) uint64 { return 0 }

func MkdirAt(parentDir *os.File, name string, perm os.FileMode) error {
	return os.Mkdir(joined(parentDir, name), perm)
}

func OpenAt(dirFile *os.File, name string) (*os.File, error) {
	return os.Open(joined(dirFile, name))
}

func OpenDirAt(dirFile *os.File, name string) (*os.File, error) {
	return OpenDir(joined(dirFile, name))
}

func SymlinkAt(dirFile *os.File, name, target string) (err error) {
	return os.Symlink(target, joined(dirFile, name))
}

func CreateAt(dirFile *os.File, name string, permissions os.FileMode) (*os.File, error) {
	return os.OpenFile(joined(dirFile, name), os.O_RDWR|os.O_CREATE|os.O_TRUNC, permissions)
}

func CreateExclusiveAt(dirFile *os.File, name string, permissions os.FileMode) (*os.File, error) {
	return os.OpenFile(joined(dirFile, name), os.O_RDWR|os.O_CREATE|os.O_EXCL, permissions)
}

func CreateDirAt(parent *os.File, name string, permissions os.FileMode) (*os.File, error) {
	if err := MkdirAt(parent, name, permissions); err != nil && !errors.Is(err, fs.ErrExist) {
		return nil, err
	}
	return OpenDirAt(parent, name)
}

func StatAt(dirFile *os.File, name string) (os.FileInfo, error) {
	return os.Stat(joined(dirFile, name))
}

func LstatAt(dirFile *os.File, name string) (os.FileInfo, error) {
	return os.Lstat(joined(dirFile, name))
}

func UnlinkAt(parent *os.File, name string) error {
	return os.Remove(joined(parent, name))
}

func RemoveDirAt(parent *os.File, name string) error {
	return os.Remove(joined(parent, name))
}

func LinkAt(oldparent *os.File, oldname string, newparent *os.File, newname string, follow_symlinks bool) (err error) {
	oldpath := joined(oldparent, oldname)
	if follow_symlinks {
		if oldpath, err = filepath.EvalSymlinks(oldpath); err != nil {
			return err
		}
	}
	return os.Link(oldpath, joined(newparent, newname))
}

func DupFile(f *os.File) (ans *os.File, err error) {
	p := windows.CurrentProcess()
	var h windows.Handle
	if err = windows.DuplicateHandle(p, windows.Handle(f.Fd()), p, &h, 0, false, windows.DUPLICATE_SAME_ACCESS); err != nil {
		return nil, &os.PathError{Op: "dup", Path: f.Name(), Err: err}
	}
	return os.NewFile(uintptr(h), f.Name()), nil
}

func ReadLinkAt(parent *os.File, name string) (string, error) {
	return os.Readlink(joined(parent, name))
}

func RenameAt(old_parent *os.File, old_name string, new_parent *os.File, new_name string) error {
	return os.Rename(joined(old_parent, old_name), joined(new_parent, new_name))
}

func MknodAt(parent *os.File, name string, mode os.FileMode, dev uint64) error {
	return &os.PathError{Op: "mknodat", Path: joined(parent, name), Err: errors.ErrUnsupported}
}

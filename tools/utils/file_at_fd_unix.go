//go:build !windows

package utils

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

var _ = fmt.Print

// Open a directory by path for use with the *At functions.
func OpenDir(path string) (*os.File, error) {
	fd, err := unix.Open(path, unix.O_DIRECTORY|unix.O_RDONLY|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	return os.NewFile(uintptr(fd), path), nil
}

// Device and inode numbers that uniquely identify a file
func FileIdentity(i os.FileInfo, path string) (dev, inode uint64, err error) {
	switch s := i.Sys().(type) {
	case *syscall.Stat_t:
		return uint64(s.Dev), uint64(s.Ino), nil
	case *unix.Stat_t:
		return uint64(s.Dev), uint64(s.Ino), nil
	}
	panic("unknown stat result type from os.FileInfo")
}

// Check that the file is owned by the current user and not accessible to others
func CheckFilePrivateToCurrentUser(i os.FileInfo) error {
	var uid, gid uint32
	switch s := i.Sys().(type) {
	case *syscall.Stat_t:
		uid, gid = s.Uid, s.Gid
	case *unix.Stat_t:
		uid, gid = s.Uid, s.Gid
	default:
		return fmt.Errorf("Could not determine owner of %s", i.Name())
	}
	if os.Getuid() != int(uid) || os.Getgid() != int(gid) {
		return fmt.Errorf("Incorrect owner on %s", i.Name())
	}
	if i.Mode().Perm() != 0o600 {
		return fmt.Errorf("Incorrect permissions on %s", i.Name())
	}
	return nil
}

// The device number for device files
func DeviceNumber(i os.FileInfo) uint64 {
	switch s := i.Sys().(type) {
	case *syscall.Stat_t:
		return uint64(s.Rdev)
	case *unix.Stat_t:
		return uint64(s.Rdev)
	}
	return 0
}

// MkdirAt creates a new subdirectory named 'name' inside the directory
// pointed to by parentDir.
func MkdirAt(parentDir *os.File, name string, perm os.FileMode) (err error) {
	// parentDir.Fd() gives us the base directory handle
	fd := int(parentDir.Fd())

	// unix.Mkdirat(dirfd, path, mode)
	// We convert the os.FileMode to a uint32 for the syscall
	for {
		if err = unix.Mkdirat(fd, name, uint32(perm)); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		return &fs.PathError{
			Op:   "mkdirat",
			Path: filepath.Join(parentDir.Name(), name),
			Err:  err,
		}
	}
	return nil
}

// OpenAt opens a file relative to the directory pointed to by dirFile.
// Matches the behavior of os.Open (read-only).
func OpenAt(dirFile *os.File, name string) (*os.File, error) {
	return openAt(dirFile, name, unix.O_RDONLY, 0)
}

// Opens a directory relative to the directory pointed to by dirFile.
// Matches the behavior of os.Open (read-only).
func OpenDirAt(dirFile *os.File, name string) (*os.File, error) {
	return openAt(dirFile, name, unix.O_RDONLY|unix.O_DIRECTORY, 0)
}

// Create a symlink named name in the directory pointed to by dirFile. The
// target of the symlink is set to target
func SymlinkAt(dirFile *os.File, name, target string) (err error) {
	for {
		if err = unix.Symlinkat(target, int(dirFile.Fd()), name); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		return &fs.PathError{
			Op:   "symlinkat",
			Path: filepath.Join(dirFile.Name(), name),
			Err:  err,
		}

	}
	return
}

// CreateAt creates or truncates a file relative to the directory pointed to by dirFile.
// Matches the behavior of os.Create (read-write, creates if doesn't exist, truncates).
func CreateAt(dirFile *os.File, name string, permissions os.FileMode) (*os.File, error) {
	return openAt(dirFile, name, unix.O_RDWR|unix.O_CREAT|unix.O_TRUNC, permissions)
}

// CreateExclusiveAt creates a file relative to the directory pointed to by
// dirFile. Fails if a directory entry with the same name already exists.
func CreateExclusiveAt(dirFile *os.File, name string, permissions os.FileMode) (*os.File, error) {
	return openAt(dirFile, name, unix.O_RDWR|unix.O_CREAT|unix.O_EXCL, permissions)
}

// Create the specified directory, open it and return the file object. If the
// directory already exists, it is opened and returned, without changing its
// permissions, matching the behavior of CreateAt().
func CreateDirAt(parent *os.File, name string, permissions os.FileMode) (*os.File, error) {
	if err := MkdirAt(parent, name, permissions); err != nil {
		if errors.Is(err, unix.EEXIST) {
			return OpenDirAt(parent, name)
		}
		return nil, err
	}
	return OpenDirAt(parent, name)
}

// Internal helper to wrap the unix.Openat syscall
func openAt(dirFile *os.File, name string, flags int, perm os.FileMode) (ans *os.File, err error) {
	dirFd := int(dirFile.Fd())
	// Call the underlying system call
	var fd int
	for {
		if fd, err = unix.Openat(dirFd, name, flags|unix.O_CLOEXEC, uint32(perm)); err != unix.EINTR {
			break
		}
	}
	name = filepath.Join(dirFile.Name(), name)
	if err != nil {
		return nil, &os.PathError{Op: "openat", Path: name, Err: err}
	}
	return os.NewFile(uintptr(fd), name), nil
}

type UnixFileInfo struct {
	name string
	stat *unix.Stat_t
	mode os.FileMode
}

func NewUnixFileInfo(name string, stat *unix.Stat_t) os.FileInfo {
	rawMode := stat.Mode
	// Start with the standard 9-bit permissions
	mode := os.FileMode(rawMode & 0777)

	// Map the file type bits using S_IFMT
	switch rawMode & unix.S_IFMT {
	case unix.S_IFDIR:
		mode |= os.ModeDir
	case unix.S_IFLNK:
		mode |= os.ModeSymlink
	case unix.S_IFBLK:
		mode |= os.ModeDevice
	case unix.S_IFCHR:
		// Go uses ModeDevice | ModeCharDevice for character devices
		mode |= os.ModeDevice | os.ModeCharDevice
	case unix.S_IFIFO:
		mode |= os.ModeNamedPipe
	case unix.S_IFSOCK:
		mode |= os.ModeSocket
	}

	// Map setuid, setgid, and sticky bits
	if rawMode&unix.S_ISUID != 0 {
		mode |= os.ModeSetuid
	}
	if rawMode&unix.S_ISGID != 0 {
		mode |= os.ModeSetgid
	}
	if rawMode&unix.S_ISVTX != 0 {
		mode |= os.ModeSticky
	}
	return &UnixFileInfo{name, stat, mode}
}

func (m *UnixFileInfo) Name() string       { return m.name }
func (m *UnixFileInfo) Size() int64        { return m.stat.Size }
func (m *UnixFileInfo) Mode() os.FileMode  { return m.mode }
func (m *UnixFileInfo) ModTime() time.Time { return time.Unix(m.stat.Mtim.Unix()) }
func (m *UnixFileInfo) IsDir() bool        { return m.Mode().IsDir() }
func (m *UnixFileInfo) Sys() any           { return m.stat }
func (m *UnixFileInfo) Dev() uint64        { return uint64(m.stat.Rdev) }

// Get file info relative to the parent FD, follows symlinks
func StatAt(dirFile *os.File, name string) (ans os.FileInfo, err error) {
	var stat unix.Stat_t
	for {
		if err = unix.Fstatat(int(dirFile.Fd()), name, &stat, 0); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		name = filepath.Join(dirFile.Name(), name)
		return nil, &os.PathError{Op: "statat", Path: name, Err: err}
	}
	return NewUnixFileInfo(name, &stat), nil
}

// Get file info relative to the parent FD, do not follows symlinks
func LstatAt(dirFile *os.File, name string) (ans os.FileInfo, err error) {
	var stat unix.Stat_t
	for {
		if err = unix.Fstatat(int(dirFile.Fd()), name, &stat, unix.AT_SYMLINK_NOFOLLOW); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		name = filepath.Join(dirFile.Name(), name)
		return nil, &os.PathError{Op: "lstatat", Path: name, Err: err}
	}
	return NewUnixFileInfo(name, &stat), nil
}

// Remove file relative to parent fd
func UnlinkAt(parent *os.File, name string) (err error) {
	for {
		if err = unix.Unlinkat(int(parent.Fd()), name, 0); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		err = &os.PathError{Op: "unlinkat", Path: filepath.Join(parent.Name(), name), Err: err}
	}
	return
}

// Remove empty directory relative to parent fd
func RemoveDirAt(parent *os.File, name string) (err error) {
	for {
		if err = unix.Unlinkat(int(parent.Fd()), name, unix.AT_REMOVEDIR); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		err = &os.PathError{Op: "unlinkat", Path: filepath.Join(parent.Name(), name), Err: err}
	}
	return
}

// Create a hardlink pointing to oldname called newname relative to the
// specified directories. If oldname is a symlink,
// a new symlink is created pointing to its target when follow_symlinks is true otherwise to it.
func LinkAt(oldparent *os.File, oldname string, newparent *os.File, newname string, follow_symlinks bool) (err error) {
	flags := IfElse(follow_symlinks, unix.AT_SYMLINK_FOLLOW, 0)
	for {
		if err = unix.Linkat(int(oldparent.Fd()), oldname, int(newparent.Fd()), newname, flags); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		err = &os.PathError{Op: "linkat", Path: fmt.Sprintf("%s -> %s", filepath.Join(newparent.Name(), newname), filepath.Join(oldparent.Name(), oldname)), Err: err}
	}
	return
}

func DupFile(f *os.File) (ans *os.File, err error) {
	var fd int
	for {
		if fd, err = unix.Dup(int(f.Fd())); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		return nil, &os.PathError{Op: "dup", Path: f.Name(), Err: err}
	}
	return os.NewFile(uintptr(fd), f.Name()), nil
}

func ReadLinkAt(parent *os.File, name string) (ans string, err error) {
	buf := [unix.PathMax]byte{}
	n, err := readLinkAt(parent, name, buf[:])
	if err != nil {
		return "", &os.PathError{Op: "readlinkat", Path: filepath.Join(parent.Name(), name), Err: err}
	}
	return UnsafeBytesToString(buf[:n]), nil
}

func RenameAt(old_parent *os.File, old_name string, new_parent *os.File, new_name string) (err error) {
	for {
		if err = unix.Renameat(int(old_parent.Fd()), old_name, int(new_parent.Fd()), new_name); err != unix.EINTR {
			break
		}
	}
	if err != nil {
		err = &os.LinkError{Op: "renameat", Old: filepath.Join(old_parent.Name(), old_name), New: filepath.Join(new_parent.Name(), new_name), Err: err}
	}
	return
}

func ConvertFileModeToUnix(goMode os.FileMode) uint32 {
	// 1. Start with the basic permission bits (0777)
	unixMode := uint32(goMode.Perm())

	// 2. Map the type bits
	// We use the os.ModeXXX constants to identify the type
	switch {
	case goMode.IsDir():
		unixMode |= unix.S_IFDIR
	case goMode&os.ModeSymlink != 0:
		unixMode |= unix.S_IFLNK
	case goMode&os.ModeNamedPipe != 0:
		unixMode |= unix.S_IFIFO
	case goMode&os.ModeSocket != 0:
		unixMode |= unix.S_IFSOCK
	case goMode&os.ModeDevice != 0:
		if goMode&os.ModeCharDevice != 0 {
			unixMode |= unix.S_IFCHR
		} else {
			unixMode |= unix.S_IFBLK
		}
	default:
		// Default to a regular file
		unixMode |= unix.S_IFREG
	}

	// 3. Map special bits
	if goMode&os.ModeSetuid != 0 {
		unixMode |= unix.S_ISUID
	}
	if goMode&os.ModeSetgid != 0 {
		unixMode |= unix.S_ISGID
	}
	if goMode&os.ModeSticky != 0 {
		unixMode |= unix.S_ISVTX
	}

	return unixMode
}

func MknodAt(parent *os.File, name string, mode os.FileMode, dev uint64) (err error) {
	unix_mode := ConvertFileModeToUnix(mode)
	if err = mknodAt(parent, name, unix_mode, dev); err != nil {
		err = &os.PathError{Op: "mknodat", Path: filepath.Join(parent.Name(), name), Err: err}
	}
	return
}

package utils

import (
	"container/list"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync/atomic"

	"github.com/kovidgoyal/go-parallel"
)

var _ = fmt.Print

// RemoveChildren removes all files and subdirectories within the directory
// pointed to by dirFile using an explicit stack instead of recursion. Removes
// all it can but returns the first error, if any.
func RemoveChildren(dirFile *os.File) error {
	var firstErr error

	// Each stack frame is one of two kinds:
	//   expand: dir != nil  – read dir's children, unlink files, push subdirs
	//   rmdir:  dir == nil  – rmdir name from parent, then Unref parent
	type frame struct {
		dir    *RefCountedFile // non-nil: expand this directory (Unref when done)
		name   string          // rmdir sentinel: child name to remove from parent
		parent *RefCountedFile // rmdir sentinel: ref to parent dir (Unref after rmdir)
	}

	// Only the root directory needs rewinding; it was passed in from outside
	// and may have been read before. Child dirs are freshly opened by us.
	if _, err := dirFile.Seek(0, io.SeekStart); err != nil {
		return err
	}
	rcRoot := NewRefCountedFile(dirFile)
	// Extra ref so the expand frame's Unref doesn't close the caller's fd.
	rcRoot.NewRef()

	stack := []frame{{dir: rcRoot}}

	for len(stack) > 0 {
		f := stack[len(stack)-1]
		stack = stack[:len(stack)-1]

		if f.dir == nil {
			// rmdir sentinel: the subdirectory is now empty, remove it.
			if err := RemoveDirAt(f.parent.File(), f.name); err != nil && firstErr == nil {
				firstErr = err
			}
			f.parent.Unref()
			continue
		}

		for {
			// Read entries in small chunks to handle very large directories.
			entries, err := f.dir.File().ReadDir(64)
			for _, entry := range entries {
				name := entry.Name()
				if entry.IsDir() {
					childFile, openErr := OpenDirAt(f.dir.File(), name)
					if openErr != nil {
						if firstErr == nil {
							firstErr = openErr
						}
						continue
					}
					// Push rmdir sentinel first; LIFO ensures the child expand
					// frame is processed before this rmdir sentinel.
					// f.dir.NewRef() adds a ref to the parent for the sentinel.
					stack = append(stack, frame{name: name, parent: f.dir.NewRef()}, frame{dir: NewRefCountedFile(childFile)})
				} else {
					if unlinkErr := UnlinkAt(f.dir.File(), name); unlinkErr != nil && firstErr == nil {
						firstErr = unlinkErr
					}
				}
			}
			if err != nil {
				if !errors.Is(err, io.EOF) && firstErr == nil {
					firstErr = &os.PathError{Op: "readdir", Path: f.dir.File().Name(), Err: err}
				}
				break
			}
		}

		f.dir.Unref()
	}

	_, _ = dirFile.Seek(0, io.SeekStart)
	return firstErr
}

// Not thread safe reference counted wrapper for os.File
type RefCountedFile struct {
	f      *os.File
	refcnt atomic.Int32
}

func NewRefCountedFile(f *os.File) *RefCountedFile {
	ans := RefCountedFile{f: f}
	ans.refcnt.Add(1)
	return &ans
}

func (f *RefCountedFile) NewRef() *RefCountedFile {
	f.refcnt.Add(1)
	return f
}

func (f *RefCountedFile) Unref() *RefCountedFile {
	if f.refcnt.Add(-1) == 0 {
		f.f.Close()
		f.f = nil
	}
	return nil
}

func (f *RefCountedFile) File() *os.File { return f.f }

type CopyFolderOptions struct {
	Disallow_hardlinks bool
	Follow_symlinks    bool
	Filter_files       func(parent *os.File, child os.FileInfo) bool
}

// Copy the file objects as efficiently as possible with cancellation. The
// files are always closed before this function returns.
func CopyFileAndClose(ctx context.Context, src *os.File, dest *os.File) (err error) {
	err_chan := make(chan error)
	go func() {
		defer func() {
			if r := recover(); r != nil {
				err_chan <- parallel.Format_stacktrace_on_panic(r, 1)
			}
		}()
		// this go routine will automatically exit when src/dest are closed
		// even if copying is not complete. io.Copy() automatically use
		// sendfile() or similar mechanisms for efficiency.
		_, err := io.Copy(dest, src)
		err_chan <- err
	}()

	select {
	case <-ctx.Done():
		src.Close()
		dest.Close()
		// wait for go routine to exit
		<-err_chan
		return ctx.Err()
	case err := <-err_chan:
		src.Close()
		dest.Close()
		return err
	}
}

// Copy the contents of src_folder to dest_folder, recursively, preserving file
// permissions. Behavior around hard and symbolic links and filtering is
// controlled via the provided options. When symlink following is enabled,
// symlink loops are avoided and any sumlink that points to an already copied
// entry becomes a symlink pointing to the copied entry using a relative path.
// Existing regular files are overwritten, without changing their permissions.
// Existing directories also do not have their permissions updated.
func CopyFolderContents(ctx context.Context, src_folder *os.File, dest_folder *os.File, opts CopyFolderOptions) (final_error error) {
	// Ensure we get all dir contents
	_, err := src_folder.Seek(0, io.SeekStart)
	if err != nil {
		return err
	}
	// When following symlinks, store previously seen source items with the
	// abspaths they have been copied to in dest. Items are identified with
	// device + inode number which should be globally unique. This ensures 1)
	// no file/dir is copied more than once because of a symlink 2) symlinks
	// are changed to point to the relative location of the previously copied
	// target
	type dir_ident struct{ dev, inode uint64 }
	get_dir_ident := func(i os.FileInfo, path string) (dir_ident, error) {
		dev, inode, err := FileIdentity(i, path)
		return dir_ident{dev, inode}, err
	}
	var seen_map map[dir_ident]string
	if opts.Follow_symlinks {
		seen_map = make(map[dir_ident]string)
		if s, err := src_folder.Stat(); err != nil {
			return err
		} else {
			id, err := get_dir_ident(s, src_folder.Name())
			if err != nil {
				return err
			}
			seen_map[id] = dest_folder.Name()
		}
	}

	is_ok := opts.Filter_files
	if is_ok == nil {
		is_ok = func(*os.File, os.FileInfo) bool { return true }
	}
	type item struct {
		src_parent, dest_parent *RefCountedFile
		child                   os.FileInfo
	}
	queue := list.New()
	is_cancelled := func() bool {
		select {
		case <-ctx.Done():
			final_error = ctx.Err()
			return true
		default:
			return false
		}
	}
	defer func() {
		for {
			v := queue.Front()
			if v == nil {
				break
			}
			item := queue.Remove(v).(*item)
			if item.src_parent != nil {
				item.src_parent.Unref()
			}
			if item.dest_parent != nil {
				item.dest_parent.Unref()
			}
		}
	}()

	src, dest := NewRefCountedFile(src_folder), NewRefCountedFile(dest_folder)
	// Add an extra reference so that the files passed into this function are
	// not closed in do_one()
	src.NewRef()
	dest.NewRef()
	fail := func(lerr error) bool {
		final_error = lerr
		return false
	}
	mark_as_seen := func(src_parent, dest_parent *os.File, child os.FileInfo) bool {
		if opts.Follow_symlinks {
			id, err := get_dir_ident(child, filepath.Join(src_parent.Name(), child.Name()))
			if err != nil {
				return fail(err)
			}
			path := filepath.Join(dest_parent.Name(), child.Name())
			seen_map[id] = filepath.Clean(path)
		}
		return true
	}

	var do_one_child func(src *RefCountedFile, dest *RefCountedFile, child os.FileInfo, from_symlink bool) bool
	do_one_child = func(src *RefCountedFile, dest *RefCountedFile, child os.FileInfo, from_symlink bool) bool {
		if child.IsDir() {
			queue.PushBack(&item{src.NewRef(), dest.NewRef(), child})
			return true
		}
		if !mark_as_seen(src.File(), dest.File(), child) {
			return false
		}
		// First try a hardlink which works for regular files and symlinks at least
		if !opts.Disallow_hardlinks && LinkAt(src.File(), child.Name(), dest.File(), child.Name(), opts.Follow_symlinks) == nil {
			return true
		}
		t := child.Mode().Type()
		switch {
		case t.IsRegular():
			sf, err := OpenAt(src.File(), child.Name())
			if err != nil {
				return fail(err)
			}
			df, err := CreateAt(dest.File(), child.Name(), child.Mode().Perm())
			if err != nil {
				sf.Close()
				return fail(err)
			}
			if err = CopyFileAndClose(ctx, sf, df); err != nil {
				UnlinkAt(dest.File(), child.Name()) // dont leave partially copied files around
				return fail(err)
			}
		case t&os.ModeSymlink != 0:
			if opts.Follow_symlinks && !from_symlink {
				rpath, err := filepath.EvalSymlinks(filepath.Join(src.File().Name(), child.Name()))
				if err != nil {
					return do_one_child(src, dest, child, true)
				}
				parent_dir := filepath.Dir(rpath)
				pdf, err := OpenDir(parent_dir)
				if err != nil {
					return do_one_child(src, dest, child, true)
				}
				child_name := filepath.Base(rpath)
				return func() bool {
					// Use a RefCountedFile so that if st is a directory the fd
					// stays alive until the queued item is processed by next_dir.
					rcf := NewRefCountedFile(pdf)
					defer rcf.Unref()
					st, err := StatAt(pdf, child_name)
					if err != nil {
						return do_one_child(src, dest, child, true)
					}
					id, err := get_dir_ident(st, rpath)
					if err != nil {
						return fail(err)
					}
					if existing_path, found := seen_map[id]; found {
						target, err := filepath.Rel(dest.File().Name(), existing_path)
						if err != nil {
							return do_one_child(src, dest, child, true)
						}
						if err = SymlinkAt(dest.File(), child.Name(), target); err != nil {
							return fail(err)
						}
						return true
					}
					return do_one_child(rcf, dest, st, true)
				}()
			} else {
				target, err := ReadLinkAt(src.File(), child.Name())
				if err != nil {
					return fail(err)
				}
				if err = SymlinkAt(dest.File(), child.Name(), target); err != nil {
					return fail(err)
				}
			}
		case t&os.ModeDevice != 0:
			if err := MknodAt(dest.File(), child.Name(), child.Mode(), DeviceNumber(child)); err != nil {
				return fail(err)
			}
		}
		return true
	}

	do_one := func(src *RefCountedFile, dest *RefCountedFile) bool {
		defer func() {
			src = src.Unref()
			dest = dest.Unref()
		}()
		if is_cancelled() {
			return false
		}
		for {
			dir_entries, err := src.File().ReadDir(64)
			if err != nil {
				if errors.Is(err, io.EOF) {
					break
				}
				return fail(err)
			}
			for _, entry := range dir_entries {
				if is_cancelled() {
					return false
				}
				child, err := entry.Info()
				if err != nil {
					return fail(err)
				}
				if !is_ok(src.File(), child) {
					continue
				}
				if !do_one_child(src, dest, child, false) {
					return false
				}
			}
		}
		return true
	}

	next_dir := func(src_parent *RefCountedFile, dest_parent *RefCountedFile, child os.FileInfo) (ok bool) {
		src, dest = nil, nil
		defer func() {
			src_parent.Unref()
			dest_parent.Unref()
			if !ok {
				if src != nil {
					src = src.Unref()
				}
				if dest != nil {
					dest = dest.Unref()
				}
			}
		}()
		if !mark_as_seen(src_parent.File(), dest_parent.File(), child) {
			return false
		}
		sf, err := OpenDirAt(src_parent.File(), child.Name())
		if err != nil {
			final_error = err
			return false
		}
		df, err := CreateDirAt(dest_parent.File(), child.Name(), child.Mode().Perm())
		if err != nil {
			sf.Close()
			final_error = err
			return false
		}
		src, dest = NewRefCountedFile(sf), NewRefCountedFile(df)
		ok = true
		return
	}

	for {
		if !do_one(src, dest) {
			return
		}
		v := queue.Front()
		if v == nil {
			break
		}
		n := queue.Remove(v).(*item)
		if !next_dir(n.src_parent, n.dest_parent, n.child) {
			return
		}
	}
	return
}

// License: GPLv3 Copyright: 2023, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build !windows

package ssh

import (
	"archive/tar"
	"os"
	"path"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/sys/unix"
)

func get_file_data(callback func(h *tar.Header, data []byte) error, seen map[file_unique_id]string, local_path, arcname string, exclude_patterns []string) error {
	var s unix.Stat_t
	if err := unix.Lstat(local_path, &s); err != nil {
		return err
	}
	cb := func(h *tar.Header, data []byte, arcname string, st *unix.Stat_t) error {
		h.Name = arcname
		if h.Typeflag == tar.TypeDir {
			h.Name = strings.TrimRight(h.Name, "/") + "/"
		}
		h.Size = int64(len(data))
		h.Mode = int64(st.Mode & 0777) // discard the setuid, setgid and sticky bits
		h.ModTime = time.Unix(st.Mtim.Unix())
		h.AccessTime = time.Unix(st.Atim.Unix())
		h.ChangeTime = time.Unix(st.Ctim.Unix())
		h.Format = tar.FormatPAX
		return callback(h, data)
	}
	// we only copy regular files, directories and symlinks
	switch s.Mode & unix.S_IFMT {
	case unix.S_IFBLK, unix.S_IFIFO, unix.S_IFCHR, unix.S_IFSOCK: // ignored
	case unix.S_IFLNK: // symlink
		target, err := os.Readlink(local_path)
		if err != nil {
			return err
		}
		err = cb(&tar.Header{
			Typeflag: tar.TypeSymlink,
			Linkname: target,
		}, nil, arcname, &s)
		if err != nil {
			return err
		}
	case unix.S_IFDIR: // directory
		local_path = filepath.Clean(local_path)
		type entry struct {
			path, arcname string
		}
		stack := []entry{{local_path, arcname}}
		for len(stack) > 0 {
			x := stack[0]
			stack = stack[1:]
			// Each directory needs its own metadata, re-check its type as it
			// may have been replaced since its parent was read
			ds := s
			if x.path != local_path {
				if err := unix.Lstat(x.path, &ds); err != nil || ds.Mode&unix.S_IFMT != unix.S_IFDIR {
					continue
				}
			}
			entries, err := os.ReadDir(x.path)
			if err != nil {
				if x.path == local_path {
					return err
				}
				continue
			}
			err = cb(&tar.Header{Typeflag: tar.TypeDir}, nil, x.arcname, &ds)
			if err != nil {
				return err
			}
			for _, e := range entries {
				entry_path := filepath.Join(x.path, e.Name())
				aname := path.Join(x.arcname, e.Name())
				ok := true
				for _, pat := range exclude_patterns {
					if excluded(pat, entry_path) {
						ok = false
						break
					}
				}
				if !ok {
					continue
				}
				if e.IsDir() {
					stack = append(stack, entry{entry_path, aname})
				} else {
					err = get_file_data(callback, seen, entry_path, aname, exclude_patterns)
					if err != nil {
						return err
					}
				}
			}
		}
	case unix.S_IFREG: // Regular file
		fid := file_unique_id{dev: uint64(s.Dev), inode: uint64(s.Ino)}
		if prev, ok := seen[fid]; ok { // Hard link
			return cb(&tar.Header{Typeflag: tar.TypeLink, Linkname: prev}, nil, arcname, &s)
		}
		seen[fid] = arcname
		data, err := os.ReadFile(local_path)
		if err != nil {
			return err
		}
		err = cb(&tar.Header{Typeflag: tar.TypeReg}, data, arcname, &s)
		if err != nil {
			return err
		}
	}
	return nil
}

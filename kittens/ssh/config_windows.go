// License: GPLv3 Copyright: 2025, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package ssh

import (
	"archive/tar"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/kovidgoyal/kitty/tools/utils"
)

func get_file_data(callback func(h *tar.Header, data []byte) error, seen map[file_unique_id]string, local_path, arcname string, exclude_patterns []string) error {
	s, err := os.Lstat(local_path)
	if err != nil {
		return err
	}
	// The mode Go reports on Windows is 0o666 or 0o777 for everything
	mode := int64(utils.PosixPermissions(local_path, s))
	cb := func(h *tar.Header, data []byte, arcname string) error {
		h.Name = arcname
		if h.Typeflag == tar.TypeDir {
			h.Name = strings.TrimRight(h.Name, "/") + "/"
		}
		h.Size = int64(len(data))
		h.Mode = mode
		h.ModTime = s.ModTime()
		h.AccessTime = s.ModTime()
		h.ChangeTime = s.ModTime()
		h.Format = tar.FormatPAX
		return callback(h, data)
	}
	// we only copy regular files, directories and symlinks
	switch {
	case s.Mode()&os.ModeSymlink != 0:
		target, err := os.Readlink(local_path)
		if err != nil {
			return err
		}
		err = cb(&tar.Header{
			Typeflag: tar.TypeSymlink,
			Linkname: filepath.ToSlash(target),
		}, nil, arcname)
		if err != nil {
			return err
		}
	case s.IsDir():
		local_path = filepath.Clean(local_path)
		type entry struct {
			path, arcname string
		}
		stack := []entry{{local_path, arcname}}
		for len(stack) > 0 {
			x := stack[0]
			stack = stack[1:]
			entries, err := os.ReadDir(x.path)
			if err != nil {
				if x.path == local_path {
					return err
				}
				continue
			}
			err = cb(&tar.Header{Typeflag: tar.TypeDir}, nil, x.arcname)
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
	case s.Mode().IsRegular():
		dev, ino, err := utils.FileIdentity(s, local_path)
		if err != nil {
			return err
		}
		fid := file_unique_id{dev: dev, inode: ino}
		if prev, ok := seen[fid]; ok { // Hard link
			return cb(&tar.Header{Typeflag: tar.TypeLink, Linkname: prev}, nil, arcname)
		}
		seen[fid] = arcname
		data, err := os.ReadFile(local_path)
		if err != nil {
			return err
		}
		err = cb(&tar.Header{Typeflag: tar.TypeReg}, data, arcname)
		if err != nil {
			return err
		}
	}
	return nil
}

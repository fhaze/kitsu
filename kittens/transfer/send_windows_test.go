//go:build windows

package transfer

import (
	"os"
	"path/filepath"
	"testing"

	"golang.org/x/sys/windows"
)

func TestSendFileIdentity(t *testing.T) {
	d := t.TempDir()
	for name, data := range map[string]string{"a": "first", "b": "other", "empty-a": "", "empty-b": ""} {
		if err := os.WriteFile(filepath.Join(d, name), []byte(data), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{"dir-a", "dir-b"} {
		if err := os.Mkdir(filepath.Join(d, name), 0o700); err != nil {
			t.Fatal(err)
		}
	}
	created := windows.NsecToFiletime(1700000000000000000)
	for _, name := range []string{"a", "b", "empty-a", "empty-b", "dir-a", "dir-b"} {
		p, err := windows.UTF16PtrFromString(filepath.Join(d, name))
		if err != nil {
			t.Fatal(err)
		}
		h, err := windows.CreateFile(p, windows.FILE_WRITE_ATTRIBUTES, windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE|windows.FILE_SHARE_DELETE, nil, windows.OPEN_EXISTING, windows.FILE_FLAG_BACKUP_SEMANTICS, 0)
		if err != nil {
			t.Fatal(err)
		}
		err = windows.SetFileTime(h, &created, nil, nil)
		windows.CloseHandle(h)
		if err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Link(filepath.Join(d, "a"), filepath.Join(d, "hardlink")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("a", filepath.Join(d, "symlink")); err != nil {
		t.Fatal(err)
	}
	files, err := files_for_send(&Options{}, []string{d, "/dest/"})
	if err != nil {
		t.Fatal(err)
	}
	by_name := make(map[string]*File)
	for _, f := range files {
		by_name[filepath.Base(f.expanded_local_path)] = f
	}
	for _, name := range []string{"a", "b", "empty-a", "empty-b"} {
		if f := by_name[name]; f == nil || f.file_type != FileType_regular {
			t.Errorf("%s must be a regular file, got %+v", name, f)
		}
	}
	for _, name := range []string{filepath.Base(d), "dir-a", "dir-b"} {
		if f := by_name[name]; f == nil || f.file_type != FileType_directory {
			t.Errorf("%s must be a directory, got %+v", name, f)
		}
	}
	if f := by_name["hardlink"]; f.file_type != FileType_link || f.hard_link_target != "fid:"+by_name["a"].file_id {
		t.Errorf("hard link not preserved: %+v", f)
	}
	if f := by_name["symlink"]; f.file_type != FileType_symlink || f.symbolic_link_target != "fid:"+by_name["a"].file_id {
		t.Errorf("symbolic link not preserved: %+v", f)
	}
}

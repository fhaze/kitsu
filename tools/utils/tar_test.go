package utils

import (
	"archive/tar"
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"testing"

	"github.com/google/go-cmp/cmp"
)

var _ = fmt.Print

func TestTarExtract(t *testing.T) {
	tdir := t.TempDir()
	a, b := filepath.Join(tdir, "a"), filepath.Join(tdir, "b")
	if err := os.Mkdir(a, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(b, 0700); err != nil {
		t.Fatal(err)
	}
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	var files = []struct {
		name, body string
	}{
		{"s/one.txt", "This archive contains some text files."},
		{"b", b},
		{"b/two.txt", "Get animal handling license."},
		{"../b/three.txt", "Get animal handling license."},
		{"nested/dir/", ""},
	}
	for _, file := range files {
		hdr := &tar.Header{
			Name: file.name,
			Mode: 0600,
			Size: int64(len(file.body)),
		}
		if file.name == "b" {
			hdr.Linkname = file.body
			hdr.Typeflag = tar.TypeSymlink
			hdr.Size = 0
		}
		if err := tw.WriteHeader(hdr); err != nil {
			t.Fatal(err)
		}
		if hdr.Typeflag != tar.TypeSymlink && len(file.body) > 0 {
			if _, err := tw.Write([]byte(file.body)); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	tr := tar.NewReader(&buf)
	count, err := ExtractAllFromTar(tr, a)
	if err != nil {
		t.Fatal(err)
	}
	if count != len(files)-2 {
		t.Fatalf("Incorrect count of extracted files: %d != %d", count, len(files)-2)
	}
	entries := []string{}
	if err = fs.WalkDir(os.DirFS(tdir), ".", func(path string, d fs.DirEntry, err error) error {
		entries = append(entries, path)
		return err
	},
	); err != nil {
		t.Fatal(err)
	}
	if diff := cmp.Diff([]string{".", "a", "a/b", "a/nested", "a/nested/dir", "a/s", "a/s/one.txt", "b"}, entries); diff != "" {
		t.Fatalf("Directory contents not as expected: %s", diff)
	}
}

type tar_entry struct {
	name, body, linkname string
	typeflag             byte
	mode                 int64
}

func make_tar(t *testing.T, entries ...tar_entry) *tar.Reader {
	t.Helper()
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	for _, e := range entries {
		typeflag := e.typeflag
		if typeflag == 0 {
			typeflag = tar.TypeReg
		}
		mode := e.mode
		if mode == 0 {
			mode = 0600
		}
		hdr := tar.Header{Name: e.name, Mode: mode, Typeflag: typeflag, Linkname: e.linkname, Size: int64(len(e.body))}
		if err := tw.WriteHeader(&hdr); err != nil {
			t.Fatal(err)
		}
		if _, err := tw.Write([]byte(e.body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	return tar.NewReader(&buf)
}

func assert_same_file(t *testing.T, a, b string) {
	t.Helper()
	sa, err := os.Stat(a)
	if err != nil {
		t.Fatal(err)
	}
	sb, err := os.Stat(b)
	if err != nil {
		t.Fatal(err)
	}
	if !os.SameFile(sa, sb) {
		t.Fatalf("%s and %s are not hardlinks", a, b)
	}
}

func assert_file_contents(t *testing.T, path, expected string) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != expected {
		t.Fatalf("Contents of %s not as expected: %#v != %#v", path, string(data), expected)
	}
}

func TestTarExtractHardlinks(t *testing.T) {
	for _, tc := range []struct {
		name, target, link string
	}{
		{"root", "target.txt", "link.txt"},
		{"same_directory", "dir/target.txt", "dir/link.txt"},
		{"cross_directory", "one/target.txt", "two/link.txt"},
		{"dot_slash_prefix", "./dir/target.txt", "./dir/link.txt"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dest := t.TempDir()
			count, err := ExtractAllFromTar(make_tar(t,
				tar_entry{name: tc.target},
				tar_entry{name: tc.link, typeflag: tar.TypeLink, linkname: tc.target},
			), dest)
			if err != nil {
				t.Fatal(err)
			}
			if count != 2 {
				t.Fatalf("Expected two extracted files, got %d", count)
			}
			assert_same_file(t, filepath.Join(dest, tc.target), filepath.Join(dest, tc.link))
		})
	}

	t.Run("through_symlinked_directory", func(t *testing.T) {
		dest := t.TempDir()
		if _, err := ExtractAllFromTar(make_tar(t,
			tar_entry{name: "real/target.txt"},
			tar_entry{name: "alias", typeflag: tar.TypeSymlink, linkname: "real"},
			tar_entry{name: "two/link.txt", typeflag: tar.TypeLink, linkname: "alias/target.txt"},
		), dest); err != nil {
			t.Fatal(err)
		}
		assert_same_file(t, filepath.Join(dest, "real/target.txt"), filepath.Join(dest, "two/link.txt"))
	})

	t.Run("to_symlink", func(t *testing.T) {
		dest := t.TempDir()
		if _, err := ExtractAllFromTar(make_tar(t,
			tar_entry{name: "target.txt", body: "target"},
			tar_entry{name: "sym", typeflag: tar.TypeSymlink, linkname: "target.txt"},
			tar_entry{name: "dir/link", typeflag: tar.TypeLink, linkname: "sym"},
		), dest); err != nil {
			t.Fatal(err)
		}
		link := filepath.Join(dest, "dir/link")
		st, err := os.Lstat(link)
		if err != nil {
			t.Fatal(err)
		}
		if st.Mode()&fs.ModeSymlink == 0 {
			t.Fatalf("Hardlink to symlink is not a symlink: %v", st.Mode())
		}
		assert_file_contents(t, link, "target")
	})

	t.Run("duplicate_entries", func(t *testing.T) {
		dest := t.TempDir()
		if _, err := ExtractAllFromTar(make_tar(t,
			tar_entry{name: "a", body: "one"},
			tar_entry{name: "b", typeflag: tar.TypeLink, linkname: "a"},
			tar_entry{name: "b", typeflag: tar.TypeLink, linkname: "a"},
			tar_entry{name: "a", typeflag: tar.TypeLink, linkname: "a"},
			tar_entry{name: "c", typeflag: tar.TypeLink, linkname: "a"},
			tar_entry{name: "c", body: "two"},
			tar_entry{name: "s", typeflag: tar.TypeSymlink, linkname: "a"},
			tar_entry{name: "s", typeflag: tar.TypeSymlink, linkname: "c"},
		), dest); err != nil {
			t.Fatal(err)
		}
		assert_same_file(t, filepath.Join(dest, "a"), filepath.Join(dest, "b"))
		// replacing a hardlinked file must not change the other links
		assert_file_contents(t, filepath.Join(dest, "a"), "one")
		assert_file_contents(t, filepath.Join(dest, "c"), "two")
		assert_file_contents(t, filepath.Join(dest, "s"), "two")
	})

	outside := func(t *testing.T, entries ...tar_entry) {
		parent := t.TempDir()
		dest := filepath.Join(parent, "dest")
		if err := os.Mkdir(dest, 0700); err != nil {
			t.Fatal(err)
		}
		outside_file := filepath.Join(parent, "outside.txt")
		if err := os.WriteFile(outside_file, []byte("outside"), 0600); err != nil {
			t.Fatal(err)
		}
		original, err := os.Stat(outside_file)
		if err != nil {
			t.Fatal(err)
		}
		for i := range entries {
			entries[i].linkname = strings.ReplaceAll(entries[i].linkname, "PARENT", parent)
		}
		if _, err := ExtractAllFromTar(make_tar(t, entries...), dest); err == nil {
			t.Fatal("Hardlink to file outside destination did not fail")
		}
		if _, err := os.Lstat(filepath.Join(dest, "link.txt")); !os.IsNotExist(err) {
			t.Fatalf("Hardlink to file outside destination was created: %v", err)
		}
		st, err := os.Stat(outside_file)
		if err != nil {
			t.Fatal(err)
		}
		if st.Mode().Perm() != original.Mode().Perm() {
			t.Fatalf("Permissions of file outside destination were changed: %v", st.Mode())
		}
	}
	t.Run("outside_destination", func(t *testing.T) {
		outside(t, tar_entry{name: "link.txt", typeflag: tar.TypeLink, linkname: "../outside.txt"})
	})
	t.Run("outside_destination_absolute", func(t *testing.T) {
		outside(t, tar_entry{name: "link.txt", typeflag: tar.TypeLink, linkname: "PARENT/outside.txt"})
	})
	t.Run("outside_destination_via_symlink", func(t *testing.T) {
		outside(t,
			tar_entry{name: "s", typeflag: tar.TypeSymlink, linkname: "PARENT"},
			tar_entry{name: "link.txt", typeflag: tar.TypeLink, linkname: "s/outside.txt"},
		)
	})
}

func TestTarExtractPreexistingHardlink(t *testing.T) {
	parent := t.TempDir()
	dest, victim := filepath.Join(parent, "dest"), filepath.Join(parent, "victim")
	if err := os.Mkdir(dest, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(victim, []byte("unchanged"), 0600); err != nil {
		t.Fatal(err)
	}
	output := filepath.Join(dest, "file")
	if err := os.Link(victim, output); err != nil {
		t.Fatal(err)
	}
	assert_same_file(t, output, victim)
	before, err := os.Stat(victim)
	if err != nil {
		t.Fatal(err)
	}
	count, err := ExtractAllFromTar(make_tar(t, tar_entry{name: "file", body: "changed", mode: 0400}), dest)
	if count != 1 || err != nil {
		t.Fatalf("extraction: count=%d error=%v", count, err)
	}
	t.Cleanup(func() { _ = os.Chmod(output, 0600) })
	assert_file_contents(t, victim, "unchanged")
	assert_file_contents(t, output, "changed")
	after, err := os.Stat(victim)
	if err != nil || !os.SameFile(before, after) || before.Mode() != after.Mode() {
		t.Fatalf("outside sentinel identity or metadata changed: %v", err)
	}
	inside, err := os.Stat(output)
	if err != nil || os.SameFile(inside, after) {
		t.Fatalf("output was not detached from the outside hardlink: %v", err)
	}
}

func TestTarExtractReadOnlyDirectory(t *testing.T) {
	dest := t.TempDir()
	ro := filepath.Join(dest, "ro")
	t.Cleanup(func() { _ = os.Chmod(ro, 0700) })
	if _, err := ExtractAllFromTar(make_tar(t,
		tar_entry{name: "ro/", typeflag: tar.TypeDir, mode: 0555},
		tar_entry{name: "ro/file.txt", body: "contents"},
	), dest); err != nil {
		t.Fatal(err)
	}
	assert_file_contents(t, filepath.Join(ro, "file.txt"), "contents")
	st, err := os.Stat(ro)
	if err != nil {
		t.Fatal(err)
	}
	if st.Mode().Perm() != 0555 {
		t.Fatalf("Permissions of read-only directory not preserved: %v", st.Mode())
	}
}

func TestTarExtractUnsearchableParentDirectory(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows directories do not have POSIX search permissions")
	}
	dest := t.TempDir()
	parent := filepath.Join(dest, "a")
	t.Cleanup(func() {
		_ = os.Chmod(parent, 0700)
		_ = os.Chmod(filepath.Join(parent, "b"), 0700)
	})
	if _, err := ExtractAllFromTar(make_tar(t,
		tar_entry{name: "a/", typeflag: tar.TypeDir, mode: 0600},
		tar_entry{name: "a/b/", typeflag: tar.TypeDir, mode: 0555},
		tar_entry{name: "a/b/file.txt", body: "contents"},
	), dest); err != nil {
		t.Fatal(err)
	}
	st, err := os.Stat(parent)
	if err != nil {
		t.Fatal(err)
	}
	if st.Mode().Perm() != 0600 {
		t.Fatalf("Permissions of parent directory not preserved: %v", st.Mode())
	}
	if err = os.Chmod(parent, 0700); err != nil {
		t.Fatal(err)
	}
	assert_file_contents(t, filepath.Join(parent, "b", "file.txt"), "contents")
	if st, err = os.Stat(filepath.Join(parent, "b")); err != nil {
		t.Fatal(err)
	}
	if st.Mode().Perm() != 0555 {
		t.Fatalf("Permissions of child directory not preserved: %v", st.Mode())
	}
}

func require_tar_symlinks(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	if err := os.Symlink(dir, filepath.Join(dir, "probe")); err != nil {
		if errors.Is(err, fs.ErrPermission) || errors.Is(err, syscall.Errno(1314)) {
			t.Skipf("symlink creation privilege unavailable: %v", err)
		}
		t.Fatal(err)
	}
}

func TestTarExtractContainment(t *testing.T) {
	for _, name := range []string{"sibling_prefix", "absolute", "nested_symlinks", "hardlink_through_symlinks", "preexisting_parent_symlink", "preexisting_file_symlink", "junction", "hardlink_through_junction", "directory_through_junction"} {
		t.Run(name, func(t *testing.T) {
			parent := t.TempDir()
			dest, outside := filepath.Join(parent, "dest"), filepath.Join(parent, "dest-evil")
			for _, dir := range []string{dest, outside} {
				if err := os.Mkdir(dir, 0700); err != nil {
					t.Fatal(err)
				}
			}
			victim := filepath.Join(outside, "victim")
			if err := os.WriteFile(victim, []byte("unchanged"), 0600); err != nil {
				t.Fatal(err)
			}
			entries := []tar_entry{{name: "../dest-evil/victim", body: "changed"}}
			want_error := false
			switch name {
			case "absolute":
				entries[0].name = victim
			case "nested_symlinks", "hardlink_through_symlinks":
				require_tar_symlinks(t)
				entries = []tar_entry{
					{name: "real/escape", typeflag: tar.TypeSymlink, linkname: outside},
					{name: "alias", typeflag: tar.TypeSymlink, linkname: "real"},
					{name: "alias/escape/victim", body: "changed"},
				}
				if name == "hardlink_through_symlinks" {
					entries[2] = tar_entry{name: "link", typeflag: tar.TypeLink, linkname: "alias/escape/victim"}
					want_error = true
				}
			case "preexisting_parent_symlink", "preexisting_file_symlink":
				require_tar_symlinks(t)
				target, entry := outside, "existing/victim"
				if name == "preexisting_file_symlink" {
					target, entry = victim, "existing"
				}
				if err := os.Symlink(target, filepath.Join(dest, "existing")); err != nil {
					t.Fatal(err)
				}
				entries = []tar_entry{{name: entry, body: "changed"}}
			case "junction", "hardlink_through_junction", "directory_through_junction":
				if runtime.GOOS != "windows" {
					t.Skip("Windows directory junctions only")
				}
				junction := filepath.Join(dest, "junction")
				if out, err := exec.Command("cmd.exe", "/d", "/c", "mklink", "/J", junction, outside).CombinedOutput(); err != nil {
					t.Fatalf("creating directory junction: %v: %s", err, out)
				}
				t.Cleanup(func() { _ = os.Remove(junction) })
				entries = []tar_entry{{name: "junction/victim", body: "changed"}}
				if name == "hardlink_through_junction" {
					entries[0] = tar_entry{name: "link", typeflag: tar.TypeLink, linkname: "junction/victim"}
					want_error = true
				} else if name == "directory_through_junction" {
					entries[0] = tar_entry{name: "junction/new", typeflag: tar.TypeDir}
				}
			}
			_, err := ExtractAllFromTar(make_tar(t, entries...), dest)
			if (err != nil) != want_error {
				t.Fatalf("extraction error: %v, want error: %v", err, want_error)
			}
			assert_file_contents(t, victim, "unchanged")
			if _, err := os.Lstat(filepath.Join(outside, "new")); !os.IsNotExist(err) {
				t.Fatalf("outside directory created: %v", err)
			}
			if _, err := os.Lstat(filepath.Join(dest, "link")); !os.IsNotExist(err) {
				t.Fatalf("outside hardlink created: %v", err)
			}
			if name == "preexisting_file_symlink" {
				assert_file_contents(t, filepath.Join(dest, "existing"), "changed")
				if linked, err := is_symlink_path(filepath.Join(dest, "existing")); err != nil || linked {
					t.Fatalf("destination symlink was not replaced: %v", err)
				}
			}
		})
	}
}

func TestTarExtractJunctionChains(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows directory junctions only")
	}
	for _, target := range []string{"outside", "inside", "missing", "cycle"} {
		for _, kind := range []byte{tar.TypeReg, tar.TypeLink, tar.TypeDir} {
			t.Run(fmt.Sprintf("%s/%c", target, kind), func(t *testing.T) {
				parent := t.TempDir()
				dest, outside := filepath.Join(parent, "dest"), filepath.Join(parent, "dest-evil")
				for _, dir := range []string{dest, outside} {
					if err := os.Mkdir(dir, 0700); err != nil {
						t.Fatal(err)
					}
				}
				victim := filepath.Join(outside, "victim")
				if err := os.WriteFile(victim, []byte("unchanged"), 0600); err != nil {
					t.Fatal(err)
				}
				before, err := os.Stat(victim)
				if err != nil {
					t.Fatal(err)
				}
				want := map[string]bool{".": true, "dest": true, "dest/alias": true, "dest/hop": true, "dest-evil": true, "dest-evil/victim": true}
				link_target := outside
				if target == "inside" {
					link_target = filepath.Join(dest, "real")
					if err := os.Mkdir(link_target, 0700); err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(filepath.Join(link_target, "victim"), []byte("inside"), 0600); err != nil {
						t.Fatal(err)
					}
					want["dest/real"], want["dest/real/victim"] = true, true
				} else if target == "missing" {
					link_target = filepath.Join(outside, "missing")
				}
				junction := func(name, to string) {
					t.Helper()
					if out, err := exec.Command("cmd.exe", "/d", "/c", "mklink", "/J", name, to).CombinedOutput(); err != nil {
						t.Fatalf("creating junction: %v: %s", err, out)
					}
					t.Cleanup(func() { _ = os.Remove(name) })
				}
				alias, hop := filepath.Join(dest, "alias"), filepath.Join(dest, "hop")
				junction(hop, link_target)
				junction(alias, hop)
				if target == "cycle" {
					if err := os.Remove(hop); err != nil {
						t.Fatal(err)
					}
					junction(hop, alias)
				} else if target != "missing" {
					assert_same_file(t, filepath.Join(alias, "victim"), filepath.Join(link_target, "victim"))
				}
				entry := tar_entry{name: "alias/child/../victim", body: "changed"}
				if kind == tar.TypeLink {
					entry = tar_entry{name: "result", typeflag: kind, linkname: entry.name}
				} else if kind == tar.TypeDir {
					entry = tar_entry{name: "alias/child/../new", typeflag: kind, mode: 0700}
				}
				count, err := ExtractAllFromTar(make_tar(t, entry), dest)
				assert_file_contents(t, victim, "unchanged")
				if target == "outside" && kind == tar.TypeLink {
					if st, err := os.Stat(filepath.Join(dest, "result")); err == nil && os.SameFile(before, st) {
						t.Fatal("archive created a hardlink to the outside sentinel")
					}
				}
				want_count := 0
				if target == "inside" {
					want_count = 1
					if kind == tar.TypeLink {
						want["dest/result"] = true
						assert_same_file(t, filepath.Join(dest, "result"), filepath.Join(link_target, "victim"))
					} else if kind == tar.TypeDir {
						want["dest/real/new"] = true
					} else {
						assert_file_contents(t, filepath.Join(link_target, "victim"), "changed")
					}
				}
				want_error := target == "cycle" || (kind == tar.TypeLink && target != "inside")
				if count != want_count || (err != nil) != want_error {
					t.Fatalf("extraction: count=%d error=%v, want count=%d error=%v", count, err, want_count, want_error)
				}
				if target == "cycle" && !strings.Contains(err.Error(), "too many links") {
					t.Fatalf("junction cycle did not reach the link traversal guard: %v", err)
				}
				after, err := os.Stat(victim)
				if err != nil || !os.SameFile(before, after) || before.Mode() != after.Mode() {
					t.Fatalf("outside sentinel identity or metadata changed: %v", err)
				}
				paths := make(map[string]bool)
				if err := filepath.WalkDir(parent, func(path string, d fs.DirEntry, err error) error {
					if err != nil {
						return err
					}
					rel, err := filepath.Rel(parent, path)
					if err != nil {
						return err
					}
					paths[filepath.ToSlash(rel)] = true
					if d.IsDir() {
						if _, err := os.Readlink(path); err == nil {
							return filepath.SkipDir
						}
					}
					return nil
				}); err != nil {
					t.Fatal(err)
				}
				if diff := cmp.Diff(want, paths); diff != "" {
					t.Fatalf("unexpected extraction output (-want +got):\n%s", diff)
				}
			})
		}
	}
}

func TestTarExtractWindowsPaths(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows drive and separator semantics only")
	}
	for _, name := range []string{"drive_relative", "drive_qualified", "root_relative", "unc", "extended_length", "mixed_separators", "mixed_separators_reverse"} {
		for _, hardlink := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/hardlink=%v", name, hardlink), func(t *testing.T) {
				parent := t.TempDir()
				dest, outside := filepath.Join(parent, "dest"), filepath.Join(parent, "dest-evil")
				for _, dir := range []string{dest, outside} {
					if err := os.Mkdir(dir, 0700); err != nil {
						t.Fatal(err)
					}
				}
				victim := filepath.Join(outside, "victim")
				if err := os.WriteFile(victim, []byte("unchanged"), 0600); err != nil {
					t.Fatal(err)
				}
				volume := filepath.VolumeName(victim)
				path := map[string]string{
					"drive_relative":           volume + strings.TrimPrefix(victim, volume+`\`),
					"drive_qualified":          victim,
					"root_relative":            strings.TrimPrefix(victim, volume),
					"unc":                      `\\localhost\` + strings.TrimSuffix(volume, ":") + "$" + strings.TrimPrefix(victim, volume),
					"extended_length":          `\\?\` + victim,
					"mixed_separators":         `..\dest-evil/victim`,
					"mixed_separators_reverse": `../dest-evil\victim`,
				}[name]
				mixed := strings.HasPrefix(name, "mixed_separators")
				if mixed {
					target := filepath.Clean(filepath.Join(dest, path))
					if target != victim {
						t.Fatalf("unsafe extraction would target %q, not victim %q", target, victim)
					}
					got, err := os.Stat(target)
					want, victim_err := os.Stat(victim)
					if err != nil || victim_err != nil || !os.SameFile(got, want) {
						t.Fatalf("archive target is not the sentinel file: %v, %v", err, victim_err)
					}
				}
				entry := tar_entry{name: path, body: "changed"}
				if hardlink {
					entry = tar_entry{name: "link", typeflag: tar.TypeLink, linkname: path}
				}
				count, err := ExtractAllFromTar(make_tar(t, entry), dest)
				want_count := 0
				want_paths := map[string]bool{".": true, "dest": true, "dest-evil": true, "dest-evil/victim": true}
				// A root-relative Windows name is joined beneath dest, not the drive root.
				if name == "root_relative" && !hardlink {
					want_count = 1
					target := filepath.Join(dest, path)
					if !strings.HasPrefix(target, dest+string(os.PathSeparator)) {
						t.Fatalf("root-relative name escaped destination: %q", target)
					}
					assert_file_contents(t, target, "changed")
					relative, rel_err := filepath.Rel(parent, target)
					if rel_err != nil {
						t.Fatal(rel_err)
					}
					for relative != "dest" {
						want_paths[filepath.ToSlash(relative)] = true
						relative = filepath.Dir(relative)
					}
				}
				if count != want_count {
					t.Fatalf("extracted %d entries, want %d", count, want_count)
				}
				// Escaping members are skipped; escaping hardlink targets are errors.
				if mixed && (err != nil) != hardlink {
					t.Fatalf("extraction error: %v, want error: %v", err, hardlink)
				}
				assert_file_contents(t, victim, "unchanged")
				paths := make(map[string]bool)
				if err := fs.WalkDir(os.DirFS(parent), ".", func(path string, d fs.DirEntry, err error) error {
					paths[path] = true
					return err
				}); err != nil {
					t.Fatal(err)
				}
				if diff := cmp.Diff(want_paths, paths); diff != "" {
					t.Fatalf("unexpected extraction output (-want +got):\n%s", diff)
				}
				if count, err := ExtractAllFromTar(make_tar(t, tar_entry{name: "valid/file", body: "valid"}), dest); err != nil || count != 1 {
					t.Fatalf("valid extraction: count=%d, error=%v", count, err)
				}
				assert_file_contents(t, filepath.Join(dest, "valid", "file"), "valid")
				assert_file_contents(t, victim, "unchanged")
			})
		}
	}
}

func TestTarExtractBackslashLiterals(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX literal backslashes; Windows separators are covered by TestTarExtractWindowsPaths")
	}
	for _, name := range []string{`..\dest-evil/victim`, `..\dest-evil\victim`} {
		t.Run(name, func(t *testing.T) {
			parent := t.TempDir()
			dest, outside := filepath.Join(parent, "dest"), filepath.Join(parent, "dest-evil")
			for _, dir := range []string{dest, outside} {
				if err := os.Mkdir(dir, 0700); err != nil {
					t.Fatal(err)
				}
			}
			victim := filepath.Join(outside, "victim")
			if err := os.WriteFile(victim, []byte("unchanged"), 0600); err != nil {
				t.Fatal(err)
			}
			target := filepath.Join(dest, name)
			if target == victim || !strings.HasPrefix(target, dest+string(os.PathSeparator)) {
				t.Fatalf("POSIX backslash name did not stay inside destination: %q", target)
			}
			if count, err := ExtractAllFromTar(make_tar(t, tar_entry{name: name, body: "literal"}), dest); err != nil || count != 1 {
				t.Fatalf("literal extraction: count=%d, error=%v", count, err)
			}
			assert_file_contents(t, target, "literal")
			if count, err := ExtractAllFromTar(make_tar(t, tar_entry{name: "link", typeflag: tar.TypeLink, linkname: name}), dest); err != nil || count != 1 {
				t.Fatalf("literal hardlink: count=%d, error=%v", count, err)
			}
			assert_file_contents(t, filepath.Join(dest, "link"), "literal")
			assert_file_contents(t, victim, "unchanged")
		})
	}
}

func TestTarExtractJunctionDestination(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows directory junctions only")
	}
	parent := t.TempDir()
	real, junction := filepath.Join(parent, "real"), filepath.Join(parent, "junction")
	if err := os.Mkdir(real, 0700); err != nil {
		t.Fatal(err)
	}
	if out, err := exec.Command("cmd.exe", "/d", "/c", "mklink", "/J", junction, real).CombinedOutput(); err != nil {
		t.Fatalf("creating destination junction: %v: %s", err, out)
	}
	t.Cleanup(func() { _ = os.Remove(junction) })
	if _, err := ExtractAllFromTar(make_tar(t, tar_entry{name: "file", body: "data"}), junction); err != nil {
		t.Fatal(err)
	}
	assert_file_contents(t, filepath.Join(real, "file"), "data")
}

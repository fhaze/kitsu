package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCompleteFilesFromWindowsDriveRoot(t *testing.T) {
	cwd := t.TempDir()
	t.Chdir(t.TempDir())
	file := filepath.Join(cwd, "sessions", "work.kitty-session")
	if err := os.Mkdir(filepath.Dir(file), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(file, nil, 0600); err != nil {
		t.Fatal(err)
	}
	path := strings.TrimPrefix(file, filepath.VolumeName(file))
	volume := filepath.VolumeName(file)
	for _, candidate := range []string{path, filepath.ToSlash(path), "sessions/work.kitty-session", volume + "sessions/work.kitty-session"} {
		prefix := strings.TrimSuffix(candidate, "ork.kitty-session")
		var matches []FileEntry
		if err := CompleteFiles(prefix, func(e *FileEntry) { matches = append(matches, *e) }, cwd); err != nil {
			t.Fatalf("completing %q: %v", prefix, err)
		}
		if len(matches) != 1 || matches[0].CompletionCandidate != candidate || matches[0].Abspath != file {
			t.Errorf("completing %q: got %#v", prefix, matches)
		}
	}
	dir := volume + "sessions/"
	var matches []FileEntry
	if err := CompleteFiles(volume+"ses", func(e *FileEntry) { matches = append(matches, *e) }, cwd); err != nil {
		t.Fatal(err)
	}
	if len(matches) != 1 || matches[0].CompletionCandidate != dir || matches[0].Abspath != filepath.Dir(file) {
		t.Errorf("completing a drive-relative directory: got %+v", matches)
	}
}

func TestCompleteFilesWithWindowsDotPrefix(t *testing.T) {
	parent := t.TempDir()
	cwd := filepath.Join(parent, "child")
	if err := os.Mkdir(cwd, 0700); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{filepath.Join(parent, ".parent"), filepath.Join(cwd, ".child")} {
		if err := os.WriteFile(path, nil, 0600); err != nil {
			t.Fatal(err)
		}
	}
	for _, prefix := range []string{filepath.VolumeName(cwd) + ".", filepath.ToSlash(cwd) + "/."} {
		var matches []FileEntry
		if err := CompleteFiles(prefix, func(e *FileEntry) { matches = append(matches, *e) }, cwd); err != nil {
			t.Fatal(err)
		}
		if len(matches) != 1 || matches[0].CompletionCandidate != prefix+"child" || matches[0].Abspath != filepath.Join(cwd, ".child") {
			t.Errorf("completing %q: got %+v", prefix, matches)
		}
	}
}

func TestSplitUNCCompletionPrefix(t *testing.T) {
	for _, tc := range []struct{ prefix, sep string }{
		{"\\\\server\\share", "\\"},
		{"//server/share", "/"},
	} {
		directory, candidate := split_completion_prefix(tc.prefix, tc.sep)
		if directory != tc.prefix || candidate != tc.prefix+tc.sep {
			t.Errorf("completing %q: got directory %q, candidate prefix %q", tc.prefix, directory, candidate)
		}
	}
}

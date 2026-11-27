package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCompleteFilesFromWindowsDriveRoot(t *testing.T) {
	cwd := t.TempDir()
	file := filepath.Join(cwd, "sessions", "work.kitty-session")
	if err := os.Mkdir(filepath.Dir(file), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(file, nil, 0600); err != nil {
		t.Fatal(err)
	}
	path := strings.TrimPrefix(file, filepath.VolumeName(file))
	for _, candidate := range []string{path, filepath.ToSlash(path), "sessions/work.kitty-session"} {
		prefix := strings.TrimSuffix(candidate, "ork.kitty-session")
		var matches []FileEntry
		if err := CompleteFiles(prefix, func(e *FileEntry) { matches = append(matches, *e) }, cwd); err != nil {
			t.Fatalf("completing %q: %v", prefix, err)
		}
		if len(matches) != 1 || matches[0].CompletionCandidate != candidate || matches[0].Abspath != file {
			t.Errorf("completing %q: got %#v", prefix, matches)
		}
	}
}

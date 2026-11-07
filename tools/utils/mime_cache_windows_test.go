// License: GPLv3 Copyright: 2026, Kitsu contributors

package utils

import "testing"

func TestWindowsMimeFallback(t *testing.T) {
	// Even with XDG configuration present, use Windows' MIME database and
	// the existing builtins rather than loading a POSIX desktop's cache.
	t.Setenv("XDG_DATA_HOME", t.TempDir())
	if HaveMimeCache() {
		t.Fatal("Windows unexpectedly uses a system MIME cache")
	}
	for name, want := range map[string]string{"example.txt": "text/plain", "example.png": "image/png"} {
		if got := GuessMimeType(name); got != want {
			t.Errorf("GuessMimeType(%q) = %q, want %q", name, got, want)
		}
	}
}

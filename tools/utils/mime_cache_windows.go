// License: GPLv3 Copyright: 2026, Kitsu contributors

package utils

import (
	"os"
	"syscall"
)

// Explicit cache loads use load_mime_cache's portable read fallback. System
// MIME detection on Windows uses Go's registry-backed standard library.
func map_mime_cache(f *os.File, size int) ([]byte, error) { return nil, syscall.ENOSYS }

func unmap_mime_cache(data []byte) error { return nil }

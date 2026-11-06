// License: GPLv3 Copyright: 2026, Kitsu contributors

package dnd

import "os"

// Windows StatAt returns native os.FileInfo, whose file IDs os.SameFile can
// compare without reopening a potentially renamed file by its path.
func same_file(a, b os.FileInfo) (bool, error) { return os.SameFile(a, b), nil }

package choose_files

import (
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestResolveTypedPath(t *testing.T) {
	cwd := t.TempDir()
	vol := filepath.VolumeName(cwd)
	cases := map[string]string{
		"a.txt":     filepath.Join(cwd, "a.txt"),
		"sub/a.txt": filepath.Join(cwd, "sub", "a.txt"),
		"../a.txt":  filepath.Join(filepath.Dir(cwd), "a.txt"),
	}
	if runtime.GOOS == "windows" {
		cases[`\x\y`] = vol + `\x\y`
		cases[`/x/y`] = vol + `\x\y`
		cases[vol+`x`] = filepath.Join(cwd, "x")
		cases[vol+`\x`] = vol + `\x`
		cases[`\\srv\share\x`] = `\\srv\share\x`
	} else {
		cases["/x/y"] = "/x/y"
	}
	for typed, expected := range cases {
		if actual := resolve_typed_path(typed, cwd); actual != expected {
			t.Errorf("%#v resolved to %#v instead of %#v", typed, actual, expected)
		}
	}
}

func TestSaveFileCompletion(t *testing.T) {
	cwd := t.TempDir()
	for _, p := range []string{"sub/File.txt", "sub/other.txt", "top.txt"} {
		p = filepath.Join(cwd, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	complete := FilePromptCompleter(func() string { return cwd })
	words := func(typed string) (ans []string) {
		c := complete(typed, "")
		if c == nil {
			return nil
		}
		for _, g := range c.Groups {
			for _, m := range g.Matches {
				ans = append(ans, m.Word)
			}
		}
		slices.Sort(ans)
		return
	}
	sep := string(os.PathSeparator)
	cases := map[string][]string{
		"t":               {"top.txt"},
		"sub" + sep:       {"sub" + sep + "File.txt", "sub" + sep + "other.txt"},
		"sub" + sep + "F": {"sub" + sep + "File.txt"},
	}
	if runtime.GOOS == "windows" {
		// what was typed is kept, and names match regardless of case
		cases["sub/f"] = []string{"sub/File.txt"}
		cases["sub/"] = []string{"sub/File.txt", "sub/other.txt"}
		cases[`SUB\o`] = []string{`SUB\other.txt`}
	}
	for typed, expected := range cases {
		if diff := cmp.Diff(expected, words(typed)); diff != "" {
			t.Errorf("completing %#v:\n%s", typed, diff)
		}
	}
}

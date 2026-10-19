package choose_files

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/kovidgoyal/go-parallel"
	"github.com/kovidgoyal/kitty/tools/cli"
	"github.com/kovidgoyal/kitty/tools/tui/loop"
	"github.com/kovidgoyal/kitty/tools/utils"
)

var _ = fmt.Print

func FilePromptCompleter(getcwd func() string) func(string, string) *cli.Completions {
	if getcwd == nil {
		getcwd = func() string {
			ans, err := os.Getwd()
			if err != nil {
				ans = "."
			}
			return ans
		}
	}
	return func(before_cursor, after_cursor string) (ans *cli.Completions) {
		defer func() {
			if r := recover(); r != nil {
				err := parallel.Format_stacktrace_on_panic(r, 1)
				debugprintln(err)
			}
		}()
		ans = cli.NewCompletions()
		// The completions replace the typed text, so keep it as typed up to the last separator
		prefix := filepath.VolumeName(before_cursor)
		if idx := last_separator_index(before_cursor); idx > -1 {
			prefix = before_cursor[:idx+1]
		}
		path, dir := "", ""
		if before_cursor == "" {
			path = getcwd()
			dir = path
		} else {
			path = resolve_typed_path(before_cursor, getcwd())
			dir = filepath.Dir(path)
			if idx := last_separator_index(before_cursor); idx > -1 && idx == len(before_cursor)-1 {
				dir = path
			}
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			return nil
		}
		dirs := ans.AddMatchGroup("Directories")
		dirs.IsFiles = true
		dirs.NoTrailingSpace = true
		files := ans.AddMatchGroup("Files")
		files.IsFiles = true
		files.NoTrailingSpace = true
		leading, _ := filepath.Rel(dir, path)
		if leading == "." {
			leading = ""
		}
		for _, e := range entries {
			word := e.Name()
			if leading == "" || has_name_prefix(word, leading) {
				collection := utils.IfElse(e.Type().IsDir(), dirs, files)
				if prefix != "" {
					word = prefix + word
				}
				collection.Matches = append(collection.Matches, &cli.Match{Word: word})
			}
		}
		return ans
	}
}

func (h *Handler) current_save_file_path() string {
	ans := h.rl.AllText()
	if ans != "" {
		ans = resolve_typed_path(ans, h.state.CurrentDir())
	}
	return ans
}

// Windows accepts both separators
func last_separator_index(path string) int {
	if runtime.GOOS == "windows" {
		return strings.LastIndexAny(path, `\/`)
	}
	return strings.LastIndexByte(path, '/')
}

// Windows file names are case insensitive
func has_name_prefix(name, prefix string) bool {
	if runtime.GOOS == "windows" {
		return len(name) >= len(prefix) && strings.EqualFold(name[:len(prefix)], prefix)
	}
	return strings.HasPrefix(name, prefix)
}

// The absolute path for a path the user typed, relative to cwd
func resolve_typed_path(typed, cwd string) string {
	p := utils.Expanduser(typed)
	if filepath.IsAbs(p) {
		return filepath.Clean(p)
	}
	if runtime.GOOS == "windows" {
		if vol := filepath.VolumeName(p); vol != "" {
			// C:x is relative to the current directory of drive C:
			if strings.EqualFold(vol, filepath.VolumeName(cwd)) {
				return filepath.Join(cwd, p[len(vol):])
			}
			if abs, err := filepath.Abs(p); err == nil {
				return abs
			}
		} else if p != "" && os.IsPathSeparator(p[0]) {
			// \x is relative to the root of the current drive
			return filepath.VolumeName(cwd) + filepath.Clean(p)
		}
	}
	return filepath.Join(cwd, p)
}

func (h *Handler) save_file_name_handle_key(ev *loop.KeyEvent) (err error) {
	ac := h.shortcut_tracker.Match(ev, h.state.keyboard_shortcuts)
	if ac != nil {
		switch ac.Name {
		case "accept":
			if p := h.current_save_file_path(); p != "" {
				h.state.selections = append(h.state.selections, p)
				h.lp.Quit(0)
			} else {
				h.lp.Beep()
			}
			ev.Handled = true
			return nil
		case "select":
			if !h.state.mode.AllowsMultipleSelection() {
				// otherwise accept would add a second name when saving a single file
				h.lp.Beep()
				ev.Handled = true
				return nil
			}
			if p := h.current_save_file_path(); p != "" {
				h.state.selections = append(h.state.selections, p)
			}
			ev.Handled = true
			h.rl.ResetText()
			return h.draw_screen()
		case "quit":
			h.state.screen = NORMAL
			ev.Handled = true
			return h.draw_screen()
		}
	}
	if err = h.rl.OnKeyEvent(ev); err == nil {
		err = h.draw_screen()
	}
	return
}

func (h *Handler) initialize_save_file_name(fname string) {
	h.state.screen = SAVE_FILE
	h.rl.ResetText()
	if len(h.state.selections) > 0 && fname == "" {
		if q, err := filepath.Abs(h.state.selections[0]); err == nil {
			if s, err := os.Stat(q); err == nil {
				if s.IsDir() == h.state.mode.OnlyDirs() {
					if fname, err = filepath.Rel(h.state.CurrentDir(), q); err != nil {
						fname = q
					}
				}
			}
		}
	}
	h.rl.SetText(fname)
}

func (h *Handler) draw_save_file_name_screen() (err error) {
	h.lp.AllowLineWrapping(true)
	desc := utils.IfElse(h.state.mode == SELECT_SAVE_FILE, "file", "directory")
	if h.state.DisplayTitle() {
		h.lp.Println(h.state.WindowTitle())
	}
	h.lp.Println("Enter the name of the", desc, "below, relative to:")
	h.lp.Println(h.lp.SprintStyled("fg=green", h.state.CurrentDir()))
	if h.state.mode.AllowsMultipleSelection() {
		h.lp.Println("Use shift+enter (or whatever you mapped the select action to) to enter multiple filenames")
	}
	h.lp.Println()
	h.rl.RedrawNonAtomic()
	h.lp.AllowLineWrapping(false)
	if len(h.state.selections) > 0 {
		h.lp.SaveCursorPosition()
		h.draw_footer()
		h.lp.RestoreCursorPosition()
	}
	return
}

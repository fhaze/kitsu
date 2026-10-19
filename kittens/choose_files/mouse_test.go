package choose_files

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/kovidgoyal/kitty/tools/tui"
	"github.com/kovidgoyal/kitty/tools/tui/loop"
	"github.com/kovidgoyal/kitty/tools/tui/readline"
	"golang.org/x/text/language"
	"golang.org/x/text/message"
)

const test_cell_width, test_cell_height = 10, 20

func new_test_handler(t *testing.T, width, height int) *Handler {
	t.Helper()
	dir := t.TempDir()
	if cwd, err := os.Getwd(); err == nil {
		// set_root_dir() changes into the chosen directory, which Windows then cannot delete
		t.Cleanup(func() { _ = os.Chdir(cwd) })
	}
	for _, name := range []string{"a.txt", "b.txt", "c.txt", ".hidden"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(name), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	lp, err := loop.New()
	if err != nil {
		t.Fatal(err)
	}
	h := &Handler{lp: lp, err_chan: make(chan error, 8), msg_printer: message.NewPrinter(language.English), spinner: tui.NewSpinner("dots")}
	h.rl = readline.New(lp, readline.RlInit{Prompt: "> "})
	h.search_rl = readline.New(lp, readline.RlInit{DontMarkPrompts: true})
	if err = h.set_state_from_config(NewConfig(), &Options{}); err != nil {
		t.Fatal(err)
	}
	h.state.show_preview = false
	h.state.base_dir = dir
	h.state.mouse_state.ApplyHoverStyles(lp, HOVER_STYLE) // avoid reading kitty.conf in tests
	wakeups := make(chan bool, 1024)
	wakeup := func() bool { wakeups <- true; return true }
	h.result_manager = NewResultManager(h.err_chan, &h.state, wakeup)
	h.preview_manager = NewPreviewManager(h.err_chan, &h.state, wakeup)
	h.init_sizes(loop.ScreenSize{WidthCells: uint(width), HeightCells: uint(height), CellWidth: test_cell_width, CellHeight: test_cell_height,
		WidthPx: uint(width * test_cell_width), HeightPx: uint(height * test_cell_height)})
	h.result_manager.set_root_dir()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, complete := h.get_results(); complete {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("timed out waiting for scan")
		}
		time.Sleep(5 * time.Millisecond)
	}
	if err = h.draw_screen(); err != nil {
		t.Fatal(err)
	}
	return h
}

func mouse_event(etype loop.MouseEventType, x, y int) *loop.MouseEvent {
	ev := &loop.MouseEvent{Event_type: etype}
	if etype != loop.MOUSE_MOVE {
		ev.Buttons = loop.LEFT_MOUSE_BUTTON
	}
	ev.Cell.X, ev.Cell.Y = x, y
	ev.Pixel.X, ev.Pixel.Y = x*test_cell_width+test_cell_width/2, y*test_cell_height+test_cell_height/2
	return ev
}

func click(t *testing.T, h *Handler, x, y int) {
	t.Helper()
	for _, ev := range []*loop.MouseEvent{mouse_event(loop.MOUSE_MOVE, x, y), mouse_event(loop.MOUSE_PRESS, x, y), mouse_event(loop.MOUSE_RELEASE, x, y), mouse_event(loop.MOUSE_CLICK, x, y)} {
		if err := h.OnMouseEvent(ev); err != nil {
			t.Fatal(err)
		}
	}
}

func TestChooseFilesControlsClickable(t *testing.T) {
	h := new_test_handler(t, 100, 30)
	// "show dotfiles" is the first control, at the right edge of the first row
	click(t, h, h.screen_size.width-3, 0)
	if !h.state.show_hidden {
		t.Fatal("clicking show dotfiles did not toggle show_hidden")
	}
}

func TestChooseFilesHoverDoesNotRedrawOnEveryMove(t *testing.T) {
	h := new_test_handler(t, 100, 30)
	for _, pos := range [][2]int{{h.screen_size.width - 3, 0}, {5, 1}} {
		if err := h.OnMouseEvent(mouse_event(loop.MOUSE_MOVE, pos[0], pos[1])); err != nil {
			t.Fatal(err)
		}
		for i := range 3 {
			if changed := h.state.mouse_state.UpdateState(mouse_event(loop.MOUSE_MOVE, pos[0], pos[1])); changed {
				t.Fatalf("hovered regions changed on repeated move %d at %v with no actual change", i, pos)
			}
		}
	}
}

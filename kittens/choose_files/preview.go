package choose_files

import (
	"bytes"
	"cmp"
	"fmt"
	"io"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"unicode/utf8"

	"github.com/kovidgoyal/go-parallel"
	"github.com/kovidgoyal/kitty/tools/highlight"
	"github.com/kovidgoyal/kitty/tools/icons"
	"github.com/kovidgoyal/kitty/tools/tui/loop"
	"github.com/kovidgoyal/kitty/tools/utils"
	"github.com/kovidgoyal/kitty/tools/utils/humanize"
	"github.com/kovidgoyal/kitty/tools/utils/style"
	"github.com/kovidgoyal/kitty/tools/wcswidth"
)

var _ = fmt.Print

type Preview interface {
	Render(h *Handler, x, y, width, height int)
	IsValidForColorScheme(light bool) bool
	Unload()
	IsReady() bool
	String() string
}

type PreviewManager struct {
	report_errors    chan error
	settings         Settings
	WakeupMainThread func() bool
	cache            map[string]*cached_preview
	use_counter      uint64
	lock             sync.Mutex
	highlighter      highlight.Highlighter
}

type cached_preview struct {
	p         Preview
	last_used uint64
}

// Previews can hold the whole text of a file, or a decoded image, so only keep
// the most recently used ones
const max_cached_previews = 256
const max_cached_preview_bytes = 256 * 1024 * 1024

// Implemented by previews that can use a lot of memory
type memory_user interface {
	memory_cost() int
}

func preview_memory_cost(p Preview) int {
	if m, ok := p.(memory_user); ok {
		return m.memory_cost()
	}
	return 1024
}

func NewPreviewManager(err_chan chan error, settings Settings, WakeupMainThread func() bool) (ans *PreviewManager) {
	defer func() { sanitize = ans.highlighter.Sanitize }()
	return &PreviewManager{
		report_errors: err_chan, settings: settings, WakeupMainThread: WakeupMainThread,
		cache: make(map[string]*cached_preview), highlighter: highlight.NewHighlighter(nil),
	}
}

func (pm *PreviewManager) cached_preview(path string) Preview {
	pm.lock.Lock()
	defer pm.lock.Unlock()
	if e := pm.cache[path]; e != nil {
		pm.use_counter++
		e.last_used = pm.use_counter
		return e.p
	}
	return nil
}

func (pm *PreviewManager) set_cached_preview(path string, val Preview) {
	pm.lock.Lock()
	defer pm.lock.Unlock()
	pm.use_counter++
	pm.cache[path] = &cached_preview{val, pm.use_counter}
	total := 0
	for _, e := range pm.cache {
		total += preview_memory_cost(e.p)
	}
	if len(pm.cache) <= max_cached_previews && total <= max_cached_preview_bytes {
		return
	}
	paths := slices.Collect(maps.Keys(pm.cache))
	slices.SortFunc(paths, func(a, b string) int { return cmp.Compare(pm.cache[a].last_used, pm.cache[b].last_used) })
	for _, q := range paths[:len(paths)-1] { // never the one just added
		if len(pm.cache) <= max_cached_previews && total <= max_cached_preview_bytes {
			break
		}
		total -= preview_memory_cost(pm.cache[q].p)
		delete(pm.cache, q)
		pm.highlighter.Forget(q)
	}
}

// A preview that is slow to make, such as one that reads a large file, or a
// directory on a network drive, made off the UI thread
type AsyncPreview struct {
	placeholder     Preview
	result          chan Preview
	p               Preview
	color_dependent bool
	light           bool
}

func NewAsyncPreview(placeholder Preview, color_dependent bool, WakeupMainThread func() bool, create func() Preview) *AsyncPreview {
	ans := &AsyncPreview{placeholder: placeholder, result: make(chan Preview, 1), color_dependent: color_dependent, light: use_light_colors}
	go func() {
		defer func() {
			if r := recover(); r != nil {
				ans.result <- NewErrorPreview(parallel.Format_stacktrace_on_panic(r, 1))
			}
			WakeupMainThread()
		}()
		ans.result <- create()
	}()
	return ans
}

// Only for the UI thread
func (p *AsyncPreview) get() Preview {
	if p.p == nil {
		select {
		case p.p = <-p.result:
		default:
		}
	}
	return p.p
}

func (p *AsyncPreview) String() string { return fmt.Sprintf("AsyncPreview{%s}", p.get()) }
func (p *AsyncPreview) IsReady() bool  { q := p.get(); return q != nil && q.IsReady() }
func (p *AsyncPreview) IsValidForColorScheme(light bool) bool {
	if q := p.get(); q != nil {
		return q.IsValidForColorScheme(light)
	}
	return !p.color_dependent || p.light == light
}
func (p *AsyncPreview) Unload() {
	if q := p.get(); q != nil {
		q.Unload()
	}
}
func (p *AsyncPreview) Render(h *Handler, x, y, width, height int) {
	if q := p.get(); q != nil {
		q.Render(h, x, y, width, height)
	} else {
		p.placeholder.Render(h, x, y, width, height)
	}
}
func (p *AsyncPreview) memory_cost() int {
	if q := p.get(); q != nil {
		return preview_memory_cost(q)
	}
	return 0
}

func loading_preview(abspath string, metadata fs.FileInfo) Preview {
	title := icons.IconForFileWithMode(filepath.Base(abspath), metadata.Mode().Type(), false) + "  " + filepath.Base(abspath)
	return &MessagePreview{title: title, msg: "Loading…"}
}

func (h *Handler) render_wrapped_text_in_region(text string, x, y, width, height int, centered bool) int {
	lines := style.WrapTextAsLines(text, width, style.WrapOptions{})
	for i, line := range lines {
		if i >= height { // before writing, as the next line is the frame
			break
		}
		extra := 0
		if centered {
			extra = max(0, width-wcswidth.Stringwidth(line)) / 2
		}
		h.lp.MoveCursorTo(x+extra, y+i)
		h.lp.QueueWriteString(line)
	}
	return len(lines)
}

type MessagePreview struct {
	title    string
	msg      string
	trailers []string
}

func (p MessagePreview) String() string                  { return fmt.Sprintf("MessagePreview{%#v}", p.title) }
func (p MessagePreview) IsValidForColorScheme(bool) bool { return true }
func (p MessagePreview) IsReady() bool                   { return true }

func (p MessagePreview) Unload() {}
func (p MessagePreview) Render(h *Handler, x, y, width, height int) {
	offset := 0
	if p.title != "" {
		offset += h.render_wrapped_text_in_region(p.title, x, y, width, height, true)
	}
	offset += h.render_wrapped_text_in_region(p.msg, x, y+offset, width, height-offset, false)
	limit := height - offset
	if limit > 1 {
		for i, line := range p.trailers {
			text := wcswidth.TruncateToVisualLength(line, width-1)
			if len(text) < len(line) {
				text += "…"
			}
			h.lp.MoveCursorTo(x, y+offset+i-1)
			if i >= limit {
				h.lp.QueueWriteString("…")
				break
			}
			h.lp.QueueWriteString(text)
		}
	}
}

func NewErrorPreview(err error) Preview {
	sctx := style.Context{AllowEscapeCodes: true}
	text := fmt.Sprintf("%s: %s", sctx.SprintFunc("fg=red")("Error"), err)
	return &MessagePreview{msg: text}
}

var sanitize func(string) string

func write_file_metadata(abspath string, metadata fs.FileInfo, entries []fs.DirEntry) (header string, trailers []string) {
	buf := strings.Builder{}
	buf.Grow(4096)
	add := func(key, val string) { fmt.Fprintf(&buf, "%s: %s\n", key, val) }
	ftype := metadata.Mode().Type()
	const file_icon = " "
	fmt.Fprintln(&buf, filepath.Base(abspath))
	switch ftype {
	case 0:
		add("Size", humanize.Bytes(uint64(metadata.Size())))
	case fs.ModeSymlink:
		if tgt, err := os.Readlink(abspath); err == nil {
			add("Target", sanitize(tgt))
		} else {
			add("Target", err.Error())
		}
	case fs.ModeDir:
		num_files, num_dirs := 0, 0
		for _, e := range entries {
			if e.IsDir() {
				num_dirs++
			} else {
				num_files++
			}
		}
		add("Children", fmt.Sprintf("%d %s  %d %s", num_dirs, icons.IconForFileWithMode("dir", fs.ModeDir, false), num_files, file_icon))
	}
	add("Modified", humanize.Time(metadata.ModTime()))
	add("Mode", metadata.Mode().String())
	if len(entries) > 0 {
		type entry struct {
			lname string
			ftype fs.FileMode
		}
		type_map := make(map[string]entry, len(entries))
		for _, e := range entries {
			type_map[e.Name()] = entry{strings.ToLower(e.Name()), e.Type()}
		}
		names := utils.Map(func(e fs.DirEntry) string { return e.Name() }, entries)
		slices.SortFunc(names, func(a, b string) int { return strings.Compare(type_map[a].lname, type_map[b].lname) })
		fmt.Fprintln(&buf, "Contents:")
		for _, n := range names {
			trailers = append(trailers, icons.IconForFileWithMode(n, type_map[n].ftype, false)+"  "+sanitize(n))
		}
	}
	return buf.String(), trailers
}

func NewDirectoryPreview(abspath string, metadata fs.FileInfo) Preview {
	entries, err := os.ReadDir(abspath)
	if err != nil {
		return NewErrorPreview(fmt.Errorf("failed to read the directory %s with error: %w", abspath, err))
	}
	title := icons.IconForFileWithMode("dir", fs.ModeDir, false) + "  Directory\n"
	header, extra := write_file_metadata(abspath, metadata, entries)
	return &MessagePreview{title: title, msg: header, trailers: extra}
}

func NewFileMetadataPreview(abspath string, metadata fs.FileInfo) *MessagePreview {
	ext := filepath.Ext(abspath)
	if ext == "" {
		ext = "File"
	}
	title := icons.IconForFileWithMode(filepath.Base(abspath), metadata.Mode().Type(), false) + "  " + ext
	h, t := write_file_metadata(abspath, metadata, nil)
	return &MessagePreview{title: title, msg: h, trailers: t}
}

func NewFileMetadataPreviewWithError(abspath string, metadata fs.FileInfo, err error) *MessagePreview {
	ext := filepath.Ext(abspath)
	if ext == "" {
		ext = "File"
	}
	title := icons.IconForFileWithMode(filepath.Base(abspath), metadata.Mode().Type(), false) + "  " + ext
	h, t := write_file_metadata(abspath, metadata, nil)
	ans := &MessagePreview{title: title, msg: h, trailers: t}
	lines := style.WrapTextAsLines(err.Error(), 30, style.WrapOptions{})
	ans.trailers = append(ans.trailers, "")
	ans.trailers = append(ans.trailers, lines...)
	return ans
}

type highlighed_data struct {
	text  string
	light bool
	err   error
}

type TextFilePreview struct {
	plain_text, highlighted_text string
	highlighted_chan             chan highlighed_data
	ready                        atomic.Bool
	light                        bool
	path                         string
	metadata                     fs.FileInfo
}

func (p *TextFilePreview) String() string {
	return fmt.Sprintf("TextFilePreview{%#v, ready: %v}", p.path, p.ready.Load())
}

func (p *TextFilePreview) IsValidForColorScheme(light bool) bool { return p.light == light }

func (p *TextFilePreview) Unload() {}

func (p *TextFilePreview) IsReady() bool { return p.ready.Load() || p.highlighted_chan == nil }
func (p *TextFilePreview) Render(h *Handler, x, y, width, height int) {
	if p.highlighted_chan != nil {
		select {
		case hd := <-p.highlighted_chan:
			p.highlighted_chan = nil
			if hd.err == nil {
				p.highlighted_text = hd.text
			}
		default:
		}
	}
	text := p.highlighted_text
	if text == "" {
		text = p.plain_text
	}
	s := utils.NewLineScanner(text)
	buf := strings.Builder{}
	buf.Grow(1024 * height)
	title := icons.IconForPath(p.path) + "  " + filepath.Base(p.path) + fmt.Sprintf(" %s", humanize.Bytes(uint64(p.metadata.Size())))
	for num := 1 + h.render_wrapped_text_in_region(title, x, y, width, height, true); s.Scan() && num < height; num++ {
		line := s.Text()
		truncated := wcswidth.TruncateToVisualLength(line, width)
		buf.WriteString(fmt.Sprintf(loop.MoveCursorToTemplate, y+num, x))
		buf.WriteString(truncated)
		if len(truncated) < len(line) {
			wcswidth.KeepOnlyCSI(line[len(truncated):], &buf)
		}
	}
	buf.WriteString("\x1b[m") // reset any highlight styles
	h.lp.QueueWriteString(buf.String())
}

func (p *TextFilePreview) memory_cost() int { return len(p.plain_text) + len(p.highlighted_text) }

func (p MessagePreview) memory_cost() (ans int) {
	ans = len(p.title) + len(p.msg)
	for _, t := range p.trailers {
		ans += len(t)
	}
	return
}

type style_resolver struct {
	light                   bool
	light_style, dark_style string
	syntax_aliases          map[string]string
	text                    string // already read, if not empty
}

func (s style_resolver) StyleName() string {
	return utils.IfElse(s.light, s.light_style, s.dark_style)
}
func (s style_resolver) UseLightColors() bool             { return s.light }
func (s style_resolver) SyntaxAliases() map[string]string { return s.syntax_aliases }
func (s style_resolver) TextForPath(path string) (string, error) {
	if s.text != "" {
		return s.text, nil
	}
	ans, err := os.ReadFile(path)
	if err == nil {
		return utils.UnsafeBytesToString(ans), nil
	}
	return "", err
}

func (pm *PreviewManager) highlight_file_async(path string, s style_resolver, output chan highlighed_data, ready *atomic.Bool) {
	go func() {
		defer func() {
			if r := recover(); r != nil {
				err := parallel.Format_stacktrace_on_panic(r, 1)
				output <- highlighed_data{err: err, light: s.light}
			}
			close(output)
			ready.Store(true)
			pm.WakeupMainThread()
		}()
		highlighted, err := pm.highlighter.HighlightFile(path, &s)
		output <- highlighed_data{text: highlighted, err: err, light: s.light}
	}()
}

func (pm *PreviewManager) invalidate_color_scheme_based_cached_items() {
	pm.lock.Lock()
	defer pm.lock.Unlock()
	maps.DeleteFunc(pm.cache, func(key string, e *cached_preview) bool { return !e.p.IsValidForColorScheme(use_light_colors) })
}

func (pm *PreviewManager) preview_for(abspath string, ftype fs.FileMode) (ans Preview) {
	if ans = pm.cached_preview(abspath); ans != nil {
		return ans
	}
	defer func() { pm.set_cached_preview(abspath, ans) }()
	s, err := os.Lstat(abspath)
	if err != nil {
		return NewErrorPreview(err)
	}
	if s.IsDir() {
		return NewAsyncPreview(loading_preview(abspath, s), false, pm.WakeupMainThread, func() Preview { return NewDirectoryPreview(abspath, s) })
	}
	if ftype&fs.ModeSymlink != 0 && ftype&SymlinkToDir != 0 {
		s, err = os.Stat(abspath)
		if err != nil {
			return NewErrorPreview(err)
		}
		return NewAsyncPreview(loading_preview(abspath, s), false, pm.WakeupMainThread, func() Preview { return NewDirectoryPreview(abspath, s) })
	}
	fname := filepath.Base(abspath)
	mt := utils.GuessMimeType(fname)
	for _, q := range previewers {
		if q.matches(fname, mt) {
			return NewCmdPreview(abspath, s, pm.settings, pm.WakeupMainThread, q)
		}
	}
	const MAX_TEXT_FILE_SIZE = 16 * 1024 * 1024
	could_be_text := s.Size() <= MAX_TEXT_FILE_SIZE
	if could_be_text && (utils.KnownTextualMimes[mt] || strings.HasPrefix(mt, "text/") || (ambiguous_mimes[mt] && looks_like_text(abspath))) {
		return pm.text_preview(abspath, s)
	}
	switch {
	case strings.HasPrefix(mt, "image/"):
		var r ImagePreviewRenderer
		if ans, err := NewImagePreview(abspath, s, pm.settings, pm.WakeupMainThread, r); err == nil {
			return ans
		} else {
			return NewErrorPreview(err)
		}

	case strings.HasPrefix(mt, "video/"):
		return NewFFMpegPreview(abspath, s, pm.settings, pm.WakeupMainThread)

	case IsSupportedArchiveFile(abspath):
		return NewArchivePeview(abspath, s, pm.settings, pm.WakeupMainThread)

	case IsSupportedByCalibre(abspath):
		return NewCalibrePreview(abspath, s, pm.settings, pm.WakeupMainThread)
	}
	if could_be_text && looks_like_text(abspath) {
		// Extensions with no known type, such as .go or .log on Windows, which has
		// no shared MIME database
		return pm.text_preview(abspath, s)
	}
	return NewFileMetadataPreview(abspath, s)
}

// Types that are also used for common text formats, .ts is both an MPEG
// transport stream and TypeScript
var ambiguous_mimes = map[string]bool{"video/mp2t": true, "video/vnd.dlna.mpeg-tts": true}

// Whether the start of the file looks like UTF-8 text, rather than binary data
func looks_like_text(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	buf := make([]byte, 4096)
	n, _ := io.ReadFull(f, buf)
	data := buf[:n]
	if n == 0 || bytes.IndexByte(data, 0) > -1 {
		return false
	}
	controls := 0
	for _, b := range data {
		if b < 0x20 && b != '\t' && b != '\n' && b != '\r' && b != '\f' && b != '\v' && b != 0x1b {
			controls++
		}
	}
	if controls > n/32 {
		return false
	}
	if n == len(buf) { // the read may have split a multi-byte character
		for i := 0; i < utf8.UTFMax-1 && len(data) > 0 && !utf8.Valid(data); i++ {
			data = data[:len(data)-1]
		}
	}
	return utf8.Valid(data)
}

func (pm *PreviewManager) text_preview(abspath string, metadata fs.FileInfo) Preview {
	// The settings and color scheme are read here, on the UI thread
	sr := style_resolver{light: use_light_colors, syntax_aliases: pm.settings.SyntaxAliases()}
	sr.light_style, sr.dark_style = pm.settings.HighlightStyles()
	return NewAsyncPreview(loading_preview(abspath, metadata), true, pm.WakeupMainThread, func() Preview {
		data, err := os.ReadFile(abspath)
		if err != nil {
			return NewFileMetadataPreview(abspath, metadata)
		}
		text := utils.UnsafeBytesToString(data)
		ans := &TextFilePreview{path: abspath, light: sr.light, metadata: metadata}
		if !utf8.ValidString(text) {
			ans.plain_text = "Error: not valid utf-8 text" // and not highlighted, which would show it as garbage
			return ans
		}
		ans.plain_text = pm.highlighter.Sanitize(strings.ReplaceAll(text, "\r\n", "\n")) // otherwise sanitize() shows each \r as a symbol
		ans.highlighted_chan = make(chan highlighed_data, 2)
		sr.text = text // so that it is not read again
		pm.highlight_file_async(abspath, sr, ans.highlighted_chan, &ans.ready)
		return ans
	})
}

func (h *Handler) clear_cached_previews() {
	if h.last_rendered_preview != nil {
		h.last_rendered_preview.Unload()
		h.last_rendered_preview = nil
	}
	if h.prev_preview_for_smooth_transition != nil {
		h.prev_preview_for_smooth_transition.Unload()
		h.prev_preview_for_smooth_transition = nil
	}
}

func (h *Handler) draw_preview_content(x, y, width, height int) {
	matches, _ := h.get_results()
	r := matches.At(h.state.CurrentIndex())
	if r == nil {
		h.render_wrapped_text_in_region("No preview available", x, y, width, height, false)
		return
	}
	render := func() {
		p := h.last_rendered_preview
		if p.IsReady() || h.prev_preview_for_smooth_transition == nil {
			p.Render(h, x, y, width, height)
			if h.prev_preview_for_smooth_transition != nil {
				h.prev_preview_for_smooth_transition.Unload()
				h.prev_preview_for_smooth_transition = nil
			}
		} else {
			h.prev_preview_for_smooth_transition.Render(h, x, y, width, height)
		}
	}

	abspath := h.current_abspath()
	if h.last_rendered_preview != nil {
		if abspath == h.last_rendered_preview_abspath {
			render()
			return
		}
		if h.prev_preview_for_smooth_transition != nil {
			h.prev_preview_for_smooth_transition.Unload()
		}
		h.prev_preview_for_smooth_transition = h.last_rendered_preview
		h.last_rendered_preview = nil
	}
	if p := h.preview_manager.preview_for(abspath, r.ftype); p == nil {
		h.render_wrapped_text_in_region("No preview available", x, y, width, height, false)
	} else {
		h.last_rendered_preview = p
		h.last_rendered_preview_abspath = abspath
		render()
	}
}

package choose_files

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLooksLikeText(t *testing.T) {
	dir := t.TempDir()
	// a multi-byte character split by the 4096 byte read
	split := strings.Repeat("a", 4095) + "é" + "b"
	mpeg_ts := bytes.Repeat(append([]byte{0x47, 0x40, 0x11, 0x10, 0x00}, make([]byte, 183)...), 4)
	for name, tc := range map[string]struct {
		data     []byte
		expected bool
	}{
		"main.go":     {[]byte("package main\r\n\nfunc main() {}\n"), true},
		"ansi.log":    {[]byte("\x1b[31merror\x1b[m\tdetails\f\n"), true},
		"split.txt":   {[]byte(split), true},
		"empty":       {nil, false},
		"video.ts":    {mpeg_ts, false},
		"utf16.txt":   {[]byte{0xff, 0xfe, 'a', 0, 'b', 0}, false},
		"latin1.txt":  {[]byte("caf\xe9 cr\xe8me"), false},
		"control.bin": {bytes.Repeat([]byte{1, 2, 3, 'a'}, 64), false},
	} {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, tc.data, 0o600); err != nil {
			t.Fatal(err)
		}
		if actual := looks_like_text(p); actual != tc.expected {
			t.Errorf("looks_like_text(%s) = %v", name, actual)
		}
	}
}

func wait_for_async_preview(t *testing.T, p Preview) Preview {
	t.Helper()
	a, ok := p.(*AsyncPreview)
	if !ok {
		t.Fatalf("not made off the UI thread: %s", p)
	}
	for deadline := time.Now().Add(5 * time.Second); a.get() == nil; time.Sleep(time.Millisecond) {
		if time.Now().After(deadline) {
			t.Fatal("timed out waiting for the preview")
		}
	}
	return a.get()
}

func TestPreviewOfUnregisteredTextFiles(t *testing.T) {
	dir := t.TempDir()
	h := &Handler{}
	h.state.base_dir = dir
	pm := NewPreviewManager(make(chan error, 8), &h.state, func() bool { return true })
	for name, data := range map[string]string{
		"main.go":     "package main\r\n",
		"script.ts":   "const x: number = 1\n",
		"no-ext-file": "plain text\n",
	} {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(data), 0o600); err != nil {
			t.Fatal(err)
		}
		tp, ok := wait_for_async_preview(t, pm.preview_for(p, 0)).(*TextFilePreview)
		if !ok {
			t.Fatalf("%s did not get a text preview", name)
		}
		if strings.Contains(tp.plain_text, "\r") || strings.Contains(tp.plain_text, "␍") {
			t.Fatalf("%s: line endings were not normalized: %q", name, tp.plain_text)
		}
	}
	p := filepath.Join(dir, "data.bin")
	if err := os.WriteFile(p, []byte{0, 1, 2, 3}, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, ok := pm.preview_for(p, 0).(*MessagePreview); !ok {
		t.Fatal("binary file did not get a metadata preview")
	}
	d := filepath.Join(dir, "sub")
	if err := os.Mkdir(d, 0o700); err != nil {
		t.Fatal(err)
	}
	if _, ok := wait_for_async_preview(t, pm.preview_for(d, 0)).(*MessagePreview); !ok {
		t.Fatal("directory did not get a directory preview")
	}
}

type costly_preview struct {
	MessagePreview
	cost int
}

func (p *costly_preview) memory_cost() int { return p.cost }

func TestPreviewCacheEviction(t *testing.T) {
	h := &Handler{}
	pm := NewPreviewManager(make(chan error, 8), &h.state, func() bool { return true })
	key := func(i int) string { return fmt.Sprintf("file-%d", i) }
	for i := range max_cached_previews + 50 {
		pm.set_cached_preview(key(i), &costly_preview{cost: 1})
		if i >= max_cached_previews-1 {
			pm.cached_preview(key(0)) // kept in use, so never dropped
		}
	}
	if len(pm.cache) != max_cached_previews {
		t.Fatalf("the cache has %d previews", len(pm.cache))
	}
	if pm.cached_preview(key(0)) == nil || pm.cached_preview(key(1)) != nil || pm.cached_preview(key(max_cached_previews+49)) == nil {
		t.Fatal("the least recently used previews were not the ones dropped")
	}
	// one preview larger than the budget evicts everything else, but stays
	pm.set_cached_preview("huge", &costly_preview{cost: max_cached_preview_bytes + 1})
	if len(pm.cache) != 1 || pm.cached_preview("huge") == nil {
		t.Fatalf("the cache has %d previews after a huge one", len(pm.cache))
	}
}

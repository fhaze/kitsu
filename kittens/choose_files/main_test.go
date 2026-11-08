// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package choose_files

import (
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
	"github.com/kovidgoyal/kitty/tools/utils/shlex"
)

func TestVideoPreviewValidation(t *testing.T) {
	duration, fps, width := video_duration, video_fps, video_width
	t.Cleanup(func() { video_duration, video_fps, video_width = duration, fps, width })
	for _, spec := range []string{"width=480 fps=10 duration=5", "width=1 fps=1 duration=0.5", "width=4096 fps=60 duration=10", ""} {
		if err := parse_video_preview(spec); err != nil {
			t.Fatalf("%q: %v", spec, err)
		}
		args := strings.Join(ffmpeg_thumbnail_cmd("input", "output").Args, " ")
		if !strings.Contains(args, "fps="+strconv.Itoa(video_fps)+",scale="+strconv.Itoa(video_width)+":-1") {
			t.Fatalf("Incorrect ffmpeg arguments: %s", args)
		}
	}
	maxInt := uint64(^uint(0) >> 1)
	for _, key := range []string{"fps", "width"} {
		for _, value := range []string{"0", "-1", "bad", "1.5", "", strconv.FormatUint(maxInt, 10), strconv.FormatUint(maxInt+1, 10), "18446744073709551615"} {
			spec := key + "=" + value
			if err := parse_video_preview(spec); err == nil {
				t.Errorf("Accepted %q", spec)
			}
		}
	}
	for _, spec := range []string{"fps=61", "width=4097", "duration=0", "duration=-1", "duration=NaN", "duration=Inf", "duration=bad", "unknown=1", "fps", "'"} {
		if err := parse_video_preview(spec); err == nil {
			t.Errorf("Accepted %q", spec)
		}
	}
	beforeDuration, beforeFPS, beforeWidth := video_duration, video_fps, video_width
	if err := parse_video_preview("fps=12 width=-1"); err == nil {
		t.Fatal("Accepted invalid partial configuration")
	}
	if video_duration != beforeDuration || video_fps != beforeFPS || video_width != beforeWidth {
		t.Fatal("Invalid configuration changed preview settings")
	}
}

func TestShellOutput(t *testing.T) {
	paths := []string{"simple", "a path", "a;command", "*.txt", "~root", "a>output", `a\b`, "it's"}
	actual, err := shlex.Split(shell_output(paths))
	if err != nil {
		t.Fatal(err)
	}
	if diff := cmp.Diff(paths, actual); diff != "" {
		t.Fatalf("Shell output did not round-trip paths:\n%s", diff)
	}
	if q := shell_output([]string{"simple", "a/b"}); q != "simple a/b" {
		t.Fatalf("Shell-safe paths were unnecessarily quoted: %q", q)
	}

	original_cwd := default_cwd
	default_cwd = t.TempDir()
	t.Cleanup(func() { default_cwd = original_cwd })
	relative, err := shlex.Split(for_shell_relative(filepath.Join(default_cwd, "a;command")))
	if err != nil {
		t.Fatal(err)
	}
	if diff := cmp.Diff([]string{"a;command"}, relative); diff != "" {
		t.Fatalf("Shell-relative output did not round-trip the path:\n%s", diff)
	}
}

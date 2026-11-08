package choose_files

import (
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
)

func resetVideoPreview(t *testing.T) {
	t.Helper()
	duration, fps, width, quality := video_duration, video_fps, video_width, video_encoding_quality
	t.Cleanup(func() { video_duration, video_fps, video_width, video_encoding_quality = duration, fps, width, quality })
	video_duration, video_fps, video_width, video_encoding_quality = 5, 10, 480, 75
}

func TestVideoPreviewValidation(t *testing.T) {
	for _, tc := range []struct {
		name, spec, duration, filter string
	}{
		{"empty", "", "5.000000", "fps=10,scale=480:-1:flags=lanczos"},
		{"defaults", "width=480 fps=10 duration=5", "5.000000", "fps=10,scale=480:-1:flags=lanczos"},
		{"ordinary", "width=640 fps=24 duration=2.5", "2.500000", "fps=24,scale=640:-1:flags=lanczos"},
		{"minimum", "width=1 fps=1 duration=0.000001", "0.000001", "fps=1,scale=1:-1:flags=lanczos"},
		{"maximum", "width=4096 fps=60 duration=10", "10.000000", "fps=60,scale=4096:-1:flags=lanczos"},
		{"input_width", "width=0", "5.000000", "fps=10,scale=0:-1:flags=lanczos"},
		{"partial", "fps=12", "5.000000", "fps=12,scale=480:-1:flags=lanczos"},
		{"quoted", `width="640" fps='24' duration=2.5`, "2.500000", "fps=24,scale=640:-1:flags=lanczos"},
		{"last_value", "width=480 width=640", "5.000000", "fps=10,scale=640:-1:flags=lanczos"},
		{"duration_limit", "duration=" + strconv.FormatFloat(math.Nextafter(float64(math.MaxInt64)/1e6, 0), 'g', -1, 64),
			"9223372036854.773438", "fps=10,scale=480:-1:flags=lanczos"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			resetVideoPreview(t)
			if err := parse_video_preview(tc.spec); err != nil {
				t.Fatal(err)
			}
			args := ffmpeg_thumbnail_cmd("input with spaces.mp4", "output.webp").Args
			want := []string{
				"ffmpeg", "-loglevel", "fatal", "-y", "-an", "-sn", "-dn", "-i", "input with spaces.mp4", "-t", tc.duration,
				"-vf", tc.filter, "-c:v", "libwebp", "-lossless", "0", "-compression_level", "0", "-q:v", "75", "-loop", "0", "-f", "webp", "output.webp",
			}
			if !slices.Equal(args, want) {
				t.Fatalf("ffmpeg arguments: got %q, want %q", args, want)
			}
			if video_width < 0 || video_width > 4096 || video_fps < 1 || video_fps > 60 {
				t.Fatal("Accepted out-of-range or wrapped preview settings")
			}
			micros, err := strconv.ParseInt(strings.ReplaceAll(args[10], ".", ""), 10, 64)
			if err != nil || micros <= 0 {
				t.Fatalf("Invalid ffmpeg microsecond duration %q: %v", args[10], err)
			}
		})
	}
}

func TestVideoPreviewRejectedSettings(t *testing.T) {
	maxInt := uint64(^uint(0) >> 1)
	invalid := []string{
		"fps=0", "fps=61", "width=4097", "duration=0", "duration=-0", "duration=-1", "duration=NaN", "duration=Inf", "duration=-Inf",
		"duration=bad", "duration=", "duration=1e309", "duration=1e-400", "duration=1e-10", "duration=1e20",
		"duration=" + strconv.FormatFloat(math.Nextafter(1e-6, 0), 'g', -1, 64),
		"duration=" + strconv.FormatFloat(float64(math.MaxInt64)/1e6, 'g', -1, 64),
		"unknown=1", "fps", "'", "fps=20 fps=0", "width=640 width=-1",
	}
	for _, key := range []string{"fps", "width"} {
		for _, value := range []string{
			"-1", "bad", "1.5", "1e1", "0x10", "", "18446744073709551616",
			strconv.FormatUint(maxInt, 10), strconv.FormatUint(maxInt+1, 10), strconv.FormatUint(math.MaxUint64, 10),
		} {
			invalid = append(invalid, key+"="+value)
		}
	}
	for _, spec := range invalid {
		t.Run(spec, func(t *testing.T) {
			resetVideoPreview(t)
			before := slices.Clone(ffmpeg_thumbnail_cmd("input", "output").Args)
			if err := parse_video_preview("duration=3 fps=12 width=640 " + spec); err == nil {
				t.Fatalf("Accepted invalid %q", spec)
			}
			if !slices.Equal(before, ffmpeg_thumbnail_cmd("input", "output").Args) {
				t.Fatal("Invalid configuration changed preview settings")
			}
		})
	}
}

func TestVideoPreviewPartialSettings(t *testing.T) {
	resetVideoPreview(t)
	if err := parse_video_preview("duration=3 fps=12 width=640"); err != nil {
		t.Fatal(err)
	}
	for _, spec := range []string{"", "fps=24"} {
		if err := parse_video_preview(spec); err != nil {
			t.Fatal(err)
		}
		if video_duration != 3 || video_width != 640 {
			t.Fatal("Unspecified settings were reset")
		}
	}
	if video_fps != 24 {
		t.Fatal("Partial configuration was not applied")
	}
}

func TestVideoPreviewFFmpeg(t *testing.T) {
	ffmpeg, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Skip("ffmpeg is not installed")
	}
	dir := t.TempDir()
	input := filepath.Join(dir, "input.mkv")
	cmd := exec.Command(ffmpeg, "-loglevel", "error", "-f", "lavfi", "-i", "color=c=black:s=256x16:r=25:d=0.2", "-c:v", "ffv1", input)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("Creating ffmpeg fixture: %v: %s", err, output)
	}
	for i, spec := range []string{"width=480 fps=10 duration=5", "width=640 fps=24 duration=2.5", "width=0", "width=4096 fps=60 duration=10"} {
		t.Run(spec, func(t *testing.T) {
			resetVideoPreview(t)
			if err := parse_video_preview(spec); err != nil {
				t.Fatal(err)
			}
			outpath := filepath.Join(dir, fmt.Sprintf("output-%d.webp", i))
			if output, err := ffmpeg_thumbnail_cmd(input, outpath).CombinedOutput(); err != nil {
				t.Fatalf("ffmpeg rejected accepted configuration: %v: %s", err, output)
			}
			if st, err := os.Stat(outpath); err != nil || st.Size() == 0 {
				t.Fatalf("ffmpeg did not produce a thumbnail: %v", err)
			}
		})
	}
}

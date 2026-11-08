package choose_files

import (
	"fmt"
	"math"
	"os/exec"
	"strconv"
	"strings"

	"github.com/kovidgoyal/kitty/tools/utils"
	"github.com/kovidgoyal/kitty/tools/utils/shlex"
)

var (
	video_width            = 480
	video_fps              = 10
	video_duration         = 5.0
	video_encoding_quality = 75
)

func ffmpeg_thumbnail_cmd(path, outpath string) *exec.Cmd {
	return utils.Command(
		"ffmpeg", "-loglevel", "fatal", "-y", "-an", "-sn", "-dn", "-i", path, "-t", fmt.Sprintf("%f", video_duration),
		"-vf", fmt.Sprintf("fps=%d,scale=%d:-1:flags=lanczos", video_fps, video_width),
		"-c:v", "libwebp", "-lossless", "0", "-compression_level", "0", "-q:v",
		fmt.Sprintf("%d", video_encoding_quality), "-loop", "0", "-f", "webp", outpath,
	)
}

func parse_preview_integer(key, value string, minimum, maximum int) (int, error) {
	i, err := strconv.ParseUint(value, 10, strconv.IntSize-1)
	if err != nil || i < uint64(minimum) || i > uint64(maximum) {
		return 0, fmt.Errorf("invalid %s in video_preview: %s (must be between %d and %d)", key, value, minimum, maximum)
	}
	return int(i), nil
}

func parse_video_preview(spec string) error {
	parts, err := shlex.Split(spec)
	if err != nil {
		return err
	}
	duration, fps, width := video_duration, video_fps, video_width
	for _, x := range parts {
		k, v, found := strings.Cut(x, "=")
		if !found {
			return fmt.Errorf("invalid value %s in video_preview", x)
		}
		switch k {
		case "duration":
			duration, err = strconv.ParseFloat(v, 64)
			// ffmpeg's -t is a signed 64-bit microsecond count; %f emits six decimal places.
			if err != nil || math.IsNaN(duration) || math.IsInf(duration, 0) || duration < 1e-6 || duration >= float64(math.MaxInt64)/1e6 {
				return fmt.Errorf("invalid %s in video_preview: %s (must be at least one microsecond and fit ffmpeg's duration range)", k, v)
			}
		case "fps":
			fps, err = parse_preview_integer(k, v, 1, 60)
		case "width":
			width, err = parse_preview_integer(k, v, 0, 4096)
		default:
			return fmt.Errorf("unrecognized key in video_preview: %s", k)
		}
		if err != nil {
			return err
		}
	}
	video_duration, video_fps, video_width = duration, fps, width
	return nil
}

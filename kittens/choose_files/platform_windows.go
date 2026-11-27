package choose_files

import (
	"os"
	"time"
)

const open_nonblock = 0
const calibre_start_timeout = 15 * time.Second

func stop_calibre_process(process *os.Process) {
	_ = process.Kill()
}

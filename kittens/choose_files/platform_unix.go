//go:build !windows

package choose_files

import (
	"os"
	"time"

	"golang.org/x/sys/unix"
)

const open_nonblock = unix.O_NONBLOCK
const calibre_start_timeout = 2 * time.Second

func stop_calibre_process(process *os.Process) {
	// The forked workers share the server's process group and can outlive it.
	_ = unix.Kill(-process.Pid, unix.SIGKILL)
}

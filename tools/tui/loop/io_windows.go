// License: GPLv3 Copyright: 2025, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package loop

import (
	"errors"
	"os"
	"sync/atomic"

	"github.com/kovidgoyal/go-parallel"
	"github.com/kovidgoyal/kitty/tools/tty"
	"github.com/kovidgoyal/kitty/tools/utils"
)

// The main loop signals the I/O goroutines to quit by closing the write end
// of pipe_r. Console handles cannot be multiplexed with pipes via select(),
// so a helper goroutine converts the pipe EOF into an interrupt of any
// blocked console read and a flag checked by the writer.
func watch_for_quit(pipe_r *os.File, term *tty.Term, quit *atomic.Bool) {
	var b [1]byte
	_, _ = pipe_r.Read(b[:])
	quit.Store(true)
	if term != nil {
		term.Interrupt()
	}
}

func read_from_tty(pipe_r *os.File, term *tty.Term, results_channel chan<- []byte, err_channel chan<- error, quit_channel <-chan byte, leftover_channel chan<- []byte) {
	defer func() {
		if r := recover(); r != nil {
			err := parallel.Format_stacktrace_on_panic(r, 1)
			err_channel <- err
		}
	}()
	var quit atomic.Bool
	go watch_for_quit(pipe_r, term, &quit)
	defer func() {
		close(results_channel)
		pipe_r.Close()
	}()

	const bufsize = 2 * utils.DEFAULT_IO_BUFFER_SIZE
	buf := make([]byte, bufsize)
	for !quit.Load() {
		if len(buf) < 64 {
			buf = make([]byte, bufsize)
		}
		n, err := term.ReadKeyEvents(buf, encode_windows_key_event)
		if err != nil {
			if errors.Is(err, tty.ErrInterrupted) && quit.Load() {
				break
			}
			err_channel <- err
			break
		}
		if n == 0 {
			continue
		}
		send := buf[:n]
		buf = buf[n:]
		select {
		case results_channel <- send:
		case <-quit_channel:
			leftover_channel <- send
			return
		}
	}
}

func write_to_tty(
	pipe_r *os.File, term *tty.Term,
	job_channel <-chan write_msg, err_channel chan<- error, write_done_channel chan<- IdType,
) {
	defer func() {
		if r := recover(); r != nil {
			err_channel <- parallel.Format_stacktrace_on_panic(r, 1)
		}
	}()
	var quit atomic.Bool
	go watch_for_quit(pipe_r, nil, &quit)
	defer func() {
		pipe_r.Close()
		close(write_done_channel)
	}()

	// Every console write is a round trip to ConPTY, about 40us, and a single redraw
	// is often hundreds of small messages, so write all queued messages at once.
	const max_batch_size = 64 * 1024
	buf := make([]byte, 0, max_batch_size)
	var ids []IdType
	closed := false
	add := func(data write_msg) {
		if data.bytes == nil {
			buf = append(buf, data.str...)
		} else {
			buf = append(buf, data.bytes...)
		}
		ids = append(ids, data.id)
	}
	for !closed {
		data, more := <-job_channel
		if !more {
			break
		}
		add(data)
	gather:
		for len(buf) < max_batch_size {
			select {
			case data, more := <-job_channel:
				if !more {
					closed = true
					break gather
				}
				add(data)
			default:
				break gather
			}
		}
		written := 0
		for written < len(buf) {
			if quit.Load() {
				return
			}
			n, err := term.Write(buf[written:])
			written += n
			if err != nil {
				if is_temporary_error(err) {
					if n == 0 {
						// an incomplete UTF-8 sequence, completed by a later message
						break
					}
					continue
				}
				err_channel <- err
				return
			}
		}
		if written < len(buf) && !closed {
			buf = buf[:copy(buf, buf[written:])]
			continue
		}
		for _, id := range ids {
			write_done_channel <- id
		}
		buf, ids = buf[:0], ids[:0]
	}
}

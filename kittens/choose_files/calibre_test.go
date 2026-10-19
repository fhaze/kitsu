package choose_files

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"sync"
	"testing"
	"time"
)

// A stand-in for calibre's metadata server, answering requests in order, the
// first one slowly
func fake_calibre_server(t *testing.T) *calibre_server_process {
	req_r, req_w := io.Pipe()
	resp_r, resp_w := io.Pipe()
	go func() {
		defer resp_w.Close()
		s := bufio.NewScanner(req_r)
		for n := 0; s.Scan(); n++ {
			if n == 0 {
				time.Sleep(300 * time.Millisecond)
			}
			if _, err := fmt.Fprintf(resp_w, "%s\n", s.Text()); err != nil {
				return
			}
		}
	}()
	c := &calibre_server_process{stdin: req_w, stdout: resp_r, responses: make(chan calibre_line, 16)}
	go c.read_responses()
	t.Cleanup(func() { req_w.Close(); resp_r.Close() })
	return c
}

func TestCalibreRequestsGetTheirOwnResponses(t *testing.T) {
	c := fake_calibre_server(t)
	if _, err := c.request([]byte("slow"), 50*time.Millisecond); !errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("expected a timeout, got: %v", err)
	}
	// The late response to the timed out request must not be taken for this one
	if r, err := c.request([]byte("second"), 5*time.Second); err != nil || r != "second" {
		t.Fatalf("got the response %q, %v", r, err)
	}
	var wg sync.WaitGroup
	for i := range 8 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			q := fmt.Sprintf("concurrent-%d", i)
			if r, err := c.request([]byte(q), 5*time.Second); err != nil || r != q {
				t.Errorf("request %q got the response %q, %v", q, r, err)
			}
		}()
	}
	wg.Wait()
}

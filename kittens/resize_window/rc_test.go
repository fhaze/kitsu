// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package resize_window

import (
	"encoding/json"
	"io"
	"net"
	"strings"
	"testing"

	"github.com/emmansun/base64"
)

func TestResizeCommandEscapeCode(t *testing.T) {
	t.Setenv("KITTY_DCS_CHANNEL", "")
	ec, err := resize_command_escape_code(-4, "vertical", 0)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(ec, "\x1bP@kitty-cmd") || !strings.HasSuffix(ec, "\x1b\\") {
		t.Fatalf("bad framing: %q", ec)
	}
	var cmd struct {
		Cmd     string `json:"cmd"`
		Version [3]int `json:"version"`
		Payload struct {
			Increment int    `json:"increment"`
			Axis      string `json:"axis"`
			Self      bool   `json:"self"`
		} `json:"payload"`
	}
	body := strings.TrimSuffix(strings.TrimPrefix(ec, "\x1bP@kitty-cmd"), "\x1b\\")
	if err := json.Unmarshal([]byte(body), &cmd); err != nil {
		t.Fatal(err)
	}
	if cmd.Cmd != "resize-window" || cmd.Payload.Increment != -4 || cmd.Payload.Axis != "vertical" || !cmd.Payload.Self {
		t.Fatalf("bad command: %+v", cmd)
	}
	if cmd.Version == [3]int{} {
		t.Fatal("version not set")
	}
}

func TestResizeResetUsesEntrySnapshotOnlyWhenRequested(t *testing.T) {
	for _, axis := range []string{"restore", "reset"} {
		ec, err := resize_command_escape_code(2, axis, 0)
		if err != nil {
			t.Fatal(err)
		}
		var cmd struct {
			Payload resize_payload `json:"payload"`
		}
		body := strings.TrimSuffix(strings.TrimPrefix(ec, "\x1bP@kitty-cmd"), "\x1b\\")
		if err := json.Unmarshal([]byte(body), &cmd); err != nil {
			t.Fatal(err)
		}
		if cmd.Payload.Axis != "reset" || cmd.Payload.Restore != (axis == "restore") {
			t.Fatalf("reset source was not preserved: %+v", cmd.Payload)
		}
	}
}

func TestResizeCommandDCSChannel(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	t.Setenv("KITTY_DCS_CHANNEL", "tcp:"+ln.Addr().String())
	t.Setenv("KITTY_DCS_CHANNEL_TOKEN", "test-token")
	t.Setenv("KITTY_WINDOW_ID", "42")
	ec, err := resize_command_escape_code(2, "horizontal", 0)
	if err != nil {
		t.Fatal(err)
	}
	if ec != "" {
		t.Fatalf("unexpected tty escape code: %q", ec)
	}
	conn, err := ln.Accept()
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	data, err := io.ReadAll(conn)
	if err != nil {
		t.Fatal(err)
	}
	var msg struct {
		Cmd      string `json:"cmd"`
		Token    string `json:"token"`
		WindowID int    `json:"window_id"`
		DCS      string `json:"dcs"`
	}
	if err := json.Unmarshal(data, &msg); err != nil {
		t.Fatal(err)
	}
	dcs, err := base64.StdEncoding.DecodeString(msg.DCS)
	if err != nil {
		t.Fatal(err)
	}
	if msg.Cmd != "dcs" || msg.Token != "test-token" || msg.WindowID != 42 || !strings.HasPrefix(string(dcs), "@kitty-cmd{") {
		t.Fatalf("bad message: %+v, dcs: %q", msg, dcs)
	}
}

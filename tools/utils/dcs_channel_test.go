// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package utils

import (
	"io"
	"net"
	"strings"
	"testing"
)

func TestDCSChannelMessageStartsWithToken(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	t.Setenv("KITTY_DCS_CHANNEL", "tcp:"+ln.Addr().String())
	t.Setenv("KITTY_DCS_CHANNEL_TOKEN", "test-token")
	t.Setenv("KITTY_WINDOW_ID", "42")
	if err = SendDCSViaChannel("overlay-ready", ""); err != nil {
		t.Fatal(err)
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
	// kitty checks this exact prefix before parsing the message, see
	// dcs_channel_message_prefix() in kitty/boss.py
	if prefix := `{"cmd":"dcs","token":"test-token",`; !strings.HasPrefix(string(data), prefix) {
		t.Fatalf("message does not start with %#v: %#v", prefix, string(data))
	}
}

// License: GPLv3 Copyright: 2025, Kovid Goyal, <kovid at kovidgoyal.net>

package utils

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/emmansun/base64"
)

var _ = fmt.Print

// DCSChannelAddress returns the out-of-band channel kitty uses to receive
// @kitty-* DCS messages when the tty cannot carry them (ConPTY on Windows).
func DCSChannelAddress() string {
	return os.Getenv("KITTY_DCS_CHANNEL")
}

// SendDCSViaChannel delivers the DCS payload @kitty-<msgtype>|<body> to kitty
// over the out-of-band channel instead of the tty.
func SendDCSViaChannel(msgtype, body string) error {
	return SendRawDCSViaChannel("@kitty-" + msgtype + "|" + body)
}

// The field order matters: kitty requires messages to start with the cmd and
// token so that it can check the token before parsing the rest, see
// dcs_channel_message_prefix() in kitty/boss.py
type dcs_channel_message struct {
	Cmd      string `json:"cmd"`
	Token    string `json:"token"`
	WindowID int    `json:"window_id"`
	DCS      string `json:"dcs"`
}

// SendRawDCSViaChannel delivers a complete @kitty- DCS payload.
func SendRawDCSViaChannel(dcs string) error {
	window_id, err := strconv.Atoi(os.Getenv("KITTY_WINDOW_ID"))
	if err != nil {
		return fmt.Errorf("Invalid KITTY_WINDOW_ID env var: %#v", os.Getenv("KITTY_WINDOW_ID"))
	}
	msg, err := json.Marshal(dcs_channel_message{
		Cmd:      "dcs",
		Token:    os.Getenv("KITTY_DCS_CHANNEL_TOKEN"),
		WindowID: window_id,
		DCS:      base64.StdEncoding.EncodeToString(UnsafeStringToBytes(dcs)),
	})
	if err != nil {
		return err
	}
	network, address, found := strings.Cut(DCSChannelAddress(), ":")
	if !found {
		return fmt.Errorf("Invalid KITTY_DCS_CHANNEL address: %#v", DCSChannelAddress())
	}
	conn, err := net.DialTimeout(network, address, 5*time.Second)
	if err != nil {
		return fmt.Errorf("Failed to connect to kitty DCS channel %s with error: %w", DCSChannelAddress(), err)
	}
	defer conn.Close()
	_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	_, err = conn.Write(msg)
	return err
}

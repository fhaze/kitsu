// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package transfer

import (
	"strings"
	"testing"

	"github.com/kovidgoyal/kitty/tools/crypto"
	"github.com/kovidgoyal/kitty/tools/tui/loop"
)

func TestSendBypassInitialization(t *testing.T) {
	for _, key := range []string{"", "invalid", "1:00"} {
		t.Run("refuse "+key, func(t *testing.T) {
			t.Setenv("KITTY_PUBLIC_KEY", key)
			manager := &SendManager{request_id: "rid", bypass: "secret"}
			// A nil loop panics on any write, so failure must return before sending.
			handler := SendHandler{manager: manager, opts: &Options{PermissionsBypass: "secret"}}
			if err := handler.initialize(); err == nil {
				t.Fatal("expected authentication encoding failure")
			}
			if manager.bypass != "" || handler.spinner != nil || handler.ctx != nil || handler.file_metadata_sent || handler.transmit_started {
				t.Fatal("failed initialization retained the password or started the transfer")
			}
		})
	}
	_, publicKey, err := crypto.KeyPair("1")
	if err != nil {
		t.Fatal(err)
	}
	encodedKey, err := crypto.EncodePublicKey(publicKey, "1")
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("KITTY_PUBLIC_KEY", encodedKey)
	lp, err := loop.New()
	if err != nil {
		t.Fatal(err)
	}
	manager := &SendManager{request_id: "rid", bypass: "secret"}
	handler := SendHandler{manager: manager, lp: lp, opts: &Options{PermissionsBypass: "secret"}}
	if err := handler.initialize(); err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(manager.bypass, "kitty-1:") || !handler.file_metadata_sent {
		t.Fatal("successful initialization did not use encrypted authentication")
	}
	cmd, err := NewFileTransmissionCommand(manager.start_transfer())
	if err != nil {
		t.Fatal(err)
	}
	if cmd.Bypass != manager.bypass {
		t.Fatal("transfer command did not serialize the encrypted token")
	}
}

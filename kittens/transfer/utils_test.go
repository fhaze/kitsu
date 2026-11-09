// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

package transfer

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/sha256"
	"encoding/json"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/kovidgoyal/kitty/tools/crypto"
	"github.com/kovidgoyal/kitty/tools/utils/base85"
)

func TestEncodeBypass(t *testing.T) {
	privateKey, publicKey, err := crypto.KeyPair("1")
	if err != nil {
		t.Fatal(err)
	}
	encodedKey, err := crypto.EncodePublicKey(publicKey, "1")
	if err != nil {
		t.Fatal(err)
	}
	for name, key := range map[string]string{
		"missing key": "", "malformed key": "invalid", "unsupported protocol": "2:" + strings.SplitN(encodedKey, ":", 2)[1],
		"invalid key data": "1::::::", "short key": "1:00", "incomplete key data": "1:~",
	} {
		t.Run(name, func(t *testing.T) {
			t.Setenv("KITTY_PUBLIC_KEY", key)
			token, err := encode_bypass("rid", "secret")
			if err == nil || token != "" {
				t.Fatalf("expected refusal without a token or legacy downgrade, got %q, %v", token, err)
			}
		})
	}
	t.Setenv("KITTY_PUBLIC_KEY", encodedKey)
	token, err := encode_bypass("rid", "secret")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(token, "kitty-1:") {
		t.Fatalf("expected encrypted authentication, got %q", token)
	}
	var envelope struct {
		Pubkey, IV, Tag, Encrypted string
	}
	if err := json.Unmarshal([]byte(strings.TrimPrefix(token, "kitty-1:")), &envelope); err != nil {
		t.Fatal(err)
	}
	decode := func(value string) []byte {
		t.Helper()
		ans, err := base85.DecodeString(value)
		if err != nil {
			t.Fatal(err)
		}
		return ans
	}
	priv, err := ecdh.X25519().NewPrivateKey(privateKey)
	if err != nil {
		t.Fatal(err)
	}
	pub, err := ecdh.X25519().NewPublicKey(decode(envelope.Pubkey))
	if err != nil {
		t.Fatal(err)
	}
	shared, err := priv.ECDH(pub)
	if err != nil {
		t.Fatal(err)
	}
	key := sha256.Sum256(shared)
	block, err := aes.NewCipher(key[:])
	if err != nil {
		t.Fatal(err)
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	plaintext, err := gcm.Open(nil, decode(envelope.IV), append(decode(envelope.Encrypted), decode(envelope.Tag)...), nil)
	if err != nil {
		t.Fatal(err)
	}
	timestamp, payload, found := strings.Cut(string(plaintext), ":")
	if !found || payload != "rid;secret" {
		t.Fatalf("unexpected encrypted payload: %q", plaintext)
	}
	ts, err := strconv.ParseInt(timestamp, 10, 64)
	if err != nil || time.Since(time.Unix(0, ts)).Abs() > time.Minute {
		t.Fatalf("invalid token timestamp: %q, %v", timestamp, err)
	}
}

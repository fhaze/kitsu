package ssh

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"testing"
)

func TestAskpassResponseProtocol(t *testing.T) {
	for _, tc := range []struct {
		name, data, stdout   string
		confirm, fingerprint bool
	}{
		{"password", `" Mixed Case password "`, " Mixed Case password \n", false, false},
		{"password_escapes", `"tab\tline\nquote\""`, "tab\tline\nquote\"\n", false, false},
		{"empty", `""`, "", false, false},
		{"null_password", `null`, "", false, false},
		{"confirm_yes", `true`, "yes\n", true, false},
		{"confirm_no", `false`, "no\n", true, false},
		{"null_confirm", `null`, "no\n", true, false},
		{"fingerprint_yes", `"Y"`, "yes\n", false, true},
		{"fingerprint_no", `"N"`, "no\n", false, true},
		{"fingerprint", `"SHA256:AbCd"`, "sha256:abcd\n", false, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var stdout bytes.Buffer
			if err := write_askpass_response(&stdout, []byte(tc.data), tc.confirm, tc.fingerprint); err != nil {
				t.Fatal(err)
			}
			if stdout.String() != tc.stdout {
				t.Fatalf("stdout protocol mismatch: %q != %q", stdout.String(), tc.stdout)
			}
		})
	}
}

func TestAskpassResponseRedaction(t *testing.T) {
	// Type mismatches must not expose decoder details either.
	const sensitive_number = "98765432109876543210"
	var value string
	var type_error *json.UnmarshalTypeError
	if err := json.Unmarshal([]byte(sensitive_number), &value); !errors.As(err, &type_error) {
		t.Fatal("test must cover a JSON type error")
	}
	for _, data := range []string{
		`"synthetic-private-value`,
		`{"synthetic-private-field":"synthetic-private-value"}`,
		`["synthetic-private-value"]`,
		`"synthetic-private-value" trailing`,
		sensitive_number,
	} {
		for _, confirm := range []bool{false, true} {
			var stdout, stderr bytes.Buffer
			err := write_askpass_response(&stdout, []byte(data), confirm, false)
			if err == nil {
				t.Fatal("malformed response accepted")
			}
			fmt.Fprintln(&stderr, err)
			if stdout.Len() != 0 || stderr.String() != "Failed to parse askpass response\n" {
				t.Fatal("malformed response must produce only the fixed diagnostic and no stdout")
			}
		}
	}
}

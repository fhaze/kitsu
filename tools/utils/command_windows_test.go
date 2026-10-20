// License: GPLv3 Copyright: 2026, Kovid Goyal, <kovid at kovidgoyal.net>

//go:build windows

package utils

import (
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"

	"github.com/google/go-cmp/cmp"
	"golang.org/x/sys/windows"
)

func TestIsBatchFile(t *testing.T) {
	for path, expected := range map[string]bool{
		`C:\x\a.bat`: true, `C:\x\a.CMD`: true, `a.cmd`: true, `.cmd`: true, `C:\x\a.cmd. .`: true, `C:\x\a.bat...`: true,
		`\\?\C:\x\a.cmd`: true, `\\?\C:\x\a.cmd.`: false, `C:\x\a.exe`: false, `C:\x\acmd`: false, `cmd`: false, `C:\x.cmd\a`: false,
	} {
		if actual := IsBatchFile(path); actual != expected {
			t.Errorf("IsBatchFile(%#v) = %v, expected %v", path, actual, expected)
		}
	}
}

func TestBatchCommandLine(t *testing.T) {
	// The same string as kitty_win32_append_batch_arg() in kitty/win32-compat.c
	// produces, see test_windows_batch_file_arguments_outside_windows in
	// kitty_tests/child.py
	actual, err := BatchCommandLine(`C:\x\r.cmd`, []string{"a b", "", "100%", `a"b`, `C:\dir\`, `x\"y`, "a&b", "safe-arg_1"})
	if err != nil {
		t.Fatal(err)
	}
	expected := `cmd.exe /d /e:ON /v:OFF /s /c " C:\x\r.cmd "a b" "" "100%%cd:~,%" "a""b" "C:\dir\\" "x\\""y" "a&b" safe-arg_1"`
	if diff := cmp.Diff(expected, actual); diff != "" {
		t.Fatalf("Unexpected command line:\n%s", diff)
	}
	for _, arg := range []string{"a\rb", "a\nb", "a\r\nb"} {
		if _, err = BatchCommandLine(`C:\x\r.cmd`, []string{arg}); !errors.Is(err, syscall.EINVAL) {
			t.Fatalf("Argument %#v did not fail with EINVAL: %v", arg, err)
		}
	}
}

// Run by the batch files TestCommandRunsBatchFilesSafely creates to record the
// arguments they receive
func TestBatchFileHelperProcess(t *testing.T) {
	output := os.Getenv("KITTY_TEST_RECORD_ARGS_TO")
	if output == "" {
		return
	}
	args := os.Args
	for i, a := range args {
		if a == "--" {
			args = args[i+1:]
			break
		}
	}
	data, err := json.Marshal(args)
	if err == nil {
		err = os.WriteFile(output, data, 0o600)
	}
	if err != nil {
		os.Exit(1)
	}
	os.Exit(0)
}

func TestCommandRunsBatchFilesSafely(t *testing.T) {
	inject := "echo.>pwned"
	// The Go runtime splits the command line of the recording helper with the
	// "prior to 2008" rules, which read "" inside quotes differently from the C
	// runtime that the escaping targets, so arguments with double quotes are
	// only covered by the exact command line checked in TestBatchCommandLine
	// and by the kitty tests, which record arguments with kitty.exe.
	args := []string{`C:\tmp\a.txt&` + inject, "x|" + inject, "x<a.json&" + inject, "x>y&" + inject, "x^&" + inject, "(x)&" + inject}
	args = append(args, "%PATH%", "100%", "%cd%", "%%", "%=C:%", "%~dp0", "%1", "%*", "%%cd:~,%", "!PATH!", "!", "", " ", "a b", "\t")
	args = append(args, `C:\dir\`, `\\`, ";", ",", "=", "[]{}", "`~'", "ünïcødé&"+inject, strings.Repeat("y", 5000))
	tdir := t.TempDir()
	// cmd.exe must be able to run a script whose path has metacharacters too
	sdir := filepath.Join(tdir, "a b&c%PATH%d^")
	if err := os.Mkdir(sdir, 0o700); err != nil {
		t.Fatal(err)
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	recorder := "@\"" + self + "\" -test.run=TestBatchFileHelperProcess -- %*\r\n"
	script := filepath.Join(sdir, "record args.cmd")
	for _, name := range []string{script, filepath.Join(sdir, "recorder.cmd")} {
		if err = os.WriteFile(name, []byte(recorder), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	output, pwned := filepath.Join(tdir, "args.json"), filepath.Join(tdir, "pwned")
	run := func(name string) {
		cmd := Command(name, args...)
		if cmd.SysProcAttr == nil || cmd.SysProcAttr.CmdLine == "" {
			t.Fatalf("%s is not run via cmd.exe", name)
		}
		DetachCommand(cmd)
		if cmd.SysProcAttr.CmdLine == "" || cmd.SysProcAttr.CreationFlags&windows.CREATE_NO_WINDOW == 0 {
			t.Fatalf("DetachCommand() lost the command line or did not detach")
		}
		cmd.Dir = tdir
		cmd.Env = append(os.Environ(), "KITTY_TEST_RECORD_ARGS_TO="+output)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("Running %s failed with error: %s and output: %s", name, err, out)
		}
		data, err := os.ReadFile(output)
		if err != nil {
			t.Fatalf("%s did not record its arguments: %s", name, err)
		}
		var recorded []string
		if err = json.Unmarshal(data, &recorded); err != nil {
			t.Fatal(err)
		}
		if diff := cmp.Diff(args, recorded); diff != "" {
			t.Fatalf("%s received different arguments:\n%s", name, diff)
		}
		if _, err = os.Stat(pwned); err == nil {
			t.Fatalf("An argument to %s was run as a command", name)
		}
		_ = os.Remove(output)
	}
	for _, name := range []string{script, strings.ToUpper(script), script + ". ."} {
		run(name)
	}
	// A bare name resolves to the batch file via PATHEXT
	t.Setenv("PATH", sdir+string(os.PathListSeparator)+os.Getenv("PATH"))
	run("recorder")
	// So does a name with a directory and no extension, even next to an
	// extensionless file of that name, as in the bin dir of VS Code, which has
	// both code and code.cmd. exec.Command() only adds the extension in Start().
	if err = os.WriteFile(filepath.Join(sdir, "recorder"), []byte("#!/bin/sh\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	run(filepath.Join(sdir, "recorder"))
	t.Chdir(tdir)
	run(filepath.Join(filepath.Base(sdir), "recorder"))

	// A missing batch file still fails to start, instead of cmd.exe reporting it
	missing := Command(filepath.Join(sdir, "missing.cmd"), "x")
	if err = missing.Run(); !errors.Is(err, fs.ErrNotExist) {
		t.Fatalf("Running a missing batch file did not fail with ErrNotExist: %v", err)
	}

	if err = Command(script, "a\r\ndir").Run(); !errors.Is(err, syscall.EINVAL) {
		t.Fatalf("An argument with CR LF did not fail with EINVAL: %v", err)
	}
	if cmd := Command("cmd.exe", "/c", "exit"); cmd.SysProcAttr != nil {
		t.Fatalf("A program that is not a batch file got a custom command line: %#v", cmd.SysProcAttr)
	}
}

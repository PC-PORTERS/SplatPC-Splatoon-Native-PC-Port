package prereq

import (
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"
)

// linuxTerminals are tried in order; each entry is the program and the flag
// after which it takes the command to run.
var linuxTerminals = [][]string{
	{"x-terminal-emulator", "-e"},
	{"gnome-terminal", "--"},
	{"konsole", "-e"},
	{"xfce4-terminal", "-x"},
	{"xterm", "-e"},
}

// OpenInstallTerminal runs the install steps in a visible terminal window, so
// the player sees the progress and can answer the Windows permission prompt
// or type the sudo password.
func OpenInstallTerminal(steps []InstallStep) error {
	if len(steps) == 0 {
		return fmt.Errorf("nothing to install")
	}
	if runtime.GOOS == "windows" {
		script, err := writeScript("splatpc-install-*.cmd", windowsInstallScript(steps))
		if err != nil {
			return err
		}
		// The Builder has no console, so cmd.exe opens a window of its own.
		return exec.Command("cmd.exe", "/c", script).Start()
	}
	script, err := writeScript("splatpc-install-*.sh", linuxInstallScript(steps))
	if err != nil {
		return err
	}
	for _, term := range linuxTerminals {
		if _, err := exec.LookPath(term[0]); err == nil {
			return exec.Command(term[0], term[1], "bash", script).Start()
		}
	}
	return fmt.Errorf("no terminal program was found; run this in a terminal:\n%s", installScript(steps, nil))
}

func writeScript(pattern, content string) (string, error) {
	f, err := os.CreateTemp("", pattern)
	if err != nil {
		return "", err
	}
	defer f.Close()
	if _, err := f.WriteString(content); err != nil {
		return "", err
	}
	return f.Name(), nil
}

func windowsInstallScript(steps []InstallStep) string {
	var b strings.Builder
	b.WriteString("@echo off\r\ntitle SplatPC - Installing missing tools\r\n")
	b.WriteString("echo SplatPC Builder: installing the missing tools.\r\n")
	b.WriteString("echo Windows may ask for permission; click Yes.\r\necho.\r\nset failed=0\r\n")
	for i, s := range steps {
		fmt.Fprintf(&b, "echo [%d/%d] %s\r\n%s\r\nset rc=%%errorlevel%%\r\n", i+1, len(steps), s.Name, s.Command)
		// 3010: installed, Windows wants a restart.
		fmt.Fprintf(&b, "if not \"%%rc%%\"==\"0\" if not \"%%rc%%\"==\"3010\" echo     %s did not install, error %%rc%%.\r\n", s.Name)
		b.WriteString("if not \"%rc%\"==\"0\" if not \"%rc%\"==\"3010\" set failed=1\r\necho.\r\n")
	}
	b.WriteString("if \"%failed%\"==\"0\" echo Done. Close this window and start the SplatPC Builder again.\r\n")
	b.WriteString("if not \"%failed%\"==\"0\" echo Something did not install. Click Troubleshooting in the SplatPC Builder.\r\n")
	b.WriteString("pause\r\n")
	return b.String()
}

func linuxInstallScript(steps []InstallStep) string {
	var b strings.Builder
	b.WriteString("#!/usr/bin/env bash\necho 'SplatPC Builder: installing the missing tools.'\necho\nfailed=0\n")
	for i, s := range steps {
		fmt.Fprintf(&b, "echo '[%d/%d] %s'\n%s || { echo '    %s did not install.'; failed=1; }\necho\n", i+1, len(steps), s.Name, s.Command, s.Name)
	}
	b.WriteString("if [ \"$failed\" = 0 ]; then echo 'Done. Click \"Re-check system\" in the SplatPC Builder.'; " +
		"else echo 'Something did not install. Click Troubleshooting in the SplatPC Builder.'; fi\n")
	b.WriteString("read -rp 'Press Enter to close this window.' _\n")
	return b.String()
}

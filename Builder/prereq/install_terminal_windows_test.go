package prereq

import (
	"os"
	"os/exec"
	"strings"
	"testing"
)

func TestWindowsInstallScriptReportsFailedSteps(t *testing.T) {
	steps := []InstallStep{
		{Name: "Works", Command: "cmd /c exit 0"},
		{Name: "Needs restart (VS)", Command: "cmd /c exit 3010"},
		{Name: "Breaks", Command: "cmd /c exit 5"},
	}
	script, err := writeScript("splatpc-install-test-*.cmd", windowsInstallScript(steps))
	if err != nil {
		t.Fatal(err)
	}
	defer os.Remove(script)

	cmd := exec.Command("cmd.exe", "/c", script)
	cmd.Stdin = strings.NewReader("\r\n") // answers the final pause
	out, _ := cmd.CombinedOutput()
	got := string(out)
	for _, want := range []string{"[1/3] Works", "[3/3] Breaks", "Breaks did not install, error 5.", "Something did not install"} {
		if !strings.Contains(got, want) {
			t.Errorf("script output lacks %q:\n%s", want, got)
		}
	}
	for _, unwanted := range []string{"Works did not install", "Needs restart (VS) did not install"} {
		if strings.Contains(got, unwanted) {
			t.Errorf("script output wrongly contains %q:\n%s", unwanted, got)
		}
	}
}

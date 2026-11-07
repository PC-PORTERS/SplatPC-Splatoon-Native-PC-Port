package settings

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestBatEscapeKeepsPseudoAsText(t *testing.T) {
	got := batEscape("x\" & calc & \"%PATH%!^\r\nend")
	for _, bad := range []string{"\"", "%", "!", "^", "\r", "\n"} {
		if strings.Contains(got, bad) {
			t.Errorf("batEscape left %q in %q", bad, got)
		}
	}
	if got != "x & calc & PATHend" {
		t.Errorf("unexpected result %q", got)
	}
	if batEscape("Ana") != "Ana" {
		t.Errorf("a plain pseudo must stay unchanged")
	}
}

func TestWindowsLauncherGoesThroughLogin(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Launch_Splatoon.bat is only written on Windows")
	}
	dir := t.TempDir()
	cfg := DefaultConfiguration("/tmp")
	cfg.PlayerName = "Ana\" & calc"
	cfg.EnableOnline = true
	cfg.OnlineServer = "127.0.0.1:60000"
	if err := WriteLauncherScript(cfg, dir); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(dir, "Launch_Splatoon.bat"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	if !strings.Contains(text, `scripts\Splatoon-Login.ps1`) || !strings.Contains(text, "--offline") {
		t.Errorf("online launcher must go through Splatoon-Login.ps1 and honour --offline:\n%s", text)
	}
	if !strings.Contains(text, "set \"RBREW_PLAYER_NAME=Ana & calc\"\n") {
		t.Errorf("pseudo must stay inside its quotes:\n%s", text)
	}

	cfg.EnableOnline = false
	if err := WriteLauncherScript(cfg, dir); err != nil {
		t.Fatal(err)
	}
	data, _ = os.ReadFile(filepath.Join(dir, "Launch_Splatoon.bat"))
	if strings.Contains(string(data), "Splatoon-Login.ps1") {
		t.Errorf("offline launcher must not call the login script")
	}
}

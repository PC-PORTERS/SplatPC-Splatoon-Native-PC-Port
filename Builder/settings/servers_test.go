package settings

import (
	"os"
	"path/filepath"
	"testing"
)

func TestServerPresetsAndSettingsRoundTrip(t *testing.T) {
	root := t.TempDir()
	scripts := filepath.Join(root, "scripts")
	if err := os.MkdirAll(scripts, 0o755); err != nil {
		t.Fatal(err)
	}
	write := func(name, text string) {
		if err := os.WriteFile(filepath.Join(scripts, name), []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("server.public.txt", "name=Community\nserver=203.0.113.10:60000\nnncs1=203.0.113.10\nnncs2=203.0.113.11\nregister=https://example.org/register\n")
	write("server.self.txt", "# comment\nname=Self hosted\nserver=198.51.100.7:25300\naccount=https://account.example.net\nboss=https://boss.example.net/pc/boss/eur\n")
	write("server.broken.txt", "name=No server line\n")

	presets := ServerPresets(root)
	if len(presets) != 2 || presets[0].Name != "Community" || presets[1].Account != "https://account.example.net" {
		t.Fatalf("presets: %+v", presets)
	}

	var cfg Configuration
	cfg.EnableOnline = true
	cfg.ApplyPreset(presets[1])
	if cfg.PresetIndex(presets) != 1 {
		t.Fatal("preset not recognised after ApplyPreset")
	}
	cfg.OnlineBoss = "https://other.example.net/boss"
	if cfg.PresetIndex(presets) != -1 {
		t.Fatal("an edited preset must count as custom")
	}

	out := t.TempDir()
	if err := WriteSettingsIni(cfg, out); err != nil {
		t.Fatal(err)
	}
	var back Configuration
	if err := LoadSettingsIni(filepath.Join(out, "settings.ini"), &back); err != nil {
		t.Fatal(err)
	}
	if back.OnlineServer != cfg.OnlineServer || back.OnlineAccount != cfg.OnlineAccount || back.OnlineBoss != cfg.OnlineBoss {
		t.Fatalf("round trip: %+v", back)
	}
	// Second write goes through the "existing file" path and must keep the values.
	if err := WriteSettingsIni(back, out); err != nil {
		t.Fatal(err)
	}
	var again Configuration
	_ = LoadSettingsIni(filepath.Join(out, "settings.ini"), &again)
	if again.OnlineAccount != cfg.OnlineAccount || again.OnlineBoss != cfg.OnlineBoss {
		t.Fatalf("rewrite: %+v", again)
	}
}

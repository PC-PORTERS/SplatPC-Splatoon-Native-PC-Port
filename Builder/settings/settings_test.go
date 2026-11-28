package settings

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRegionMaskGivesTheGameANumber(t *testing.T) {
	cases := map[string]string{
		"EUR":   "0x4",
		"usa":   "0x2",
		" JPN ": "0x1",
		"0x4":   "0x4",
		"0X2":   "0x2",
		"4":     "4",
		"":      "",
		"Mars":  "",
	}
	for region, want := range cases {
		if got := RegionMask(region); got != want {
			t.Errorf("RegionMask(%q) = %q, expected %q", region, got, want)
		}
	}
}

func TestDefaultConfiguration(t *testing.T) {
	cfg := DefaultConfiguration("/tmp/splatpc")
	if cfg.PlayerName != "default" {
		t.Errorf("Expected player name 'default', got %s", cfg.PlayerName)
	}
	if cfg.ParallelJobs < 1 {
		t.Errorf("Expected at least 1 job, got %d", cfg.ParallelJobs)
	}
	expectedOutputDir := filepath.Join("/tmp/splatpc", "Game")
	if cfg.OutputDir != expectedOutputDir {
		t.Errorf("Expected output dir %s, got %s", expectedOutputDir, cfg.OutputDir)
	}
}

func TestOutputDirFor(t *testing.T) {
	if got, want := OutputDirFor("/tmp/splatpc", false), filepath.Join("/tmp/splatpc", "Game"); got != want {
		t.Errorf("Expected repository output dir %s, got %s", want, got)
	}

	t.Setenv("APPIMAGE", "/opt/splatpc/SplatPC-Builder-x86_64.AppImage")
	if got, want := OutputDirFor("/tmp/splatpc", true), filepath.Join("/opt/splatpc", "Game"); got != want {
		t.Errorf("Expected standalone AppImage output dir %s, got %s", want, got)
	}
}

func TestWriteSettingsIniFresh(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_settings_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	cfg := DefaultConfiguration("/tmp")
	cfg.PlayerName = "TestSquid"
	cfg.Resolution = "1440p"
	cfg.FrameRate = 120
	cfg.Plaza60FPS = true

	err = WriteSettingsIni(cfg, tempDir)
	if err != nil {
		t.Fatalf("WriteSettingsIni failed: %v", err)
	}

	iniPath := filepath.Join(tempDir, "settings.ini")
	data, err := os.ReadFile(iniPath)
	if err != nil {
		t.Fatalf("Failed to read settings.ini: %v", err)
	}

	content := string(data)
	if !strings.Contains(content, "player_name=TestSquid") {
		t.Errorf("Expected player_name=TestSquid in settings.ini")
	}
	if !strings.Contains(content, "resolution=1440p") {
		t.Errorf("Expected resolution=1440p in settings.ini")
	}
	if !strings.Contains(content, "frame_rate=120") {
		t.Errorf("Expected frame_rate=120 in settings.ini")
	}
}

func TestLoadAndPreserveCustomSettings(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_custom_settings_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	existingContent := `# Custom user settings
resolution=1080p
frame_rate=30
fullscreen=0
fps_overlay=1
# Custom keybindings
bindings=2
key_a=40
key_b=25
pad_a=1
weather=2
look_1=custom_look_data
`
	iniPath := filepath.Join(tempDir, "settings.ini")
	if err := os.WriteFile(iniPath, []byte(existingContent), 0644); err != nil {
		t.Fatalf("Failed to write initial settings.ini: %v", err)
	}

	cfg := DefaultConfiguration("/tmp")
	if err := LoadSettingsIni(iniPath, &cfg); err != nil {
		t.Fatalf("LoadSettingsIni failed: %v", err)
	}

	if cfg.FrameRate != 30 {
		t.Errorf("Expected imported frame_rate=30, got %d", cfg.FrameRate)
	}
	if cfg.Fullscreen != false {
		t.Errorf("Expected imported fullscreen=false, got %v", cfg.Fullscreen)
	}
	if cfg.FPSOverlay != true {
		t.Errorf("Expected imported fps_overlay=true, got %v", cfg.FPSOverlay)
	}

	// Change only nickname and resolution in Builder
	cfg.PlayerName = "OctolingHero"
	cfg.Resolution = "1440p"

	// Write back
	if err := WriteSettingsIni(cfg, tempDir); err != nil {
		t.Fatalf("WriteSettingsIni failed: %v", err)
	}

	savedData, err := os.ReadFile(iniPath)
	if err != nil {
		t.Fatalf("Failed to read back settings.ini: %v", err)
	}
	saved := string(savedData)

	// Check that custom unmanaged keys were preserved untouched
	if !strings.Contains(saved, "key_a=40") {
		t.Errorf("Expected custom keybinding key_a=40 to be preserved")
	}
	if !strings.Contains(saved, "key_b=25") {
		t.Errorf("Expected custom keybinding key_b=25 to be preserved")
	}
	if !strings.Contains(saved, "weather=2") {
		t.Errorf("Expected custom weather=2 to be preserved")
	}
	if !strings.Contains(saved, "look_1=custom_look_data") {
		t.Errorf("Expected custom look_1 to be preserved")
	}

	// Check that user-modified settings were updated
	if !strings.Contains(saved, "player_name=OctolingHero") {
		t.Errorf("Expected updated player_name=OctolingHero")
	}
	if !strings.Contains(saved, "resolution=1440p") {
		t.Errorf("Expected updated resolution=1440p")
	}
}

func TestHDTexturingOption(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_hd_settings_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	cfg := DefaultConfiguration("/tmp")
	cfg.UpscaleTextures = true

	if err := WriteSettingsIni(cfg, tempDir); err != nil {
		t.Fatalf("WriteSettingsIni failed: %v", err)
	}

	data, err := os.ReadFile(filepath.Join(tempDir, "settings.ini"))
	if err != nil {
		t.Fatalf("Failed to read settings.ini: %v", err)
	}

	if !strings.Contains(string(data), "hd_textures=1") {
		t.Errorf("Expected hd_textures=1 in settings.ini, got:\n%s", string(data))
	}
}

func TestOnlineSettings(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_online_settings_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	cfg := DefaultConfiguration("/tmp")
	cfg.PlayerName = "OnlineSquid"
	cfg.EnableOnline = true
	cfg.OnlineServer = "127.0.0.1:60000"

	// 1. Fresh write with online enabled
	if err := WriteSettingsIni(cfg, tempDir); err != nil {
		t.Fatalf("WriteSettingsIni failed: %v", err)
	}

	iniPath := filepath.Join(tempDir, "settings.ini")
	data, err := os.ReadFile(iniPath)
	if err != nil {
		t.Fatalf("Failed to read settings.ini: %v", err)
	}
	content := string(data)
	if !strings.Contains(content, "online_server=127.0.0.1:60000") {
		t.Errorf("Expected online_server=127.0.0.1:60000 in settings.ini, got:\n%s", content)
	}
	if !strings.Contains(content, "pseudo=OnlineSquid") {
		t.Errorf("Expected pseudo=OnlineSquid in settings.ini, got:\n%s", content)
	}

	// 2. Load settings back into a blank config
	var loadedCfg Configuration
	if err := LoadSettingsIni(iniPath, &loadedCfg); err != nil {
		t.Fatalf("LoadSettingsIni failed: %v", err)
	}
	if !loadedCfg.EnableOnline {
		t.Errorf("Expected EnableOnline to be true")
	}
	if loadedCfg.OnlineServer != "127.0.0.1:60000" {
		t.Errorf("Expected OnlineServer 127.0.0.1:60000, got %s", loadedCfg.OnlineServer)
	}
	if loadedCfg.PlayerName != "OnlineSquid" {
		t.Errorf("Expected PlayerName OnlineSquid, got %s", loadedCfg.PlayerName)
	}

	// 3. Disabling online clears online_server
	cfg.EnableOnline = false
	if err := WriteSettingsIni(cfg, tempDir); err != nil {
		t.Fatalf("WriteSettingsIni (offline) failed: %v", err)
	}
	var reloadedOffline Configuration
	if err := LoadSettingsIni(iniPath, &reloadedOffline); err != nil {
		t.Fatalf("LoadSettingsIni (offline) failed: %v", err)
	}
	if reloadedOffline.EnableOnline || reloadedOffline.OnlineServer != "" {
		t.Errorf("Expected offline settings to clear online_server, got %q", reloadedOffline.OnlineServer)
	}
}

func TestLoadThenWriteKeepsPlayerSettings(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_settings_load_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	// A settings file as the game and the launcher leave it.
	iniPath := filepath.Join(tempDir, "settings.ini")
	content := "# Splatoon PC port settings (ESC menu)\nresolution=1440p\nframe_rate=120\nfullscreen=0\nshader_quality=2\nsimple_overlay=1\n"
	if err := os.WriteFile(iniPath, []byte(content), 0644); err != nil {
		t.Fatalf("Failed to write settings.ini: %v", err)
	}

	cfg := DefaultConfiguration("/tmp/splatpc")
	if err := LoadSettingsIni(iniPath, &cfg); err != nil {
		t.Fatalf("LoadSettingsIni failed: %v", err)
	}
	if err := WriteSettingsIni(cfg, tempDir); err != nil {
		t.Fatalf("WriteSettingsIni failed: %v", err)
	}

	data, err := os.ReadFile(iniPath)
	if err != nil {
		t.Fatalf("Failed to read settings.ini: %v", err)
	}
	written := string(data)
	for _, expected := range []string{"resolution=1440p", "frame_rate=120", "fullscreen=0", "shader_quality=2", "simple_overlay=1"} {
		if !strings.Contains(written, expected) {
			t.Errorf("Expected settings.ini to keep %q, got:\n%s", expected, written)
		}
	}
}

func TestGraphicsAndCameraQoLSettingsRoundTrip(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_qol_settings_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	for _, res := range []string{"4k", "8k"} {
		cfg := DefaultConfiguration("/tmp/splatpc")
		cfg.Resolution = res
		cfg.AnisotropicFiltering = 16
		cfg.DepthOfField = false
		cfg.AntiAliasing = true
		cfg.AAMode = "smaa"
		cfg.Upscaler = "fsr"
		if res == "8k" {
			cfg.Upscaler = "anime4k"
		}
		cfg.UpscalerSharpness = 85
		cfg.MotionBlur = false
		cfg.DisableAutoPitch = true

		if err := WriteSettingsIni(cfg, tempDir); err != nil {
			t.Fatalf("WriteSettingsIni(%s) failed: %v", res, err)
		}
		iniPath := filepath.Join(tempDir, "settings.ini")
		data, err := os.ReadFile(iniPath)
		if err != nil {
			t.Fatalf("ReadFile failed: %v", err)
		}
		content := string(data)
		for _, want := range []string{
			"resolution=" + res,
			"anisotropic_filtering=16",
			"depth_of_field=0",
			"antialiasing=1",
			"aa_mode=smaa",
			"upscaler=" + cfg.Upscaler,
			"upscaler_sharpness=85",
			"motion_blur=0",
			"disable_auto_pitch=1",
		} {
			if !strings.Contains(content, want) {
				t.Errorf("Expected %q in settings.ini, got:\n%s", want, content)
			}
		}

		loaded := DefaultConfiguration("/tmp/splatpc")
		if err := LoadSettingsIni(iniPath, &loaded); err != nil {
			t.Fatalf("LoadSettingsIni(%s) failed: %v", res, err)
		}
		if loaded.Resolution != res || loaded.AnisotropicFiltering != 16 || loaded.DepthOfField ||
			!loaded.AntiAliasing || loaded.AAMode != "smaa" || loaded.Upscaler != cfg.Upscaler ||
			loaded.UpscalerSharpness != 85 || loaded.MotionBlur || !loaded.DisableAutoPitch {
			t.Errorf("Round-trip mismatch for %s: %+v", res, loaded)
		}
	}
}

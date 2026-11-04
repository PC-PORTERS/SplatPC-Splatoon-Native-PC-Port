package settings

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDefaultConfiguration(t *testing.T) {
	cfg := DefaultConfiguration("/tmp/splatpc")
	if cfg.PlayerName != "default" {
		t.Errorf("Expected player name 'default', got %s", cfg.PlayerName)
	}
	if cfg.ParallelJobs < 1 {
		t.Errorf("Expected at least 1 job, got %d", cfg.ParallelJobs)
	}
	if cfg.OutputDir != "/tmp/splatpc/Game" {
		t.Errorf("Expected output dir /tmp/splatpc/Game, got %s", cfg.OutputDir)
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

func TestWriteLauncherScript(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "test_launcher_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	cfg := DefaultConfiguration("/tmp")
	cfg.PlayerName = "InklingMaster"
	cfg.LanguageID = 2

	err = WriteLauncherScript(cfg, tempDir)
	if err != nil {
		t.Fatalf("WriteLauncherScript failed: %v", err)
	}

	// On Linux, Splatoon.sh is created
	scriptPath := filepath.Join(tempDir, "Splatoon.sh")
	data, err := os.ReadFile(scriptPath)
	if err != nil {
		t.Fatalf("Failed to read Splatoon.sh: %v", err)
	}

	content := string(data)
	if !strings.Contains(content, "RBREW_PLAYER_NAME=\"InklingMaster\"") {
		t.Errorf("Expected RBREW_PLAYER_NAME export in launcher script")
	}
	if !strings.Contains(content, "RBREW_CONSOLE_LANGUAGE=\"2\"") {
		t.Errorf("Expected RBREW_CONSOLE_LANGUAGE export in launcher script")
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

	// Verify launcher script sets RBREW_TEXTURE_DUMP
	if err := WriteLauncherScript(cfg, tempDir); err != nil {
		t.Fatalf("WriteLauncherScript failed: %v", err)
	}

	scriptPath := filepath.Join(tempDir, "Splatoon.sh")
	sData, err := os.ReadFile(scriptPath)
	if err != nil {
		t.Fatalf("Failed to read launcher script: %v", err)
	}
	if !strings.Contains(string(sData), "RBREW_TEXTURE_DUMP") {
		t.Errorf("Expected RBREW_TEXTURE_DUMP in launcher script when UpscaleTextures is true")
	}
}

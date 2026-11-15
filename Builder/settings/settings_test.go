package settings

import (
	"os"
	"path/filepath"
	"runtime"
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

	launcherName := "Splatoon.sh"
	expectedPlayer := `RBREW_PLAYER_NAME="InklingMaster"`
	expectedLang := `RBREW_CONSOLE_LANGUAGE="2"`
	if runtime.GOOS == "windows" {
		launcherName = "Launch_Splatoon.bat"
		expectedPlayer = `set "RBREW_PLAYER_NAME=InklingMaster"`
		expectedLang = `set "RBREW_CONSOLE_LANGUAGE=2"`
	}

	scriptPath := filepath.Join(tempDir, launcherName)
	data, err := os.ReadFile(scriptPath)
	if err != nil {
		t.Fatalf("Failed to read %s: %v", launcherName, err)
	}

	content := string(data)
	if !strings.Contains(content, expectedPlayer) {
		t.Errorf("Expected %s in launcher script", expectedPlayer)
	}
	if !strings.Contains(content, expectedLang) {
		t.Errorf("Expected %s in launcher script", expectedLang)
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

	launcherName := "Splatoon.sh"
	if runtime.GOOS == "windows" {
		launcherName = "Launch_Splatoon.bat"
	}
	scriptPath := filepath.Join(tempDir, launcherName)
	sData, err := os.ReadFile(scriptPath)
	if err != nil {
		t.Fatalf("Failed to read launcher script: %v", err)
	}
	if !strings.Contains(string(sData), "RBREW_TEXTURE_DUMP") {
		t.Errorf("Expected RBREW_TEXTURE_DUMP in launcher script when UpscaleTextures is true")
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

	// 3. Launcher script generation with online enabled
	if err := WriteLauncherScript(cfg, tempDir); err != nil {
		t.Fatalf("WriteLauncherScript failed: %v", err)
	}
	launcherName := "Splatoon.sh"
	if runtime.GOOS == "windows" {
		launcherName = "Launch_Splatoon.bat"
	}
	launcherData, err := os.ReadFile(filepath.Join(tempDir, launcherName))
	if err != nil {
		t.Fatalf("Failed to read launcher: %v", err)
	}
	launcherContent := string(launcherData)
	if !strings.Contains(launcherContent, "RBREW_ONLINE_SERVER") {
		t.Errorf("Expected RBREW_ONLINE_SERVER in launcher script when online enabled")
	}
	if !strings.Contains(launcherContent, "RBREW_PSEUDO") {
		t.Errorf("Expected RBREW_PSEUDO in launcher script when online enabled")
	}

	// 4. Launcher script generation with online disabled
	cfg.EnableOnline = false
	if err := WriteLauncherScript(cfg, tempDir); err != nil {
		t.Fatalf("WriteLauncherScript (offline) failed: %v", err)
	}
	launcherDataOff, err := os.ReadFile(filepath.Join(tempDir, launcherName))
	if err != nil {
		t.Fatalf("Failed to read offline launcher: %v", err)
	}
	launcherContentOff := string(launcherDataOff)
	if strings.Contains(launcherContentOff, "RBREW_ONLINE_SERVER") {
		t.Errorf("Did not expect RBREW_ONLINE_SERVER in launcher script when online disabled")
	}
}

func TestLinuxLauncherOnline(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Splatoon.sh is only written on Linux")
	}
	tempDir := t.TempDir()
	cfg := DefaultConfiguration("/tmp")
	cfg.PlayerName = "Sq\"$(id)`x`"
	cfg.EnableOnline = true
	cfg.OnlineServer = "127.0.0.1:60000"
	if err := WriteLauncherScript(cfg, tempDir); err != nil {
		t.Fatalf("WriteLauncherScript failed: %v", err)
	}
	data, err := os.ReadFile(filepath.Join(tempDir, "Splatoon.sh"))
	if err != nil {
		t.Fatalf("Failed to read launcher: %v", err)
	}
	content := string(data)
	for _, want := range []string{`cd "$HERE"`, "scripts/splatoon_login.py", `--game "$HERE"`, "RBREW_PSEUDO=\"Sq\\\"\\$(id)\\`x\\`\""} {
		if !strings.Contains(content, want) {
			t.Errorf("Expected %q in launcher:\n%s", want, content)
		}
	}
	cfg.EnableOnline = false
	if err := WriteLauncherScript(cfg, tempDir); err != nil {
		t.Fatalf("WriteLauncherScript (offline) failed: %v", err)
	}
	data, _ = os.ReadFile(filepath.Join(tempDir, "Splatoon.sh"))
	if strings.Contains(string(data), "splatoon_login.py") {
		t.Errorf("Did not expect the online launcher when online is disabled")
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

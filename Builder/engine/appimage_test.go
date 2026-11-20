package engine

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestCreateAppImage(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("AppImage tests only run on Linux")
	}

	tempDir, err := os.MkdirTemp("", "test_appimage_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	gameDir := filepath.Join(tempDir, "Game")
	_ = os.MkdirAll(gameDir, 0755)

	dummyExe := filepath.Join(gameDir, "Splatoon")
	scriptContent := "#!/bin/sh\necho Splatoon Test\n"
	_ = os.WriteFile(dummyExe, []byte(scriptContent), 0755)

	metaDir := filepath.Join(gameDir, "meta")
	_ = os.MkdirAll(metaDir, 0755)
	_ = os.WriteFile(filepath.Join(metaDir, "meta.xml"), []byte("<menu></menu>"), 0644)

	appImagePath, err := CreateAppImage(gameDir, tempDir, func(format string, args ...interface{}) {
		t.Logf(format, args...)
	})

	if err != nil {
		if strings.Contains(err.Error(), "Failed to download") || strings.Contains(err.Error(), "status code 0") {
			t.Skipf("Skipping AppImage test due to offline environment: %v", err)
		}
		t.Fatalf("CreateAppImage failed: %v", err)
	}

	if appImagePath == "" {
		t.Fatalf("Expected non-empty appImagePath")
	}

	if _, err := os.Stat(appImagePath); err != nil {
		t.Errorf("AppImage file was not created: %s", appImagePath)
	}
}

package engine

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"splatpc-builder/settings"
)

func OpenFolder(path string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("explorer", path)
	case "darwin":
		cmd = exec.Command("open", path)
	default:
		cmd = exec.Command("xdg-open", path)
	}
	return cmd.Start()
}

func LaunchGame(gameDir string, cfg settings.Configuration, log func(string, ...interface{})) (*exec.Cmd, error) {
	var exePath string
	if runtime.GOOS == "windows" {
		exePath = filepath.Join(gameDir, "Splatoon.exe")
	} else {
		exePath = filepath.Join(gameDir, "Splatoon")
	}

	if _, err := os.Stat(exePath); err != nil {
		return nil, fmt.Errorf("executable not found: %s", exePath)
	}

	cmd := exec.Command(exePath)
	cmd.Dir = gameDir

	// Inherit process environment and pass game configuration
	cmd.Env = os.Environ()
	cmd.Env = append(cmd.Env,
		fmt.Sprintf("RBREW_PLAYER_NAME=%s", cfg.PlayerName),
		fmt.Sprintf("RBREW_MII_NAME=%s", cfg.PlayerName),
		fmt.Sprintf("RBREW_CONSOLE_LANGUAGE=%d", cfg.LanguageID),
		fmt.Sprintf("RBREW_CONSOLE_REGION=%s", cfg.ConsoleRegion),
	)

	if cfg.UpscaleTextures {
		dumpDir := filepath.Join(gameDir, "texture-dump")
		_ = os.MkdirAll(dumpDir, 0755)
		cmd.Env = append(cmd.Env, fmt.Sprintf("RBREW_TEXTURE_DUMP=%s", dumpDir))
	}

	if log != nil {
		log("Launching game: %s with nickname '%s'", exePath, cfg.PlayerName)
	}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("failed to launch game: %w", err)
	}

	return cmd, nil
}

package engine

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// UpscaleTextures runs scripts/hd_textures.py on the texture-dump directory
// and outputs the upscaled HD textures to textures/.
func UpscaleTextures(repoRoot string, gameDir string, log func(string, ...interface{})) error {
	dumpDir := filepath.Join(gameDir, "texture-dump")
	packDir := filepath.Join(gameDir, "textures")
	scriptPath := filepath.Join(repoRoot, "scripts", "hd_textures.py")

	if _, err := os.Stat(scriptPath); err != nil {
		return fmt.Errorf("hd_textures.py script not found: %s", scriptPath)
	}

	_ = os.MkdirAll(dumpDir, 0755)
	_ = os.MkdirAll(packDir, 0755)

	entries, _ := os.ReadDir(dumpDir)
	var ddsCount int
	for _, e := range entries {
		if !e.IsDir() && filepath.Ext(e.Name()) == ".dds" {
			ddsCount++
		}
	}
	if ddsCount == 0 {
		return fmt.Errorf("no dumped textures found in %s\n\nPlay the game first (visit plaza, Octo Valley, or levels) so textures are recorded to texture-dump/, then run upscaling.", dumpDir)
	}

	pyBin := "python3"
	if runtime.GOOS == "windows" {
		if _, err := exec.LookPath("python3"); err != nil {
			pyBin = "python"
		}
	}

	cmd := exec.Command(pyBin, scriptPath, dumpDir, packDir)
	if log != nil {
		log("Running Real-ESRGAN HD texture upscaler: %s -> %s (%d textures)...", dumpDir, packDir, ddsCount)
	}

	return runCmdWithLog(cmd, log)
}

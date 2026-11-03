package engine

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

func CreateAppImage(gameFolder, repoRoot string, log func(string, ...interface{})) (string, error) {
	if runtime.GOOS != "linux" {
		return "", fmt.Errorf("AppImage packaging is only supported on Linux")
	}

	tool, err := exec.LookPath("appimagetool")
	if err != nil {
		return "", fmt.Errorf("'appimagetool' was not found in PATH: please install appimagetool to build an AppImage")
	}

	// Clean up any legacy Splatoon.AppDir in workspace if present
	legacyAppDir := filepath.Join(filepath.Dir(gameFolder), "Splatoon.AppDir")
	if _, statErr := os.Stat(legacyAppDir); statErr == nil {
		_ = os.RemoveAll(legacyAppDir)
	}

	// Create a dedicated temporary directory in system tmp
	tmpDir, err := os.MkdirTemp("", "splatpc_appimage_*")
	if err != nil {
		return "", fmt.Errorf("failed creating temporary build directory in tmp: %w", err)
	}
	defer func() {
		log("Cleaning up temporary AppDir directory: %s", tmpDir)
		_ = os.RemoveAll(tmpDir)
	}()

	log("Preparing AppDir directory structure in tmp...")
	appDir := filepath.Join(tmpDir, "Splatoon.AppDir")
	if err := os.MkdirAll(appDir, 0755); err != nil {
		return "", err
	}

	// Copy game directory into AppDir
	log("Copying game files into AppDir/game...")
	gameDest := filepath.Join(appDir, "game")
	if err := copyDirectoryTreeSimple(gameFolder, gameDest); err != nil {
		return "", fmt.Errorf("failed copying game directory: %w", err)
	}

	// Ensure game executable has executable permissions
	_ = os.Chmod(filepath.Join(gameDest, "Splatoon"), 0755)

	// Create AppRun entrypoint
	appRunPath := filepath.Join(appDir, "AppRun")
	appRunContent := `#!/usr/bin/env bash
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$HERE/game:$HERE/usr/lib:$LD_LIBRARY_PATH"
cd "$HERE/game"
exec ./Splatoon "$@"
`
	if err := os.WriteFile(appRunPath, []byte(appRunContent), 0755); err != nil {
		return "", err
	}
	_ = os.Chmod(appRunPath, 0755)

	// Create splatoon.desktop
	desktopPath := filepath.Join(appDir, "splatoon.desktop")
	desktopContent := `[Desktop Entry]
Type=Application
Name=Splatoon PC
Exec=AppRun
Icon=splatoon
Categories=Game;
Terminal=false
StartupNotify=true
`
	if err := os.WriteFile(desktopPath, []byte(desktopContent), 0644); err != nil {
		return "", err
	}

	// Create a verified valid PNG icon
	iconDest := filepath.Join(appDir, "splatoon.png")
	if err := createValidPNGIcon(iconDest); err != nil {
		return "", fmt.Errorf("failed creating PNG icon: %w", err)
	}

	tmpAppImage := filepath.Join(tmpDir, "Splatoon-x86_64.AppImage")

	log("Packaging AppImage with appimagetool in tmp...")
	cmd := exec.Command(tool, "-n", appDir, tmpAppImage)
	cmd.Dir = tmpDir
	cmd.Env = append(os.Environ(), "ARCH=x86_64")
	output, err := cmd.CombinedOutput()
	if err != nil {
		log("appimagetool output: %s", string(output))
		return "", fmt.Errorf("appimagetool failed: %s (%w)", string(output), err)
	}

	_ = os.Chmod(tmpAppImage, 0755)

	// Deploy final AppImage to project destination paths
	outAppImage := filepath.Join(filepath.Dir(gameFolder), "Splatoon-x86_64.AppImage")
	copyDest := filepath.Join(gameFolder, "Splatoon-x86_64.AppImage")

	if err := copySingleFile(tmpAppImage, outAppImage); err != nil {
		return "", fmt.Errorf("failed copying AppImage to %s: %w", outAppImage, err)
	}
	_ = os.Chmod(outAppImage, 0755)

	if outAppImage != copyDest {
		_ = copySingleFile(tmpAppImage, copyDest)
		_ = os.Chmod(copyDest, 0755)
	}

	log("AppImage successfully created at: %s", outAppImage)
	return outAppImage, nil
}

func createValidPNGIcon(destPath string) error {
	img := image.NewRGBA(image.Rect(0, 0, 128, 128))
	bg := color.RGBA{R: 20, G: 24, B: 33, A: 255}
	fg := color.RGBA{R: 0, G: 212, B: 170, A: 255}

	for y := 0; y < 128; y++ {
		for x := 0; x < 128; x++ {
			// Rounded square with inner icon border
			if x >= 8 && x <= 120 && y >= 8 && y <= 120 {
				if x >= 14 && x <= 114 && y >= 14 && y <= 114 {
					img.Set(x, y, fg)
				} else {
					img.Set(x, y, bg)
				}
			} else {
				img.Set(x, y, bg)
			}
		}
	}

	f, err := os.Create(destPath)
	if err != nil {
		return err
	}
	defer f.Close()

	return png.Encode(f, img)
}

func copyDirectoryTreeSimple(src, dst string) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if rel == "." {
			return nil
		}
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, 0755)
		}
		return copySingleFile(path, target)
	})
}

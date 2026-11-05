package roms

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
)

type CombinerLogger func(format string, args ...interface{})

func CombineROMs(basePath string, overlays []RomLayer, targetDir string, logger CombinerLogger) error {
	if logger == nil {
		logger = func(string, ...interface{}) {}
	}

	logger("Initializing combined folder: %s", targetDir)
	if err := os.MkdirAll(targetDir, 0755); err != nil {
		return fmt.Errorf("failed to create destination directory: %w", err)
	}

	// 1. Copy base dump
	logger("Applying base ROM v16: %s", basePath)
	if err := copyDirectoryTree(basePath, targetDir, false, logger); err != nil {
		return fmt.Errorf("error copying base ROM: %w", err)
	}

	// 2. Overlay updates and mods in order
	for i, layer := range overlays {
		logger("Applying overlay layer [%d/%d]: %s", i+1, len(overlays), layer.Name)
		allowRPX := layer.IsV288
		if layer.HasRPX && !allowRPX {
			logger("Note: The .rpx in '%s' is skipped because only v288 is supported to replace the built executable.", layer.Name)
		}
		if err := copyDirectoryTree(layer.Path, targetDir, allowRPX, logger); err != nil {
			return fmt.Errorf("error merging mod %s: %w", layer.Name, err)
		}
	}

	logger("ROMs and mods combined successfully.")
	return nil
}

func copyDirectoryTree(src, dst string, allowRPXOverwrite bool, logger CombinerLogger) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}

		// Don't copy root directory itself
		if rel == "." {
			return nil
		}

		targetPath := filepath.Join(dst, rel)

		// Check if this is the RPX file and we are forbidden from overwriting it
		if filepath.ToSlash(rel) == "code/Gambit.rpx" && !allowRPXOverwrite {
			if _, err := os.Stat(targetPath); err == nil {
				// Base or v288 RPX already exists, preserve it
				return nil
			}
		}

		if info.IsDir() {
			return os.MkdirAll(targetPath, 0755)
		}

		if info.Mode()&os.ModeSymlink != 0 {
			linkTarget, err := os.Readlink(path)
			if err != nil {
				return err
			}
			_ = os.Remove(targetPath)
			return os.Symlink(linkTarget, targetPath)
		}

		return copyFile(path, targetPath)
	})
}

func copyFile(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}

	sourceFile, err := os.Open(src)
	if err != nil {
		return err
	}
	defer sourceFile.Close()

	destFile, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer destFile.Close()

	if _, err := io.Copy(destFile, sourceFile); err != nil {
		return err
	}

	info, err := os.Stat(src)
	if err == nil {
		_ = os.Chmod(dst, info.Mode())
	}
	return nil
}

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

// copyDirectoryTree copies the content of src into dst. Symbolic links are followed: a dump kept
// behind links (a linked root, code folder or .rpx) is copied by content, because a link recreated
// in dst would point at nothing there. A link back to a folder being copied is skipped.
func copyDirectoryTree(src, dst string, allowRPXOverwrite bool, logger CombinerLogger) error {
	root, err := filepath.EvalSymlinks(src)
	if err != nil {
		return err
	}
	return copyFolder(root, dst, "", allowRPXOverwrite, map[string]bool{}, logger)
}

// copyFolder copies one folder; rel is its path from the layer root, in slash form, and active holds
// the folders of the current branch of the walk (resolved), which a link must not lead back to.
func copyFolder(src, dst, rel string, allowRPXOverwrite bool, active map[string]bool, logger CombinerLogger) error {
	resolved, err := filepath.EvalSymlinks(src)
	if err != nil {
		return err
	}
	if active[resolved] {
		logger("Skipping '%s': a link leads back to a folder being copied.", src)
		return nil
	}
	active[resolved] = true
	defer delete(active, resolved)

	if err := os.MkdirAll(dst, 0755); err != nil {
		return err
	}
	entries, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		path := filepath.Join(src, entry.Name())
		targetPath := filepath.Join(dst, entry.Name())
		entryRel := entry.Name()
		if rel != "" {
			entryRel = rel + "/" + entry.Name()
		}

		// Check if this is the RPX file and we are forbidden from overwriting it
		if entryRel == "code/Gambit.rpx" && !allowRPXOverwrite {
			if _, err := os.Stat(targetPath); err == nil {
				// Base or v288 RPX already exists, preserve it
				continue
			}
		}

		// Stat follows links: a link is copied as the file or folder it names.
		info, err := os.Stat(path)
		if err != nil {
			if entry.Type()&os.ModeSymlink != 0 {
				logger("Skipping '%s': the link points at nothing.", path)
				continue
			}
			return err
		}
		if info.IsDir() {
			err = copyFolder(path, targetPath, entryRel, allowRPXOverwrite, active, logger)
		} else {
			err = copyFile(path, targetPath)
		}
		if err != nil {
			return err
		}
	}
	return nil
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

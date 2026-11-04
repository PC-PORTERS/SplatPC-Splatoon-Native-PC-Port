package embedded

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

//go:embed *
var embeddedFS embed.FS

// GetAssetsZip returns the raw bytes of embedded assets.zip, or nil if not bundled.
func GetAssetsZip() []byte {
	data, err := embeddedFS.ReadFile("assets.zip")
	if err != nil {
		return nil
	}
	return data
}

// HasEmbeddedAssets returns true if bundled assets are embedded into the binary.
func HasEmbeddedAssets() bool {
	return len(GetAssetsZip()) > 1024
}

// IsRepoRoot returns true if the directory contains the essential SplatPC files.
func IsRepoRoot(path string) bool {
	if path == "" {
		return false
	}
	manifestDir := filepath.Join(path, "Manifest")
	recompilerDir := filepath.Join(path, "recompiler")
	setupScript := filepath.Join(path, "setup.sh")

	hasManifest := fileExists(manifestDir)
	hasRecompiler := fileExists(recompilerDir)
	hasSetup := fileExists(setupScript)

	return hasManifest && hasRecompiler && hasSetup
}

func fileExists(p string) bool {
	s, err := os.Stat(p)
	return err == nil && (s.IsDir() || s.Mode().IsRegular())
}

// GetBuildEnvDir returns the user-level persistent cache directory for building.
func GetBuildEnvDir() (string, error) {
	if runtime.GOOS == "windows" {
		localAppData := os.Getenv("LOCALAPPDATA")
		if localAppData == "" {
			home := os.Getenv("USERPROFILE")
			if home == "" {
				var err error
				home, err = os.UserHomeDir()
				if err != nil {
					return "", err
				}
			}
			localAppData = filepath.Join(home, "AppData", "Local")
		}
		return filepath.Join(localAppData, "SplatPC", "build-env"), nil
	}

	// Linux / macOS / Unix: follow XDG specification
	cacheHome := os.Getenv("XDG_CACHE_HOME")
	if cacheHome == "" {
		home := os.Getenv("HOME")
		if home == "" {
			var err error
			home, err = os.UserHomeDir()
			if err != nil {
				return "", err
			}
		}
		cacheHome = filepath.Join(home, ".cache")
	}
	return filepath.Join(cacheHome, "splatpc", "build-env"), nil
}

// ExtractEmbeddedAssets unpacks embedded assets into targetDir if not already up to date.
func ExtractEmbeddedAssets(targetDir string) error {
	assetsZip := GetAssetsZip()
	if len(assetsZip) <= 1024 {
		return fmt.Errorf("no embedded assets in builder")
	}

	hash := sha256.Sum256(assetsZip)
	hashStr := hex.EncodeToString(hash[:])
	hashFile := filepath.Join(targetDir, ".assets_hash")

	// Check if already extracted and matching hash
	if currentHash, err := os.ReadFile(hashFile); err == nil {
		if strings.TrimSpace(string(currentHash)) == hashStr && IsRepoRoot(targetDir) {
			return nil
		}
	}

	if err := os.MkdirAll(targetDir, 0755); err != nil {
		return fmt.Errorf("creating directory %s: %w", targetDir, err)
	}

	zipReader, err := zip.NewReader(bytes.NewReader(assetsZip), int64(len(assetsZip)))
	if err != nil {
		return fmt.Errorf("reading embedded zip archive: %w", err)
	}

	for _, f := range zipReader.File {
		cleanName := filepath.Clean(f.Name)
		if strings.HasPrefix(cleanName, "..") || filepath.IsAbs(cleanName) {
			continue
		}

		destPath := filepath.Join(targetDir, cleanName)

		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(destPath, 0755); err != nil {
				return fmt.Errorf("creating directory %s: %w", destPath, err)
			}
			continue
		}

		if err := os.MkdirAll(filepath.Dir(destPath), 0755); err != nil {
			return fmt.Errorf("creating parent directory for %s: %w", destPath, err)
		}

		rc, err := f.Open()
		if err != nil {
			return fmt.Errorf("opening file inside zip: %w", err)
		}

		mode := f.Mode()
		if mode == 0 {
			mode = 0644
		}
		// Ensure executability for scripts and manifests
		if strings.HasSuffix(cleanName, ".sh") || strings.HasSuffix(cleanName, ".py") {
			mode |= 0755
		}

		outFile, err := os.OpenFile(destPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, mode)
		if err != nil {
			rc.Close()
			return fmt.Errorf("creating destination file %s: %w", destPath, err)
		}

		_, copyErr := io.Copy(outFile, rc)
		closeErr := outFile.Close()
		rc.Close()

		if copyErr != nil {
			return fmt.Errorf("writing file %s: %w", destPath, copyErr)
		}
		if closeErr != nil {
			return fmt.Errorf("closing file %s: %w", destPath, closeErr)
		}
		_ = os.Chmod(destPath, mode)
	}

	if err := os.WriteFile(hashFile, []byte(hashStr), 0644); err != nil {
		return fmt.Errorf("writing assets hash: %w", err)
	}

	return nil
}

// ResolveRepoRoot determines whether to use a local repository or extract embedded assets.
// Returns the repository root path, a boolean indicating if it is standalone mode, and an error if any.
func ResolveRepoRoot() (string, bool, error) {
	checkHierarchy := func(startDir string) (string, bool) {
		curr := startDir
		for {
			if IsRepoRoot(curr) {
				return curr, true
			}
			parent := filepath.Dir(curr)
			if parent == curr {
				break
			}
			curr = parent
		}
		return "", false
	}

	// 1. Check current working directory
	if cwd, err := os.Getwd(); err == nil {
		if root, ok := checkHierarchy(cwd); ok {
			return root, false, nil
		}
	}

	// 2. Check executable directory
	if exe, err := os.Executable(); err == nil {
		if root, ok := checkHierarchy(filepath.Dir(exe)); ok {
			return root, false, nil
		}
	}

	// 3. Fallback to embedded assets
	if HasEmbeddedAssets() {
		buildEnvDir, err := GetBuildEnvDir()
		if err != nil {
			return "", false, fmt.Errorf("resolving cache build directory: %w", err)
		}
		if err := ExtractEmbeddedAssets(buildEnvDir); err != nil {
			return "", false, fmt.Errorf("extracting embedded sources to %s: %w", buildEnvDir, err)
		}
		return buildEnvDir, true, nil
	}

	// 4. Default fallback
	return ".", false, nil
}

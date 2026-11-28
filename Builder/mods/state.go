package mods

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

const modStateFileName = "mods_state.ini"

// LocalMod represents an installed mod plugin library discovered in <outputDir>/mods/.
type LocalMod struct {
	FileName         string
	FullPath         string
	SizeBytes        int64
	CatalogID        string
	DisplayName      string
	InstalledVersion string
	Disabled         bool
}

func pluginExtensionForOS(goos string) string {
	if goos == "windows" {
		return ".dll"
	}
	return ".so"
}

// LoadDisabledMods reads <outputDir>/mods/mods_state.ini and returns the disabled mod IDs.
func LoadDisabledMods(outputDir string) map[string]bool {
	disabled := make(map[string]bool)
	if strings.TrimSpace(outputDir) == "" {
		return disabled
	}
	data, err := os.ReadFile(filepath.Join(ModsDir(outputDir), modStateFileName))
	if err != nil {
		return disabled
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") || strings.HasPrefix(line, "[") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		id := strings.TrimSpace(k)
		val := strings.ToLower(strings.TrimSpace(v))
		if id != "" && (val == "0" || val == "false" || val == "off" || val == "disabled") {
			disabled[id] = true
		}
	}
	return disabled
}

// SetModDisabled updates <outputDir>/mods/mods_state.ini for modID using the same
// format read by the runtime mod loader.
func SetModDisabled(outputDir, modID string, disabled bool) error {
	modID = strings.TrimSpace(modID)
	if strings.TrimSpace(outputDir) == "" {
		return fmt.Errorf("game output directory is not set")
	}
	if modID == "" {
		return fmt.Errorf("mod ID cannot be empty")
	}

	state := LoadDisabledMods(outputDir)
	if disabled {
		state[modID] = true
	} else {
		delete(state, modID)
	}

	modsDir := ModsDir(outputDir)
	if err := os.MkdirAll(modsDir, 0755); err != nil {
		return err
	}

	ids := make([]string, 0, len(state))
	for id := range state {
		ids = append(ids, id)
	}
	sort.Strings(ids)

	var sb strings.Builder
	sb.WriteString("# SplatPC mod state (0 = disabled)\n[mods]\n")
	for _, id := range ids {
		sb.WriteString(id)
		sb.WriteString("=0\n")
	}

	dest := filepath.Join(modsDir, modStateFileName)
	tmp, err := os.CreateTemp(modsDir, ".mods_state-*.tmp")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	cleanup := true
	defer func() {
		_ = tmp.Close()
		if cleanup {
			_ = os.Remove(tmpPath)
		}
	}()
	if _, err := tmp.WriteString(sb.String()); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpPath, dest); err != nil {
		return err
	}
	cleanup = false
	return nil
}

// ListInstalledFiles lists installed plugin libraries inside <outputDir>/mods/.
func ListInstalledFiles(outputDir string, catalog []ModEntry) ([]LocalMod, error) {
	return listInstalledFilesForOS(outputDir, catalog, runtime.GOOS)
}

func listInstalledFilesForOS(outputDir string, catalog []ModEntry, goos string) ([]LocalMod, error) {
	if strings.TrimSpace(outputDir) == "" {
		return nil, nil
	}
	modsDir := ModsDir(outputDir)
	entries, err := os.ReadDir(modsDir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}

	wantExt := pluginExtensionForOS(goos)
	versions := loadInstalledManifest(outputDir)
	disabled := LoadDisabledMods(outputDir)

	var out []LocalMod
	for _, dirEntry := range entries {
		if dirEntry.IsDir() {
			continue
		}
		name := dirEntry.Name()
		if !strings.EqualFold(filepath.Ext(name), wantExt) {
			continue
		}
		info, infoErr := dirEntry.Info()
		var size int64
		if infoErr == nil {
			size = info.Size()
		}
		item := LocalMod{
			FileName:    name,
			FullPath:    filepath.Join(modsDir, name),
			SizeBytes:   size,
			DisplayName: name,
		}
		for _, cat := range catalog {
			_, catFile, ok := cat.AssetForOS(goos)
			if !ok {
				continue
			}
			matches := name == catFile
			if goos == "windows" {
				matches = strings.EqualFold(name, catFile)
			}
			if matches {
				item.CatalogID = cat.ID
				item.DisplayName = cat.Name
				item.InstalledVersion = versions[cat.ID]
				item.Disabled = disabled[cat.ID]
				break
			}
		}
		out = append(out, item)
	}
	sort.Slice(out, func(i, j int) bool {
		return strings.ToLower(out[i].FileName) < strings.ToLower(out[j].FileName)
	})
	return out, nil
}

// DeleteFile deletes a mod library file inside <outputDir>/mods/ and clears
// any matching catalog manifest/state entries.
func DeleteFile(outputDir, fileName string, catalog []ModEntry) error {
	if strings.TrimSpace(outputDir) == "" {
		return fmt.Errorf("game output directory is not set")
	}
	cleanName := filepath.Base(strings.TrimSpace(fileName))
	if cleanName == "" || cleanName == "." || cleanName == ".." || cleanName != strings.TrimSpace(fileName) ||
		strings.ContainsAny(fileName, `/\`) {
		return fmt.Errorf("invalid mod filename %q", fileName)
	}
	target := filepath.Join(ModsDir(outputDir), cleanName)
	if err := os.Remove(target); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("removing %s (close Splatoon if running): %w", cleanName, err)
	}
	for _, cat := range catalog {
		_, catFile, ok := cat.AssetForOS(runtime.GOOS)
		if !ok {
			continue
		}
		matches := cleanName == catFile
		if runtime.GOOS == "windows" {
			matches = strings.EqualFold(cleanName, catFile)
		}
		if matches {
			_ = removeInstalledVersion(outputDir, cat.ID)
			_ = SetModDisabled(outputDir, cat.ID, false)
		}
	}
	return nil
}

// InstallLocalFile installs a local mod plugin (.dll/.so) or archive (.zip/.tar.gz/.tgz)
// into <outputDir>/mods/.
func InstallLocalFile(outputDir, srcPath string) (string, error) {
	if strings.TrimSpace(outputDir) == "" {
		return "", fmt.Errorf("game output directory is not set")
	}
	srcPath = strings.TrimSpace(srcPath)
	info, err := os.Stat(srcPath)
	if err != nil {
		return "", err
	}
	if info.IsDir() {
		return "", fmt.Errorf("expected a mod file, got a folder")
	}

	modsDir := ModsDir(outputDir)
	if err := os.MkdirAll(modsDir, 0755); err != nil {
		return "", err
	}

	lower := strings.ToLower(srcPath)
	switch {
	case strings.HasSuffix(lower, ".zip"):
		if err := extractZipSafe(srcPath, modsDir); err != nil {
			return "", fmt.Errorf("extracting mod zip: %w", err)
		}
		return filepath.Base(srcPath), nil
	case strings.HasSuffix(lower, ".tar.gz") || strings.HasSuffix(lower, ".tgz"):
		if err := extractTarGzSafe(srcPath, modsDir); err != nil {
			return "", fmt.Errorf("extracting mod archive: %w", err)
		}
		return filepath.Base(srcPath), nil
	case strings.HasSuffix(lower, ".dll") || strings.HasSuffix(lower, ".so"):
		f, err := os.Open(srcPath)
		if err != nil {
			return "", err
		}
		defer f.Close()
		base := filepath.Base(srcPath)
		dest := filepath.Join(modsDir, base)
		if err := writeFileFromReader(dest, f, 0755); err != nil {
			return "", fmt.Errorf("installing %s (close Splatoon if running): %w", base, err)
		}
		return base, nil
	default:
		return "", fmt.Errorf("unsupported mod file type %q (expected .dll, .so, .zip, or .tar.gz)", filepath.Ext(srcPath))
	}
}

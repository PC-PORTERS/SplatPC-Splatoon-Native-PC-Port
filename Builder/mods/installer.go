package mods

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const manifestFileName = ".installed_mods.json"

type progressReader struct {
	r          io.Reader
	total      int64
	downloaded int64
	onProgress func(downloaded, total int64)
}

func (p *progressReader) Read(buf []byte) (int, error) {
	n, err := p.r.Read(buf)
	if n > 0 {
		p.downloaded += int64(n)
		if p.onProgress != nil {
			p.onProgress(p.downloaded, p.total)
		}
	}
	return n, err
}

// ModsDir returns the runtime mods folder inside outputDir.
func ModsDir(outputDir string) string {
	return filepath.Join(outputDir, "mods")
}

// BinaryPath returns the full path where entry's plugin library lives on the current OS.
func BinaryPath(outputDir string, entry ModEntry) (string, bool) {
	if strings.TrimSpace(outputDir) == "" {
		return "", false
	}
	_, fileName, ok := entry.AssetForOS(runtime.GOOS)
	if !ok {
		return "", false
	}
	return filepath.Join(ModsDir(outputDir), fileName), true
}

// IsInstalled reports whether entry's plugin file is present in <outputDir>/mods/.
func IsInstalled(outputDir string, entry ModEntry) bool {
	p, ok := BinaryPath(outputDir, entry)
	if !ok {
		return false
	}
	info, err := os.Stat(p)
	return err == nil && !info.IsDir()
}

// InstalledVersion returns the recorded installed version if the plugin file exists on disk.
func InstalledVersion(outputDir string, entry ModEntry) string {
	if !IsInstalled(outputDir, entry) {
		return ""
	}
	versions := loadInstalledManifest(outputDir)
	return versions[entry.ID]
}

// InstalledSummary returns a concise description of installed catalog and local mods for display.
func InstalledSummary(outputDir string, entries []ModEntry) string {
	files, err := ListInstalledFiles(outputDir, entries)
	if err == nil && len(files) > 0 {
		names := make([]string, 0, len(files))
		for _, f := range files {
			label := f.DisplayName
			if f.InstalledVersion != "" {
				label += " (" + f.InstalledVersion + ")"
			}
			if f.Disabled {
				label += " [Disabled]"
			}
			names = append(names, label)
		}
		return strings.Join(names, ", ")
	}
	var names []string
	for _, entry := range entries {
		if IsInstalled(outputDir, entry) {
			if v := InstalledVersion(outputDir, entry); v != "" {
				names = append(names, fmt.Sprintf("%s (%s)", entry.Name, v))
			} else {
				names = append(names, entry.Name)
			}
		}
	}
	if len(names) == 0 {
		return "None installed"
	}
	return strings.Join(names, ", ")
}

// Install downloads and installs entry into <outputDir>/mods/.
func Install(ctx context.Context, outputDir string, entry ModEntry, onProgress func(downloaded, total int64)) error {
	return installForOS(ctx, outputDir, entry, runtime.GOOS, http.DefaultClient, onProgress)
}

func installForOS(ctx context.Context, outputDir string, entry ModEntry, goos string, client *http.Client, onProgress func(downloaded, total int64)) error {
	if strings.TrimSpace(outputDir) == "" {
		return fmt.Errorf("game output directory is not set")
	}
	assetURL, fileName, ok := entry.AssetForOS(goos)
	if !ok {
		return fmt.Errorf("mod %s has no asset configured for %s", entry.Name, goos)
	}
	if ctx == nil {
		ctx = context.Background()
	}
	if client == nil {
		client = &http.Client{Timeout: 5 * time.Minute}
	}

	modsDir := ModsDir(outputDir)
	if err := os.MkdirAll(modsDir, 0755); err != nil {
		return fmt.Errorf("creating mods folder: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, assetURL, nil)
	if err != nil {
		return fmt.Errorf("invalid mod URL: %w", err)
	}
	req.Header.Set("User-Agent", "SplatPC-Builder")

	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("downloading %s: %w", entry.Name, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("downloading %s: HTTP %s", entry.Name, resp.Status)
	}

	tmpFile, err := os.CreateTemp(modsDir, ".download-*.tmp")
	if err != nil {
		return fmt.Errorf("creating temporary file: %w", err)
	}
	tmpPath := tmpFile.Name()
	cleanupTmp := true
	defer func() {
		_ = tmpFile.Close()
		if cleanupTmp {
			_ = os.Remove(tmpPath)
		}
	}()

	pr := &progressReader{
		r:          resp.Body,
		total:      resp.ContentLength,
		onProgress: onProgress,
	}
	written, err := io.Copy(tmpFile, pr)
	if err != nil {
		return fmt.Errorf("downloading %s: %w", entry.Name, err)
	}
	if err := tmpFile.Close(); err != nil {
		return fmt.Errorf("closing temporary file: %w", err)
	}
	if written == 0 {
		return fmt.Errorf("downloaded file for %s was empty", entry.Name)
	}

	destPath := filepath.Join(modsDir, fileName)
	urlPath := strings.ToLower(strings.Split(strings.Split(assetURL, "?")[0], "#")[0])

	switch {
	case strings.HasSuffix(urlPath, ".zip"):
		if err := extractZipSafe(tmpPath, modsDir); err != nil {
			return fmt.Errorf("extracting %s zip: %w", entry.Name, err)
		}
	case strings.HasSuffix(urlPath, ".tar.gz") || strings.HasSuffix(urlPath, ".tgz"):
		if err := extractTarGzSafe(tmpPath, modsDir); err != nil {
			return fmt.Errorf("extracting %s archive: %w", entry.Name, err)
		}
	default:
		if goos != "windows" {
			_ = os.Chmod(tmpPath, 0755)
		}
		if err := os.Rename(tmpPath, destPath); err != nil {
			return fmt.Errorf("installing %s (close Splatoon if running): %w", fileName, err)
		}
		cleanupTmp = false
	}

	info, err := os.Stat(destPath)
	if err != nil || info.IsDir() || info.Size() == 0 {
		return fmt.Errorf("expected mod binary %s was not found after installation", fileName)
	}

	if err := recordInstalledVersion(outputDir, entry.ID, entry.Version); err != nil {
		return fmt.Errorf("recording mod version: %w", err)
	}
	return nil
}

// Uninstall removes entry's plugin library from <outputDir>/mods/.
func Uninstall(outputDir string, entry ModEntry) error {
	p, ok := BinaryPath(outputDir, entry)
	if !ok {
		return fmt.Errorf("mod %s has no binary configured for %s", entry.Name, runtime.GOOS)
	}
	if err := os.Remove(p); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("removing %s (close Splatoon if running): %w", filepath.Base(p), err)
	}
	return removeInstalledVersion(outputDir, entry.ID)
}

func isSafeExtractPath(destDir, target string) error {
	cleanDest := filepath.Clean(destDir)
	cleanTarget := filepath.Clean(target)
	rel, err := filepath.Rel(cleanDest, cleanTarget)
	if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) || filepath.IsAbs(rel) {
		return fmt.Errorf("unsafe archive path %q", target)
	}
	return nil
}

func extractZipSafe(zipPath, destDir string) error {
	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer r.Close()

	for _, f := range r.File {
		if f.FileInfo().IsDir() {
			continue
		}
		target := filepath.Join(destDir, filepath.FromSlash(f.Name))
		if err := isSafeExtractPath(destDir, target); err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		err = writeFileFromReader(target, rc, f.Mode())
		_ = rc.Close()
		if err != nil {
			return err
		}
	}
	return nil
}

func extractTarGzSafe(tarPath, destDir string) error {
	f, err := os.Open(tarPath)
	if err != nil {
		return err
	}
	defer f.Close()

	gz, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	defer gz.Close()

	tr := tar.NewReader(gz)
	for {
		hdr, err := tr.Next()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return err
		}
		target := filepath.Join(destDir, filepath.FromSlash(hdr.Name))
		if err := isSafeExtractPath(destDir, target); err != nil {
			return err
		}
		switch hdr.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			if err := writeFileFromReader(target, tr, os.FileMode(hdr.Mode)); err != nil {
				return err
			}
		default:
			return fmt.Errorf("unsupported tar entry type %v for %s", hdr.Typeflag, hdr.Name)
		}
	}
	return nil
}

func writeFileFromReader(target string, r io.Reader, mode os.FileMode) error {
	tmp, err := os.CreateTemp(filepath.Dir(target), ".extract-*.tmp")
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

	if _, err := io.Copy(tmp, r); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if mode != 0 && runtime.GOOS != "windows" {
		_ = os.Chmod(tmpPath, mode&0777)
	}
	if err := os.Rename(tmpPath, target); err != nil {
		return err
	}
	cleanup = false
	return nil
}

func loadInstalledManifest(outputDir string) map[string]string {
	out := make(map[string]string)
	data, err := os.ReadFile(filepath.Join(ModsDir(outputDir), manifestFileName))
	if err != nil {
		return out
	}
	_ = json.Unmarshal(data, &out)
	if out == nil {
		return make(map[string]string)
	}
	return out
}

func saveInstalledManifest(outputDir string, versions map[string]string) error {
	modsDir := ModsDir(outputDir)
	if err := os.MkdirAll(modsDir, 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(versions, "", "  ")
	if err != nil {
		return err
	}
	dest := filepath.Join(modsDir, manifestFileName)
	tmp, err := os.CreateTemp(modsDir, ".manifest-*.tmp")
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
	if _, err := tmp.Write(data); err != nil {
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

func recordInstalledVersion(outputDir, id, version string) error {
	versions := loadInstalledManifest(outputDir)
	versions[id] = version
	return saveInstalledManifest(outputDir, versions)
}

func removeInstalledVersion(outputDir, id string) error {
	versions := loadInstalledManifest(outputDir)
	if _, ok := versions[id]; !ok {
		return nil
	}
	delete(versions, id)
	return saveInstalledManifest(outputDir, versions)
}

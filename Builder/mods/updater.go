package mods

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const defaultGitHubAPIBase = "https://api.github.com"

// UpdateInfo describes the latest available release for a catalog mod.
type UpdateInfo struct {
	ModID            string
	InstalledVersion string
	LatestVersion    string
	AssetURL         string
	ReleaseURL       string
	UpdateAvailable  bool
}

type githubRelease struct {
	TagName string `json:"tag_name"`
	HTMLURL string `json:"html_url"`
	Assets  []struct {
		Name               string `json:"name"`
		BrowserDownloadURL string `json:"browser_download_url"`
	} `json:"assets"`
}

// IsNewerVersion returns true when latest represents a newer version than installed.
func IsNewerVersion(installed, latest string) bool {
	inst := strings.TrimSpace(installed)
	lat := strings.TrimSpace(latest)
	if lat == "" || inst == "" {
		return false
	}
	if strings.EqualFold(inst, lat) {
		return false
	}
	instParts, instPre, instOK := parseSemver(inst)
	latParts, latPre, latOK := parseSemver(lat)
	if instOK && latOK {
		maxLen := len(instParts)
		if len(latParts) > maxLen {
			maxLen = len(latParts)
		}
		for i := 0; i < maxLen; i++ {
			var a, b int
			if i < len(instParts) {
				a = instParts[i]
			}
			if i < len(latParts) {
				b = latParts[i]
			}
			if b != a {
				return b > a
			}
		}
		if instPre && !latPre {
			return true
		}
		return false
	}
	return inst != lat
}

func parseSemver(v string) ([]int, bool, bool) {
	v = strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(v), "v"), "V")
	if plusIdx := strings.IndexByte(v, '+'); plusIdx >= 0 {
		v = v[:plusIdx]
	}
	hasPre := false
	if dashIdx := strings.IndexByte(v, '-'); dashIdx >= 0 {
		hasPre = true
		v = v[:dashIdx]
	}
	if v == "" {
		return nil, false, false
	}
	raw := strings.Split(v, ".")
	parts := make([]int, 0, len(raw))
	for _, s := range raw {
		n, err := strconv.Atoi(s)
		if err != nil {
			return nil, false, false
		}
		parts = append(parts, n)
	}
	return parts, hasPre, true
}

// CheckUpdate queries GitHub Releases for the latest release of entry.
func CheckUpdate(ctx context.Context, outputDir string, entry ModEntry) (UpdateInfo, error) {
	return checkUpdateWithAPI(ctx, defaultGitHubAPIBase, outputDir, entry, runtime.GOOS, nil)
}

func checkUpdateWithAPI(ctx context.Context, apiBaseURL, outputDir string, entry ModEntry, goos string, client *http.Client) (UpdateInfo, error) {
	info := UpdateInfo{
		ModID:            entry.ID,
		InstalledVersion: InstalledVersion(outputDir, entry),
		LatestVersion:    entry.Version,
		ReleaseURL:       entry.HomepageURL,
	}
	assetURL, fileName, ok := entry.AssetForOS(goos)
	if ok {
		info.AssetURL = assetURL
	}

	if strings.TrimSpace(entry.GitHubRepo) == "" {
		info.UpdateAvailable = IsInstalled(outputDir, entry) && IsNewerVersion(info.InstalledVersion, info.LatestVersion)
		return info, nil
	}
	if ctx == nil {
		ctx = context.Background()
	}
	if client == nil {
		client = &http.Client{Timeout: 15 * time.Second}
	}

	endpoint := strings.TrimRight(apiBaseURL, "/") + "/repos/" + strings.TrimSpace(entry.GitHubRepo) + "/releases/latest"
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return info, err
	}
	req.Header.Set("User-Agent", "SplatPC-Builder")
	req.Header.Set("Accept", "application/vnd.github+json")

	resp, err := client.Do(req)
	if err != nil {
		return info, fmt.Errorf("checking updates for %s: %w", entry.Name, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return info, fmt.Errorf("checking updates for %s: HTTP %s", entry.Name, resp.Status)
	}

	var rel githubRelease
	if err := json.NewDecoder(resp.Body).Decode(&rel); err != nil {
		return info, fmt.Errorf("parsing release info for %s: %w", entry.Name, err)
	}

	if strings.TrimSpace(rel.TagName) != "" {
		info.LatestVersion = strings.TrimSpace(rel.TagName)
	}
	if strings.TrimSpace(rel.HTMLURL) != "" {
		info.ReleaseURL = strings.TrimSpace(rel.HTMLURL)
	}

	if matchedURL := matchReleaseAsset(rel, fileName, goos); matchedURL != "" {
		info.AssetURL = matchedURL
	} else if !strings.EqualFold(info.LatestVersion, entry.Version) {
		info.AssetURL = ""
	}

	if IsInstalled(outputDir, entry) && info.InstalledVersion != "" && info.AssetURL != "" {
		info.UpdateAvailable = IsNewerVersion(info.InstalledVersion, info.LatestVersion)
	}
	return info, nil
}

func matchReleaseAsset(rel githubRelease, fileName, goos string) string {
	for _, a := range rel.Assets {
		if strings.EqualFold(a.Name, fileName) && a.BrowserDownloadURL != "" {
			return a.BrowserDownloadURL
		}
	}
	for _, a := range rel.Assets {
		lower := strings.ToLower(a.Name)
		if a.BrowserDownloadURL == "" {
			continue
		}
		if goos == "windows" && strings.HasSuffix(lower, ".zip") {
			return a.BrowserDownloadURL
		}
		if goos == "linux" && (strings.HasSuffix(lower, ".tar.gz") || strings.HasSuffix(lower, ".tgz")) {
			return a.BrowserDownloadURL
		}
	}
	return ""
}

// InstallUpdate downloads and installs the release described by info.
func InstallUpdate(ctx context.Context, outputDir string, entry ModEntry, info UpdateInfo, onProgress func(downloaded, total int64)) error {
	return installUpdateForOS(ctx, outputDir, entry, info, runtime.GOOS, nil, onProgress)
}

func installUpdateForOS(ctx context.Context, outputDir string, entry ModEntry, info UpdateInfo, goos string, client *http.Client, onProgress func(downloaded, total int64)) error {
	updated := entry
	if strings.TrimSpace(info.LatestVersion) != "" {
		updated.Version = strings.TrimSpace(info.LatestVersion)
	}
	if strings.TrimSpace(info.ReleaseURL) != "" {
		updated.HomepageURL = strings.TrimSpace(info.ReleaseURL)
	}
	if strings.TrimSpace(info.AssetURL) != "" {
		switch goos {
		case "windows":
			updated.WindowsAssetURL = strings.TrimSpace(info.AssetURL)
		case "linux":
			updated.LinuxAssetURL = strings.TrimSpace(info.AssetURL)
		}
	}
	return installForOS(ctx, outputDir, updated, goos, client, onProgress)
}

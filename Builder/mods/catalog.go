package mods

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// ModEntry describes a downloadable mod compatible with Splatoon-modapi.
// Adding a new curated mod is as simple as appending an entry to Catalog().
type ModEntry struct {
	ID              string `json:"id"`
	Name            string `json:"name"`
	Author          string `json:"author"`
	Version         string `json:"version"`
	Description     string `json:"description"`
	HomepageURL     string `json:"homepage_url"`
	GitHubRepo      string `json:"github_repo,omitempty"`
	WindowsAssetURL string `json:"windows_asset_url"`
	LinuxAssetURL   string `json:"linux_asset_url"`
	WindowsFileName string `json:"windows_file_name"`
	LinuxFileName   string `json:"linux_file_name"`
}

// AssetForOS returns the download URL and installed binary filename for goos.
func (m ModEntry) AssetForOS(goos string) (url string, fileName string, ok bool) {
	switch goos {
	case "windows":
		return m.WindowsAssetURL, m.WindowsFileName, m.WindowsAssetURL != "" && m.WindowsFileName != ""
	case "linux":
		return m.LinuxAssetURL, m.LinuxFileName, m.LinuxAssetURL != "" && m.LinuxFileName != ""
	default:
		return "", "", false
	}
}

// Catalog returns the curated list of downloadable SplatPC mods.
func Catalog() []ModEntry {
	return []ModEntry{
		{
			ID:              "splatpc.online_coop",
			Name:            "Online Co-op",
			Author:          "Idostuffman",
			Version:         "v1.0.1",
			Description:     "2-player online co-op and Battle Dojo netplay.",
			HomepageURL:     "https://github.com/Idostuffman/SplatPC-Online-Coop/releases/tag/v1.0.1",
			GitHubRepo:      "Idostuffman/SplatPC-Online-Coop",
			WindowsAssetURL: "https://github.com/Idostuffman/SplatPC-Online-Coop/releases/download/v1.0.1/online_coop.dll",
			LinuxAssetURL:   "https://github.com/Idostuffman/SplatPC-Online-Coop/releases/download/v1.0.1/online_coop.so",
			WindowsFileName: "online_coop.dll",
			LinuxFileName:   "online_coop.so",
		},
	}
}

// DefaultExtrasPath returns the path to an optional mods_catalog.json file next to the Builder executable.
func DefaultExtrasPath() string {
	exe, err := os.Executable()
	if err != nil || exe == "" {
		return ""
	}
	return filepath.Join(filepath.Dir(exe), "mods_catalog.json")
}

// CatalogWithExtras returns the built-in Catalog() merged with any valid entries
// loaded from extraPath (if present). Entries with matching IDs update existing ones.
func CatalogWithExtras(extraPath string) []ModEntry {
	entries := Catalog()
	if strings.TrimSpace(extraPath) == "" {
		return entries
	}
	data, err := os.ReadFile(extraPath)
	if err != nil {
		return entries
	}

	var extraList []ModEntry
	if err := json.Unmarshal(data, &extraList); err != nil {
		var wrapper struct {
			Mods []ModEntry `json:"mods"`
		}
		if err2 := json.Unmarshal(data, &wrapper); err2 != nil {
			return entries
		}
		extraList = wrapper.Mods
	}

	indexByID := make(map[string]int, len(entries))
	for i, e := range entries {
		indexByID[e.ID] = i
	}
	for _, extra := range extraList {
		if strings.TrimSpace(extra.ID) == "" || strings.TrimSpace(extra.Name) == "" {
			continue
		}
		if idx, exists := indexByID[extra.ID]; exists {
			entries[idx] = extra
		} else {
			indexByID[extra.ID] = len(entries)
			entries = append(entries, extra)
		}
	}
	return entries
}

// FindByID looks up a mod in the built-in Catalog() by its ID.
func FindByID(id string) (ModEntry, bool) {
	return FindByIDInCatalog(Catalog(), id)
}

// FindByIDInCatalog looks up a mod by ID in the supplied slice.
func FindByIDInCatalog(entries []ModEntry, id string) (ModEntry, bool) {
	for _, e := range entries {
		if strings.EqualFold(e.ID, strings.TrimSpace(id)) {
			return e, true
		}
	}
	return ModEntry{}, false
}

// BinaryName returns the expected mod library filename for the current OS.
func (m ModEntry) BinaryName() (string, bool) {
	_, name, ok := m.AssetForOS(runtime.GOOS)
	return name, ok
}

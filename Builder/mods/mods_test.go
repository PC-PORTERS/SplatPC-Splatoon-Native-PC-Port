package mods

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestCatalogContainsOnlineCoop(t *testing.T) {
	entries := Catalog()
	if len(entries) == 0 {
		t.Fatal("Catalog() should not be empty")
	}
	coop, ok := FindByID("splatpc.online_coop")
	if !ok {
		t.Fatal("expected splatpc.online_coop in Catalog()")
	}
	if coop.Version != "v1.0.1" {
		t.Fatalf("expected Online Co-op version v1.0.1, got %q", coop.Version)
	}
	if coop.HomepageURL != "https://github.com/Idostuffman/SplatPC-Online-Coop/releases/tag/v1.0.1" {
		t.Fatalf("unexpected homepage URL: %q", coop.HomepageURL)
	}
	if coop.WindowsFileName != "online_coop.dll" || coop.LinuxFileName != "online_coop.so" {
		t.Fatalf("unexpected filenames: win=%q linux=%q", coop.WindowsFileName, coop.LinuxFileName)
	}
}

func TestInstallAndUninstallDirectBinary(t *testing.T) {
	payload := []byte("MZ-dummy-mod-binary")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(payload)
	}))
	defer srv.Close()

	entry := ModEntry{
		ID:              "test.mod",
		Name:            "Test Mod",
		Version:         "v1.2.3",
		WindowsAssetURL: srv.URL + "/test_mod.dll",
		LinuxAssetURL:   srv.URL + "/test_mod.so",
		WindowsFileName: "test_mod.dll",
		LinuxFileName:   "test_mod.so",
	}

	outDir := t.TempDir()
	var lastDownloaded int64
	if err := installForOS(context.Background(), outDir, entry, runtime.GOOS, srv.Client(), func(downloaded, total int64) {
		lastDownloaded = downloaded
	}); err != nil {
		t.Fatalf("Install failed: %v", err)
	}
	if lastDownloaded != int64(len(payload)) {
		t.Fatalf("expected progress %d bytes, got %d", len(payload), lastDownloaded)
	}
	if !IsInstalled(outDir, entry) {
		t.Fatal("expected mod to report installed")
	}
	if gotVer := InstalledVersion(outDir, entry); gotVer != "v1.2.3" {
		t.Fatalf("expected version v1.2.3, got %q", gotVer)
	}

	binPath, ok := BinaryPath(outDir, entry)
	if !ok {
		t.Fatal("BinaryPath returned false")
	}
	gotBytes, err := os.ReadFile(binPath)
	if err != nil || !bytes.Equal(gotBytes, payload) {
		t.Fatalf("installed payload mismatch: err=%v bytes=%q", err, string(gotBytes))
	}

	if err := Uninstall(outDir, entry); err != nil {
		t.Fatalf("Uninstall failed: %v", err)
	}
	if IsInstalled(outDir, entry) {
		t.Fatal("expected mod to report uninstalled after Uninstall")
	}
	if gotVer := InstalledVersion(outDir, entry); gotVer != "" {
		t.Fatalf("expected empty version after uninstall, got %q", gotVer)
	}
}

func TestInstallZipAndRejectsZipSlip(t *testing.T) {
	makeZip := func(entries map[string]string) []byte {
		var buf bytes.Buffer
		zw := zip.NewWriter(&buf)
		for name, content := range entries {
			w, err := zw.Create(name)
			if err != nil {
				t.Fatal(err)
			}
			_, _ = w.Write([]byte(content))
		}
		if err := zw.Close(); err != nil {
			t.Fatal(err)
		}
		return buf.Bytes()
	}

	validZip := makeZip(map[string]string{
		"good_mod.dll": "zip-dll-content",
		"good_mod.so":  "zip-so-content",
		"README.md":    "readme",
	})
	evilZip := makeZip(map[string]string{
		"../escaped.dll": "evil",
	})

	mode := "valid"
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if mode == "evil" {
			_, _ = w.Write(evilZip)
			return
		}
		_, _ = w.Write(validZip)
	}))
	defer srv.Close()

	entry := ModEntry{
		ID:              "test.zipmod",
		Name:            "Zip Mod",
		Version:         "v2.0.0",
		WindowsAssetURL: srv.URL + "/mod.zip",
		LinuxAssetURL:   srv.URL + "/mod.zip",
		WindowsFileName: "good_mod.dll",
		LinuxFileName:   "good_mod.so",
	}

	outDir := t.TempDir()
	if err := installForOS(context.Background(), outDir, entry, runtime.GOOS, srv.Client(), nil); err != nil {
		t.Fatalf("valid zip install failed: %v", err)
	}
	if !IsInstalled(outDir, entry) {
		t.Fatal("expected zip mod to be installed")
	}
	if _, err := os.Stat(filepath.Join(ModsDir(outDir), "README.md")); err != nil {
		t.Fatalf("expected README.md from zip: %v", err)
	}

	mode = "evil"
	evilDir := t.TempDir()
	err := installForOS(context.Background(), evilDir, entry, runtime.GOOS, srv.Client(), nil)
	if err == nil || !strings.Contains(err.Error(), "unsafe archive path") {
		t.Fatalf("expected unsafe archive path error, got: %v", err)
	}
	if _, statErr := os.Stat(filepath.Join(evilDir, "escaped.dll")); !os.IsNotExist(statErr) {
		t.Fatal("zip-slip file must not be created")
	}
}

func TestInstallTarGzAndRejectsTraversal(t *testing.T) {
	makeTarGz := func(entries map[string]string) []byte {
		var buf bytes.Buffer
		gw := gzip.NewWriter(&buf)
		tw := tar.NewWriter(gw)
		for name, content := range entries {
			hdr := &tar.Header{
				Name: name,
				Mode: 0644,
				Size: int64(len(content)),
			}
			if err := tw.WriteHeader(hdr); err != nil {
				t.Fatal(err)
			}
			if _, err := tw.Write([]byte(content)); err != nil {
				t.Fatal(err)
			}
		}
		if err := tw.Close(); err != nil {
			t.Fatal(err)
		}
		if err := gw.Close(); err != nil {
			t.Fatal(err)
		}
		return buf.Bytes()
	}

	validTar := makeTarGz(map[string]string{
		"tar_mod.dll": "tar-dll-content",
		"tar_mod.so":  "tar-so-content",
	})
	evilTar := makeTarGz(map[string]string{
		"../../outside.dll": "evil",
	})

	mode := "valid"
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if mode == "evil" {
			_, _ = w.Write(evilTar)
			return
		}
		_, _ = w.Write(validTar)
	}))
	defer srv.Close()

	entry := ModEntry{
		ID:              "test.tarmod",
		Name:            "Tar Mod",
		Version:         "v3.0.0",
		WindowsAssetURL: srv.URL + "/mod.tar.gz",
		LinuxAssetURL:   srv.URL + "/mod.tar.gz",
		WindowsFileName: "tar_mod.dll",
		LinuxFileName:   "tar_mod.so",
	}

	outDir := t.TempDir()
	if err := installForOS(context.Background(), outDir, entry, runtime.GOOS, srv.Client(), nil); err != nil {
		t.Fatalf("valid tar.gz install failed: %v", err)
	}
	if !IsInstalled(outDir, entry) {
		t.Fatal("expected tar.gz mod to be installed")
	}

	mode = "evil"
	evilDir := t.TempDir()
	err := installForOS(context.Background(), evilDir, entry, runtime.GOOS, srv.Client(), nil)
	if err == nil || !strings.Contains(err.Error(), "unsafe archive path") {
		t.Fatalf("expected unsafe archive path error, got: %v", err)
	}
}

func TestInstallHTTPErrorCleansUp(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "not found", http.StatusNotFound)
	}))
	defer srv.Close()

	entry := ModEntry{
		ID:              "test.fail",
		Name:            "Fail Mod",
		Version:         "v0.1.0",
		WindowsAssetURL: srv.URL + "/missing.dll",
		LinuxAssetURL:   srv.URL + "/missing.so",
		WindowsFileName: "missing.dll",
		LinuxFileName:   "missing.so",
	}
	outDir := t.TempDir()
	err := installForOS(context.Background(), outDir, entry, runtime.GOOS, srv.Client(), nil)
	if err == nil {
		t.Fatal("expected HTTP 404 to fail")
	}
	if IsInstalled(outDir, entry) {
		t.Fatal("failed install must not leave binary installed")
	}
}

func TestCatalogWithExtras(t *testing.T) {
	extraPath := filepath.Join(t.TempDir(), "mods_catalog.json")
	jsonData := `[
		{"id": "custom.mod", "name": "Custom Mod", "version": "v0.9.0", "windows_asset_url": "https://example.com/c.dll", "windows_file_name": "c.dll"}
	]`
	if err := os.WriteFile(extraPath, []byte(jsonData), 0644); err != nil {
		t.Fatal(err)
	}
	merged := CatalogWithExtras(extraPath)
	if _, ok := FindByIDInCatalog(merged, "splatpc.online_coop"); !ok {
		t.Fatal("built-in Online Co-op missing from merged catalog")
	}
	if custom, ok := FindByIDInCatalog(merged, "custom.mod"); !ok || custom.Name != "Custom Mod" {
		t.Fatalf("custom mod missing or invalid in merged catalog: %+v", custom)
	}
}

func TestIsNewerVersion(t *testing.T) {
	cases := []struct {
		installed string
		latest    string
		want      bool
	}{
		{"v1.0.1", "v1.0.2", true},
		{"1.0.1", "v1.1.0", true},
		{"v1.0.1-beta", "v1.0.1", true},
		{"v1.0.1", "v1.0.1", false},
		{"v1.2.0", "v1.0.9", false},
		{"", "v1.0.1", false},
	}
	for _, tc := range cases {
		if got := IsNewerVersion(tc.installed, tc.latest); got != tc.want {
			t.Errorf("IsNewerVersion(%q, %q) = %v, want %v", tc.installed, tc.latest, got, tc.want)
		}
	}
}

func TestCheckUpdateAndInstallUpdate(t *testing.T) {
	var srvURL string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/repos/org/mod/releases/latest":
			_ = json.NewEncoder(w).Encode(map[string]any{
				"tag_name": "v2.0.0",
				"html_url": "https://example.com/releases/v2.0.0",
				"assets": []map[string]string{
					{"name": "demo.dll", "browser_download_url": srvURL + "/dl/v2/demo.dll"},
					{"name": "demo.so", "browser_download_url": srvURL + "/dl/v2/demo.so"},
				},
			})
		case "/dl/v1/demo.dll", "/dl/v1/demo.so":
			_, _ = w.Write([]byte("v1-binary"))
		case "/dl/v2/demo.dll", "/dl/v2/demo.so":
			_, _ = w.Write([]byte("v2-binary"))
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()
	srvURL = srv.URL

	entry := ModEntry{
		ID:              "org.demo",
		Name:            "Demo Mod",
		Version:         "v1.0.0",
		GitHubRepo:      "org/mod",
		WindowsAssetURL: srv.URL + "/dl/v1/demo.dll",
		LinuxAssetURL:   srv.URL + "/dl/v1/demo.so",
		WindowsFileName: "demo.dll",
		LinuxFileName:   "demo.so",
	}

	outDir := t.TempDir()
	if err := installForOS(context.Background(), outDir, entry, runtime.GOOS, srv.Client(), nil); err != nil {
		t.Fatalf("v1 install failed: %v", err)
	}

	info, err := checkUpdateWithAPI(context.Background(), srv.URL, outDir, entry, runtime.GOOS, srv.Client())
	if err != nil {
		t.Fatalf("checkUpdateWithAPI failed: %v", err)
	}
	if !info.UpdateAvailable || info.LatestVersion != "v2.0.0" {
		t.Fatalf("expected update v2.0.0 available, got %+v", info)
	}

	if err := installUpdateForOS(context.Background(), outDir, entry, info, runtime.GOOS, srv.Client(), nil); err != nil {
		t.Fatalf("installUpdateForOS failed: %v", err)
	}
	if got := InstalledVersion(outDir, entry); got != "v2.0.0" {
		t.Fatalf("expected installed version v2.0.0, got %q", got)
	}
	binPath, _ := BinaryPath(outDir, entry)
	data, err := os.ReadFile(binPath)
	if err != nil || string(data) != "v2-binary" {
		t.Fatalf("expected v2-binary, got err=%v content=%q", err, string(data))
	}
}

func TestModStateIniRoundTrip(t *testing.T) {
	outDir := t.TempDir()
	if err := SetModDisabled(outDir, "alpha.mod", true); err != nil {
		t.Fatalf("SetModDisabled(true) failed: %v", err)
	}
	if !LoadDisabledMods(outDir)["alpha.mod"] {
		t.Fatal("expected alpha.mod to be marked disabled")
	}
	if err := SetModDisabled(outDir, "alpha.mod", false); err != nil {
		t.Fatalf("SetModDisabled(false) failed: %v", err)
	}
	if LoadDisabledMods(outDir)["alpha.mod"] {
		t.Fatal("expected alpha.mod to be re-enabled")
	}
}

func TestListInstalledFilesDeleteAndInstallLocal(t *testing.T) {
	outDir := t.TempDir()
	srcDir := t.TempDir()
	ext := pluginExtensionForOS(runtime.GOOS)
	localSrc := filepath.Join(srcDir, "custom_local"+ext)
	if err := os.WriteFile(localSrc, []byte("local-plugin-bytes"), 0644); err != nil {
		t.Fatal(err)
	}

	added, err := InstallLocalFile(outDir, localSrc)
	if err != nil {
		t.Fatalf("InstallLocalFile failed: %v", err)
	}
	if added != "custom_local"+ext {
		t.Fatalf("unexpected installed filename %q", added)
	}

	files, err := ListInstalledFiles(outDir, Catalog())
	if err != nil {
		t.Fatalf("ListInstalledFiles failed: %v", err)
	}
	if len(files) != 1 || files[0].FileName != "custom_local"+ext {
		t.Fatalf("unexpected listed files: %+v", files)
	}

	if err := DeleteFile(outDir, "../escape"+ext, Catalog()); err == nil {
		t.Fatal("expected DeleteFile to reject path traversal")
	}
	if err := DeleteFile(outDir, "custom_local"+ext, Catalog()); err != nil {
		t.Fatalf("DeleteFile failed: %v", err)
	}
	files, _ = ListInstalledFiles(outDir, Catalog())
	if len(files) != 0 {
		t.Fatalf("expected mods dir empty after DeleteFile, got %+v", files)
	}
}

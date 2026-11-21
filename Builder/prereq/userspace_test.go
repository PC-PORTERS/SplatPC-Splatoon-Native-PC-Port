package prereq

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestToolSpecsCoverSupportedPlatforms(t *testing.T) {
	for _, tc := range []struct {
		goos, goarch string
		wantIDs      []string
	}{
		{"linux", "amd64", []string{"cmake", "ninja", "python3", "compiler", "appimagetool"}},
		{"linux", "arm64", []string{"cmake", "ninja", "python3", "compiler", "appimagetool"}},
		{"windows", "amd64", []string{"cmake", "ninja", "python3", "compiler"}},
		{"windows", "arm64", []string{"cmake", "ninja", "python3", "compiler"}},
	} {
		specs := ToolSpecsFor(tc.goos, tc.goarch)
		for _, id := range tc.wantIDs {
			spec, ok := specs[id]
			if !ok {
				t.Fatalf("%s/%s missing spec for %s", tc.goos, tc.goarch, id)
			}
			if !strings.HasPrefix(spec.URL, "https://") {
				t.Errorf("%s/%s %s URL must be HTTPS: %q", tc.goos, tc.goarch, id, spec.URL)
			}
			if len(spec.SHA256) != 64 {
				t.Errorf("%s/%s %s expected 64-char SHA-256, got %q", tc.goos, tc.goarch, id, spec.SHA256)
			}
		}
	}
}

func TestOrderToolIDsPlacesPythonBeforeCompiler(t *testing.T) {
	got := orderToolIDs([]string{"appimagetool", "compiler", "ninja", "cmake", "python3"})
	want := []string{"python3", "cmake", "ninja", "compiler", "appimagetool"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("orderToolIDs = %v, want %v", got, want)
		}
	}
}

func TestDownloadableMissingToolsFiltersFoundItems(t *testing.T) {
	report := CheckReport{
		Items: []CheckItem{
			{ID: "cmake", Found: true},
			{ID: "python3", Found: false},
			{ID: "ninja", Found: false},
			{ID: "display_headers", Found: false},
		},
	}
	got := DownloadableMissingTools(report)
	if len(got) != 2 || got[0] != "python3" || got[1] != "ninja" {
		t.Fatalf("DownloadableMissingTools = %v, expected [python3 ninja]", got)
	}
}

func TestInstallAndRemoveUserspaceArchiveExtraction(t *testing.T) {
	toolsDir := t.TempDir()
	t.Setenv("SPLATPC_TOOLS_DIR", toolsDir)

	var tarBuf bytes.Buffer
	gzw := gzip.NewWriter(&tarBuf)
	tw := tar.NewWriter(gzw)
	writeTarEntry(t, tw, "cmake-4.5.1/bin/cmake", []byte("#!/bin/sh\necho cmake\n"), 0755)
	writeTarEntry(t, tw, "cmake-4.5.1/../../outside.txt", []byte("bad"), 0644)
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := gzw.Close(); err != nil {
		t.Fatal(err)
	}
	tarBytes := tarBuf.Bytes()
	tarSum := sha256Hex(tarBytes)

	var zipBuf bytes.Buffer
	zw := zip.NewWriter(&zipBuf)
	writeZipEntry(t, zw, "ninja", []byte("#!/bin/sh\necho ninja\n"))
	writeZipEntry(t, zw, "../escape.txt", []byte("bad"))
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	zipBytes := zipBuf.Bytes()
	zipSum := sha256Hex(zipBytes)

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/cmake.tar.gz":
			_, _ = w.Write(tarBytes)
		case "/ninja.zip":
			_, _ = w.Write(zipBytes)
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()

	downloads := filepath.Join(toolsDir, "downloads")
	if err := os.MkdirAll(downloads, 0755); err != nil {
		t.Fatal(err)
	}

	cmakeSpec := ToolSpec{
		ID: "cmake", Name: "CMake", Version: "4.5.1",
		URL: srv.URL + "/cmake.tar.gz", ArchiveName: "cmake.tar.gz",
		SHA256: tarSum, ArchiveType: "tar.gz", InstallDir: "cmake", StripTopDir: true,
	}
	if err := installOneTool(context.Background(), toolsDir, downloads, cmakeSpec, func(string, ...interface{}) {}); err != nil {
		t.Fatalf("installOneTool cmake failed: %v", err)
	}

	ninjaSpec := ToolSpec{
		ID: "ninja", Name: "Ninja", Version: "1.13.2",
		URL: srv.URL + "/ninja.zip", ArchiveName: "ninja.zip",
		SHA256: zipSum, ArchiveType: "zip", InstallDir: filepath.Join("ninja", "bin"), StripTopDir: false,
	}
	if err := installOneTool(context.Background(), toolsDir, downloads, ninjaSpec, func(string, ...interface{}) {}); err != nil {
		t.Fatalf("installOneTool ninja failed: %v", err)
	}

	if _, err := os.Stat(filepath.Join(toolsDir, "cmake", "bin", "cmake")); err != nil {
		t.Errorf("expected extracted cmake binary: %v", err)
	}
	if _, err := os.Stat(filepath.Join(toolsDir, "ninja", "bin", "ninja")); err != nil {
		t.Errorf("expected extracted ninja binary: %v", err)
	}
	if _, err := os.Stat(filepath.Join(toolsDir, "outside.txt")); !os.IsNotExist(err) {
		t.Errorf("path traversal file outside.txt should not exist")
	}
	if _, err := os.Stat(filepath.Join(toolsDir, "ninja", "escape.txt")); !os.IsNotExist(err) {
		t.Errorf("path traversal file escape.txt should not exist")
	}

	if runtime.GOOS != "windows" {
		if got, ok := LookPath("cmake"); !ok || !isUnderDir(got, toolsDir) {
			t.Errorf("LookPath(cmake) = %q (%v), expected under %s", got, ok, toolsDir)
		}
	}

	badSpec := cmakeSpec
	badSpec.SHA256 = strings.Repeat("0", 64)
	if err := installOneTool(context.Background(), toolsDir, downloads, badSpec, func(string, ...interface{}) {}); err == nil || !strings.Contains(err.Error(), "SHA-256") {
		t.Errorf("expected SHA-256 mismatch error, got %v", err)
	}

	if UserspaceToolsSize() <= 0 {
		t.Errorf("expected positive UserspaceToolsSize()")
	}
	if err := RemoveUserspaceTools(); err != nil {
		t.Fatalf("RemoveUserspaceTools failed: %v", err)
	}
	if UserspaceToolsSize() != 0 {
		t.Errorf("expected UserspaceToolsSize() == 0 after removal")
	}
}

func TestCreateZigWrappers(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX wrapper test")
	}
	root := t.TempDir()
	zigDir := filepath.Join(root, "zig")
	if err := os.MkdirAll(zigDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(zigDir, "zig"), []byte("#!/bin/sh\n"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := createZigWrappers(root, zigDir); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(root, "toolchain", "bin", "c++"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), "-fno-sanitize=undefined") {
		t.Errorf("expected -fno-sanitize=undefined in Zig c++ wrapper, got %q", string(data))
	}
}

func writeTarEntry(t *testing.T, tw *tar.Writer, name string, data []byte, mode int64) {
	t.Helper()
	hdr := &tar.Header{
		Name: name,
		Mode: mode,
		Size: int64(len(data)),
	}
	if err := tw.WriteHeader(hdr); err != nil {
		t.Fatal(err)
	}
	if _, err := tw.Write(data); err != nil {
		t.Fatal(err)
	}
}

func writeZipEntry(t *testing.T, zw *zip.Writer, name string, data []byte) {
	t.Helper()
	w, err := zw.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write(data); err != nil {
		t.Fatal(err)
	}
}

func sha256Hex(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

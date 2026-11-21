package engine

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInstalledDirNeedsTheReleaseStamp(t *testing.T) {
	b := dependencyBuilder{prefixRoot: t.TempDir()}
	dep := volkDependency("")
	cfgDir := filepath.Join(b.prefix(dep), "lib", "cmake", "volk")
	writeDummy(t, filepath.Join(cfgDir, "volkConfig.cmake"))

	// A volk installed by an older Builder may be a Debug build.
	if _, ok := b.installedDir(dep); ok {
		t.Errorf("expected an install without a stamp to be rebuilt")
	}
	stamp := filepath.Join(b.prefix(dep), installStamp)
	if err := os.WriteFile(stamp, []byte("Debug"), 0644); err != nil {
		t.Fatal(err)
	}
	if _, ok := b.installedDir(dep); ok {
		t.Errorf("expected a Debug install to be rebuilt")
	}
	if err := os.WriteFile(stamp, []byte(buildConfig), 0644); err != nil {
		t.Fatal(err)
	}
	if dir, ok := b.installedDir(dep); !ok || dir != cfgDir {
		t.Errorf("expected %s, got %q (found=%v)", cfgDir, dir, ok)
	}
}

func TestVolkUsesTheBundledVulkanHeaders(t *testing.T) {
	options := strings.Join(volkDependency(`C:\deps\vulkan-headers`).options, " ")
	if !strings.Contains(options, `-DVULKAN_HEADERS_INSTALL_DIR=C:\deps\vulkan-headers`) {
		t.Errorf("expected volk to be pointed at the Vulkan headers, got %s", options)
	}
	if strings.Contains(strings.Join(volkDependency("").options, " "), "VULKAN_HEADERS") {
		t.Errorf("expected no Vulkan headers option without bundled headers")
	}
}

func TestBundledLibrariesAreStatic(t *testing.T) {
	winOptions := strings.Join(curlDependencyFor("windows").options, " ")
	for _, want := range []string{"-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DCURL_USE_SCHANNEL=ON"} {
		if !strings.Contains(winOptions, want) {
			t.Errorf("expected %s in the Windows curl options", want)
		}
	}
	linuxOptions := strings.Join(curlDependencyFor("linux").options, " ")
	for _, want := range []string{"-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DCURL_ENABLE_SSL=OFF"} {
		if !strings.Contains(linuxOptions, want) {
			t.Errorf("expected %s in the Linux curl options", want)
		}
	}
}

func TestZlibDefinesOverrideACachedRelease(t *testing.T) {
	defines := strings.Join(zlibDefines("inc", "zlibstatic.lib"), " ")
	for _, want := range []string{"-DZLIB_INCLUDE_DIR=inc", "-DZLIB_LIBRARY=zlibstatic.lib", "-DZLIB_LIBRARY_RELEASE=zlibstatic.lib"} {
		if !strings.Contains(defines, want) {
			t.Errorf("expected %s in %s", want, defines)
		}
	}
}

func TestFindFileFindsStaticZlib(t *testing.T) {
	root := t.TempDir()
	writeDummy(t, filepath.Join(root, "lib", "zlib.lib"))
	lib := filepath.Join(root, "lib", "zlibstatic.lib")
	writeDummy(t, lib)
	if found, ok := findFile(root, zlibStaticNames...); !ok || found != lib {
		t.Errorf("expected %s, got %q (found=%v)", lib, found, ok)
	}
}

func TestCleanBuildDirKeepsDownloads(t *testing.T) {
	buildDir := t.TempDir()
	archive := filepath.Join(buildDir, "downloads", "zlib-1.3.1.tar.gz")
	writeDummy(t, archive)
	writeDummy(t, filepath.Join(buildDir, "game", "CMakeCache.txt"))
	writeDummy(t, filepath.Join(buildDir, "deps", "volk", "lib", "volk.lib"))

	if err := cleanBuildDir(buildDir); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(archive); err != nil {
		t.Errorf("expected the downloaded archive to be kept: %v", err)
	}
	for _, removed := range []string{"game", "deps"} {
		if _, err := os.Stat(filepath.Join(buildDir, removed)); !os.IsNotExist(err) {
			t.Errorf("expected %s to be removed", removed)
		}
	}
	if err := cleanBuildDir(filepath.Join(buildDir, "missing")); err != nil {
		t.Errorf("expected no error for a missing build folder, got %v", err)
	}
}

func TestBuildLogWritesTheFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "build", "build.log")
	var shown []string
	l := openBuildLog(path, func(line string) { shown = append(shown, line) })
	l.printf("=== Step %d/8 ===", 5)
	l.close()

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.TrimSpace(string(data)) != "=== Step 5/8 ===" || len(shown) != 1 {
		t.Errorf("expected the line in the file and the UI, got %q and %v", data, shown)
	}
}

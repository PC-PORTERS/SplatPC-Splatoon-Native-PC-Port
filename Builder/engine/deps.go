package engine

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"splatpc-builder/prereq"
	"splatpc-builder/proc"
)

// sourceDependency is a library the Builder downloads, builds with CMake and
// installs under build/deps/<name>.
type sourceDependency struct {
	name    string
	url     string
	archive string
	options []string
	// installed are file names, one of which is in the prefix once installed.
	installed []string
}

type dependencyBuilder struct {
	ctx        context.Context
	cmake      cmakeProject
	buildDir   string
	downloads  string
	prefixRoot string
	log        logFunc
}

// installStamp marks a prefix the Builder installed with buildConfig, so a
// library left over from a Debug build is built again instead of reused.
const installStamp = ".splatpc-build-config"

func (b dependencyBuilder) cmakeExe() string {
	if b.cmake.exe != "" {
		return b.cmake.exe
	}
	return "cmake"
}

func (b dependencyBuilder) prefix(dep sourceDependency) string {
	return filepath.Join(b.prefixRoot, dep.name)
}

// installedDir returns the directory holding the dependency's installed
// file, if the Builder already installed it with buildConfig.
func (b dependencyBuilder) installedDir(dep sourceDependency) (string, bool) {
	stamp, err := os.ReadFile(filepath.Join(b.prefix(dep), installStamp))
	if err != nil || strings.TrimSpace(string(stamp)) != buildConfig {
		return "", false
	}
	return findConfigDir(b.prefix(dep), dep.installed...)
}

// install builds and installs the dependency unless it already is, and
// returns the directory holding its installed file.
func (b dependencyBuilder) install(dep sourceDependency) (string, error) {
	if dir, ok := b.installedDir(dep); ok {
		return dir, nil
	}
	archive := filepath.Join(b.downloads, dep.archive)
	if _, err := os.Stat(archive); err != nil {
		b.log("Downloading %s...", dep.name)
		if err := downloadFile(dep.url, archive); err != nil {
			return "", fmt.Errorf("downloading %s: %w", dep.name, err)
		}
	}
	source := filepath.Join(b.buildDir, dep.name+"-source")
	if _, err := os.Stat(filepath.Join(source, "CMakeLists.txt")); err != nil {
		_ = os.RemoveAll(source)
		if err := os.MkdirAll(source, 0755); err != nil {
			return "", err
		}
		if err := extractTarGzStrip1(archive, source); err != nil {
			return "", fmt.Errorf("extracting %s: %w", dep.name, err)
		}
	}

	prefix := b.prefix(dep)
	_ = os.RemoveAll(prefix)
	build := filepath.Join(b.buildDir, dep.name+"-build")
	defines := append([]string{
		"-DCMAKE_INSTALL_PREFIX=" + prefix,
		// Lets CMake 4 configure projects that still declare a pre-3.5 minimum.
		"-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
	}, dep.options...)
	configure := proc.CommandContext(b.ctx, b.cmakeExe(), b.cmake.configureArgs(source, build, defines...)...)
	if err := runCmdWithLog(configure, b.log); err != nil {
		return "", fmt.Errorf("configuring %s: %w", dep.name, err)
	}
	buildCmd := proc.CommandContext(b.ctx, b.cmakeExe(), b.cmake.buildArgs(build, "install")...)
	if err := runCmdWithLog(buildCmd, b.log); err != nil {
		return "", fmt.Errorf("building %s: %w", dep.name, err)
	}
	if err := os.WriteFile(filepath.Join(prefix, installStamp), []byte(buildConfig), 0644); err != nil {
		return "", err
	}
	dir, ok := findConfigDir(prefix, dep.installed...)
	if !ok {
		return "", fmt.Errorf("%s was installed to %s but %s is missing", dep.name, prefix, strings.Join(dep.installed, " / "))
	}
	return dir, nil
}

func volkDependency(vulkanHeaders string) sourceDependency {
	options := []string{"-DVOLK_INSTALL=ON"}
	if vulkanHeaders != "" {
		options = append(options, "-DVULKAN_HEADERS_INSTALL_DIR="+vulkanHeaders, "-DCMAKE_PREFIX_PATH="+vulkanHeaders)
	}
	return sourceDependency{
		name:      "volk",
		url:       "https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
		archive:   "volk-1.4.363.tar.gz",
		options:   options,
		installed: []string{"volkConfig.cmake", "volk-config.cmake"},
	}
}

func curlDependencyFor(goos string) sourceDependency {
	opts := []string{
		"-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF",
		"-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF", "-DENABLE_CURL_MANUAL=OFF",
		"-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_ZLIB=OFF",
		"-DCURL_ZSTD=OFF", "-DCURL_BROTLI=OFF", "-DUSE_NGHTTP2=OFF", "-DUSE_LIBIDN2=OFF", "-DCURL_DISABLE_LDAP=ON",
	}
	if goos == "windows" {
		opts = append(opts, "-DCURL_USE_SCHANNEL=ON")
	} else {
		opts = append(opts, "-DCURL_ENABLE_SSL=OFF", "-DHTTP_ONLY=ON")
	}
	return sourceDependency{
		name:      "curl",
		url:       "https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.gz",
		archive:   "curl-8.11.1.tar.gz",
		options:   opts,
		installed: []string{"CURLConfig.cmake"},
	}
}

// Libraries built from official upstream sources in userspace as static
// Release libraries, so rebrewu and the game need no system dev packages or extra DLLs.
var (
	vulkanHeadersDependency = sourceDependency{
		name:      "vulkan-headers",
		url:       "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
		archive:   "Vulkan-Headers-1.4.363.tar.gz",
		options:   []string{"-DVULKAN_HEADERS_ENABLE_TESTS=OFF", "-DVULKAN_HEADERS_ENABLE_MODULE=OFF"},
		installed: []string{"VulkanHeadersConfig.cmake"},
	}
	zlibDependency = sourceDependency{
		name:      "zlib",
		url:       "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
		archive:   "zlib-1.3.1.tar.gz",
		options:   []string{"-DZLIB_BUILD_EXAMPLES=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"},
		installed: []string{"zlib.h"},
	}
	curlDependency = curlDependencyFor(runtime.GOOS)
)

// zlibStaticNames are the static library names zlib's CMake build installs.
var zlibStaticNames = []string{"zlibstatic.lib", "zs.lib", "libz.a"}

// bundledLibraries holds the CMake definitions pointing rebrewu and the game
// at the libraries the Builder built.
type bundledLibraries struct {
	vulkanHeaders string // install prefix, empty when the system provides it
	zlibDefines   []string
	gameDefines   []string
	prefixes      []string
}

func installVulkanHeaders(b dependencyBuilder) (bundledLibraries, error) {
	dir, err := b.install(vulkanHeadersDependency)
	if err != nil {
		return bundledLibraries{}, err
	}
	prefix := b.prefix(vulkanHeadersDependency)
	return bundledLibraries{
		vulkanHeaders: prefix,
		gameDefines:   []string{"-DVulkanHeaders_DIR=" + dir},
		prefixes:      []string{prefix},
	}, nil
}

func installZlib(b dependencyBuilder, libs *bundledLibraries) error {
	zlibInclude, err := b.install(zlibDependency)
	if err != nil {
		return err
	}
	zlibLib, ok := findFile(b.prefix(zlibDependency), zlibStaticNames...)
	if !ok {
		return fmt.Errorf("zlib has no static library (%s) in %s", strings.Join(zlibStaticNames, ", "), b.prefix(zlibDependency))
	}
	libs.zlibDefines = zlibDefines(zlibInclude, zlibLib)
	libs.gameDefines = append(libs.gameDefines, libs.zlibDefines...)
	libs.prefixes = append(libs.prefixes, b.prefix(zlibDependency))
	return nil
}

func installCurl(b dependencyBuilder, libs *bundledLibraries) error {
	curlDir, err := b.install(curlDependencyFor(runtime.GOOS))
	if err != nil {
		return err
	}
	libs.gameDefines = append(libs.gameDefines, "-DCURL_DIR="+curlDir)
	libs.prefixes = append(libs.prefixes, b.prefix(curlDependency))
	return nil
}

func installZlibAndCurl(b dependencyBuilder, libs *bundledLibraries) error {
	if err := installZlib(b, libs); err != nil {
		return err
	}
	return installCurl(b, libs)
}

// zlibDefines points FindZLIB at one library. ZLIB_LIBRARY_RELEASE is set
// too: FindZLIB links a cached ZLIB_LIBRARY_RELEASE before ZLIB_LIBRARY, so a
// build folder that once found another zlib would keep linking its DLL.
func zlibDefines(include, library string) []string {
	return []string{"-DZLIB_INCLUDE_DIR=" + include, "-DZLIB_LIBRARY=" + library, "-DZLIB_LIBRARY_RELEASE=" + library}
}

// locateVolk prefers volk the Builder installed, then a system volk (when not
// using the hermetic userspace compiler), and builds it otherwise.
func locateVolk(b dependencyBuilder, dep sourceDependency) (string, string, error) {
	if dir, ok := b.installedDir(dep); ok {
		return dir, b.prefix(dep), nil
	}
	if !prereq.UsingUserspaceCompiler() {
		if config, err := findVolkConfig(b.buildDir); err == nil && !strings.HasPrefix(config, b.prefix(dep)) {
			dir := filepath.Dir(config)
			p := filepath.Dir(dir)
			if filepath.Base(p) == "cmake" {
				p = filepath.Dir(p)
			}
			return dir, filepath.Dir(p), nil
		}
	}
	dir, err := b.install(dep)
	return dir, b.prefix(dep), err
}

const (
	imguiDockingURL = "https://github.com/ocornut/imgui/archive/refs/heads/docking.tar.gz"
	miniaudioURL    = "https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h"
	miniaudioSHA256 = "ac7af4de748b7e748b7e37e01cee313a308a7296a3eb080e2906b320cc55c89"
	nlohmannURL     = "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
	nlohmannSHA256  = "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6"
)

// prepareHeaderDependencies downloads imgui, miniaudio, and nlohmann/json
// from official upstream URLs so CMake never needs git at configure time.
func prepareHeaderDependencies(b dependencyBuilder, recompilerDir string, libs *bundledLibraries) error {
	// 1. nlohmann/json.hpp
	jsonTarget := filepath.Join(recompilerDir, "third_party", "nlohmann", "json.hpp")
	if _, err := os.Stat(jsonTarget); err != nil {
		jsonArchive := filepath.Join(b.downloads, "nlohmann-json-3.11.3.hpp")
		if err := ensureVerifiedDownload(jsonArchive, nlohmannURL, nlohmannSHA256, "nlohmann/json", b.log); err != nil {
			return err
		}
		if err := copySingleFile(jsonArchive, jsonTarget); err != nil {
			return err
		}
	}

	// 2. miniaudio.h
	miniaudioTarget := filepath.Join(recompilerDir, "third_party", "miniaudio.h")
	if _, err := os.Stat(miniaudioTarget); err != nil {
		miniaudioCache := filepath.Join(b.downloads, "miniaudio-0.11.25.h")
		if err := ensureVerifiedDownload(miniaudioCache, miniaudioURL, miniaudioSHA256, "miniaudio", b.log); err != nil {
			return err
		}
		if err := copySingleFile(miniaudioCache, miniaudioTarget); err != nil {
			return err
		}
	}

	// 3. Dear ImGui (docking)
	imguiArchive := filepath.Join(b.downloads, "imgui-docking.tar.gz")
	if _, err := os.Stat(imguiArchive); err != nil {
		b.log("Downloading Dear ImGui (docking)...")
		if err := downloadFile(imguiDockingURL, imguiArchive); err != nil {
			return fmt.Errorf("downloading imgui: %w", err)
		}
	}
	imguiSource := filepath.Join(b.buildDir, "imgui-source")
	if _, err := os.Stat(filepath.Join(imguiSource, "imgui.cpp")); err != nil {
		_ = os.RemoveAll(imguiSource)
		if err := os.MkdirAll(imguiSource, 0755); err != nil {
			return err
		}
		if err := extractTarGzStrip1(imguiArchive, imguiSource); err != nil {
			return fmt.Errorf("extracting imgui: %w", err)
		}
	}
	libs.gameDefines = append(libs.gameDefines, "-DFETCHCONTENT_SOURCE_DIR_IMGUI="+imguiSource)
	return nil
}

func ensureVerifiedDownload(dest, url, expectedHash, label string, log logFunc) error {
	if _, err := os.Stat(dest); err == nil {
		if h, hErr := fileHash(dest); hErr == nil && (expectedHash == "" || strings.EqualFold(h, expectedHash)) {
			return nil
		}
		_ = os.Remove(dest)
	}
	log("Downloading %s...", label)
	if err := downloadFile(url, dest); err != nil {
		return fmt.Errorf("downloading %s: %w", label, err)
	}
	if expectedHash != "" {
		h, err := fileHash(dest)
		if err != nil || !strings.EqualFold(h, expectedHash) {
			_ = os.Remove(dest)
			return fmt.Errorf("integrity check failed for %s", label)
		}
	}
	return nil
}

// extractTarGzStrip1 extracts a .tar.gz archive into destDir, stripping the
// top-level directory component without relying on an external tar binary.
func extractTarGzStrip1(archivePath, destDir string) error {
	f, err := os.Open(archivePath)
	if err != nil {
		return err
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	defer gz.Close()

	absDest, err := filepath.Abs(destDir)
	if err != nil {
		return err
	}
	tr := tar.NewReader(gz)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		clean := filepath.ToSlash(filepath.Clean(hdr.Name))
		clean = strings.TrimPrefix(clean, "./")
		idx := strings.IndexByte(clean, '/')
		if idx < 0 {
			continue
		}
		rel := clean[idx+1:]
		if rel == "" || rel == "." || strings.HasPrefix(rel, "../") || rel == ".." || filepath.IsAbs(rel) {
			continue
		}
		target := filepath.Join(absDest, filepath.FromSlash(rel))
		if !strings.HasPrefix(target, absDest+string(filepath.Separator)) && target != absDest {
			continue
		}

		switch hdr.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
		case tar.TypeSymlink:
			if filepath.IsAbs(hdr.Linkname) {
				continue
			}
			linkTarget := filepath.Clean(filepath.Join(filepath.Dir(target), hdr.Linkname))
			if !strings.HasPrefix(linkTarget, absDest+string(filepath.Separator)) {
				continue
			}
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			_ = os.Remove(target)
			if err := os.Symlink(hdr.Linkname, target); err != nil {
				return err
			}
		case tar.TypeReg, tar.TypeRegA:
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			mode := os.FileMode(hdr.Mode) & os.ModePerm
			if mode == 0 {
				mode = 0644
			}
			out, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
			if err != nil {
				return err
			}
			if _, err := io.Copy(out, tr); err != nil {
				out.Close()
				return err
			}
			if err := out.Close(); err != nil {
				return err
			}
		}
	}
	return nil
}

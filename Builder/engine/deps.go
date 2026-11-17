package engine

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
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
		extract := exec.CommandContext(b.ctx, "tar", "-xzf", archive, "-C", source, "--strip-components=1")
		if err := runCmdWithLog(extract, b.log); err != nil {
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
	configure := exec.CommandContext(b.ctx, "cmake", b.cmake.configureArgs(source, build, defines...)...)
	if err := runCmdWithLog(configure, b.log); err != nil {
		return "", fmt.Errorf("configuring %s: %w", dep.name, err)
	}
	buildCmd := exec.CommandContext(b.ctx, "cmake", b.cmake.buildArgs(build, "install")...)
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

// Windows has no system copy of these libraries. They are built as static
// Release libraries, so rebrewu and the game need no extra DLL.
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
		options:   []string{"-DZLIB_BUILD_EXAMPLES=OFF"},
		installed: []string{"zlib.h"},
	}
	curlDependency = sourceDependency{
		name:    "curl",
		url:     "https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.gz",
		archive: "curl-8.11.1.tar.gz",
		options: []string{
			"-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON", "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF",
			"-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF", "-DENABLE_CURL_MANUAL=OFF",
			"-DCURL_USE_SCHANNEL=ON", "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_ZLIB=OFF",
			"-DCURL_ZSTD=OFF", "-DCURL_BROTLI=OFF", "-DUSE_NGHTTP2=OFF", "-DUSE_LIBIDN2=OFF", "-DCURL_DISABLE_LDAP=ON",
		},
		installed: []string{"CURLConfig.cmake"},
	}
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

func installZlibAndCurl(b dependencyBuilder, libs *bundledLibraries) error {
	zlibInclude, err := b.install(zlibDependency)
	if err != nil {
		return err
	}
	zlibLib, ok := findFile(b.prefix(zlibDependency), zlibStaticNames...)
	if !ok {
		return fmt.Errorf("zlib has no static library (%s) in %s", strings.Join(zlibStaticNames, ", "), b.prefix(zlibDependency))
	}
	libs.zlibDefines = zlibDefines(zlibInclude, zlibLib)

	curlDir, err := b.install(curlDependency)
	if err != nil {
		return err
	}
	libs.gameDefines = append(libs.gameDefines, "-DCURL_DIR="+curlDir)
	libs.gameDefines = append(libs.gameDefines, libs.zlibDefines...)
	libs.prefixes = append(libs.prefixes, b.prefix(curlDependency), b.prefix(zlibDependency))
	return nil
}

// zlibDefines points FindZLIB at one library. ZLIB_LIBRARY_RELEASE is set
// too: FindZLIB links a cached ZLIB_LIBRARY_RELEASE before ZLIB_LIBRARY, so a
// build folder that once found another zlib would keep linking its DLL.
func zlibDefines(include, library string) []string {
	return []string{"-DZLIB_INCLUDE_DIR=" + include, "-DZLIB_LIBRARY=" + library, "-DZLIB_LIBRARY_RELEASE=" + library}
}

// locateVolk prefers volk the Builder installed, then a system volk, and
// builds it otherwise. It returns the config directory and install prefix.
func locateVolk(b dependencyBuilder, dep sourceDependency) (string, string, error) {
	if dir, ok := b.installedDir(dep); ok {
		return dir, b.prefix(dep), nil
	}
	if config, err := findVolkConfig(b.buildDir); err == nil && !strings.HasPrefix(config, b.prefix(dep)) {
		dir := filepath.Dir(config)
		p := filepath.Dir(dir)
		if filepath.Base(p) == "cmake" {
			p = filepath.Dir(p)
		}
		return dir, filepath.Dir(p), nil
	}
	dir, err := b.install(dep)
	return dir, b.prefix(dep), err
}

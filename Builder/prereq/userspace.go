package prereq

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"

	"splatpc-builder/proc"
)

// ToolSpec describes a dependency tool downloaded from an official upstream
// release into the user's cache directory.
type ToolSpec struct {
	ID          string
	Name        string
	Version     string
	URL         string
	ArchiveName string
	SHA256      string
	ArchiveType string // "tar.gz", "tar.xz", "zip", "appimage"
	InstallDir  string // Subdirectory under UserToolsDir()
	StripTopDir bool
}

// UserToolsDir returns the 100% userspace directory where downloaded build
// tools and toolchains live. Overridable via SPLATPC_TOOLS_DIR.
func UserToolsDir() string {
	if override := os.Getenv("SPLATPC_TOOLS_DIR"); override != "" {
		return override
	}
	if runtime.GOOS == "windows" {
		localAppData := os.Getenv("LOCALAPPDATA")
		if localAppData == "" {
			home := os.Getenv("USERPROFILE")
			if home == "" {
				home, _ = os.UserHomeDir()
			}
			localAppData = filepath.Join(home, "AppData", "Local")
		}
		return filepath.Join(localAppData, "SplatPC", "tools")
	}
	cacheHome := os.Getenv("XDG_CACHE_HOME")
	if cacheHome == "" {
		home := os.Getenv("HOME")
		if home == "" {
			home, _ = os.UserHomeDir()
		}
		cacheHome = filepath.Join(home, ".cache")
	}
	return filepath.Join(cacheHome, "splatpc", "tools")
}

// UserToolsBinDirs returns the executable directories of any userspace tools
// currently installed under UserToolsDir().
func UserToolsBinDirs() []string {
	root := UserToolsDir()
	candidates := []string{
		filepath.Join(root, "cmake", "bin"),
		filepath.Join(root, "ninja", "bin"),
		filepath.Join(root, "python", "bin"),
		filepath.Join(root, "python"),
		filepath.Join(root, "toolchain", "bin"),
		filepath.Join(root, "llvm-mingw", "bin"),
		filepath.Join(root, "appimagetool"),
	}
	var dirs []string
	for _, d := range candidates {
		if info, err := os.Stat(d); err == nil && info.IsDir() {
			dirs = append(dirs, d)
		}
	}
	return dirs
}

// isUnderDir reports whether path resides inside dir.
func isUnderDir(path, dir string) bool {
	absPath, err1 := filepath.Abs(path)
	absDir, err2 := filepath.Abs(dir)
	if err1 != nil || err2 != nil {
		return false
	}
	rel, err := filepath.Rel(absDir, absPath)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

// LookPath finds an executable first in the userspace tools directory, then
// on the system PATH.
func LookPath(name string) (string, bool) {
	exe := name
	if runtime.GOOS == "windows" && filepath.Ext(exe) == "" {
		exe += ".exe"
	}
	for _, dir := range UserToolsBinDirs() {
		candidate := filepath.Join(dir, exe)
		if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
			return candidate, true
		}
	}
	if p, err := exec.LookPath(name); err == nil {
		return p, true
	}
	return "", false
}

// UsingUserspaceCompiler reports whether the active C++ compiler comes from
// UserToolsDir() rather than a system toolchain.
func UsingUserspaceCompiler() bool {
	root := UserToolsDir()
	if runtime.GOOS == "windows" {
		if _, ok := FindVisualStudio(); ok {
			return false
		}
		for _, c := range []string{"cl", "g++", "clang++"} {
			if p, err := exec.LookPath(c); err == nil && !isUnderDir(p, root) {
				return false
			}
		}
		_, err := os.Stat(filepath.Join(root, "llvm-mingw", "bin", "clang++.exe"))
		return err == nil
	}
	for _, c := range []string{"g++", "clang++"} {
		if p, err := exec.LookPath(c); err == nil && !isUnderDir(p, root) {
			return false
		}
	}
	_, err := os.Stat(filepath.Join(root, "toolchain", "bin", "clang++"))
	return err == nil
}

// ToolEnv returns a copy of baseEnv with userspace tool bin directories
// prepended to PATH, and CC/CXX/AR/RANLIB configured when using the userspace
// compiler toolchain.
func ToolEnv(baseEnv []string) []string {
	binDirs := UserToolsBinDirs()
	env := slices.Clone(baseEnv)
	if len(binDirs) > 0 {
		prefix := strings.Join(binDirs, string(os.PathListSeparator))
		updated := false
		for i, kv := range env {
			if strings.HasPrefix(strings.ToUpper(kv), "PATH=") {
				key, val, _ := strings.Cut(kv, "=")
				env[i] = key + "=" + prefix + string(os.PathListSeparator) + val
				updated = true
				break
			}
		}
		if !updated {
			env = append(env, "PATH="+prefix)
		}
	}

	if UsingUserspaceCompiler() {
		root := UserToolsDir()
		if runtime.GOOS == "windows" {
			bin := filepath.Join(root, "llvm-mingw", "bin")
			env = setEnvDefault(env, "CC", filepath.Join(bin, "clang.exe"))
			env = setEnvDefault(env, "CXX", filepath.Join(bin, "clang++.exe"))
			env = setEnvDefault(env, "RC", filepath.Join(bin, "windres.exe"))
			env = setEnvDefault(env, "AR", filepath.Join(bin, "llvm-ar.exe"))
			env = setEnvDefault(env, "RANLIB", filepath.Join(bin, "llvm-ranlib.exe"))
		} else {
			bin := filepath.Join(root, "toolchain", "bin")
			env = setEnvDefault(env, "CC", filepath.Join(bin, "cc"))
			env = setEnvDefault(env, "CXX", filepath.Join(bin, "c++"))
			env = setEnvDefault(env, "AR", filepath.Join(bin, "ar"))
			env = setEnvDefault(env, "RANLIB", filepath.Join(bin, "ranlib"))
		}
	}
	return env
}

func setEnvDefault(env []string, key, value string) []string {
	for _, kv := range env {
		if k, _, ok := strings.Cut(kv, "="); ok && strings.EqualFold(k, key) {
			return env
		}
	}
	return append(env, key+"="+value)
}

// ToolSpecsFor returns the official upstream userspace tool definitions for
// the given GOOS and GOARCH.
func ToolSpecsFor(goos, goarch string) map[string]ToolSpec {
	specs := make(map[string]ToolSpec)

	switch goos + "/" + goarch {
	case "linux/amd64":
		specs["cmake"] = ToolSpec{
			ID: "cmake", Name: "CMake", Version: "4.5.1",
			URL:         "https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-linux-x86_64.tar.gz",
			ArchiveName: "cmake-4.5.1-linux-x86_64.tar.gz",
			SHA256:      "83b71181fb62b4fe8f26067a9321448572e7a43bd04ff6c7f54a24c33698f16d",
			ArchiveType: "tar.gz", InstallDir: "cmake", StripTopDir: true,
		}
		specs["ninja"] = ToolSpec{
			ID: "ninja", Name: "Ninja", Version: "1.13.2",
			URL:         "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip",
			ArchiveName: "ninja-linux.zip",
			SHA256:      "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6",
			ArchiveType: "zip", InstallDir: filepath.Join("ninja", "bin"), StripTopDir: false,
		}
		specs["python3"] = ToolSpec{
			ID: "python3", Name: "Python 3.13", Version: "3.13.18",
			URL:         "https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.13.18%2B20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
			ArchiveName: "cpython-3.13.18+20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
			SHA256:      "f2d2199eb25880e07863b391cf7124e7ffa1338b0d65600dfcec117c4223a7fb",
			ArchiveType: "tar.gz", InstallDir: "python", StripTopDir: true,
		}
		specs["compiler"] = ToolSpec{
			ID: "compiler", Name: "Zig C/C++ Toolchain (Clang 22)", Version: "0.17.1",
			URL:         "https://ziglang.org/download/0.17.1/zig-x86_64-linux-0.17.1.tar.xz",
			ArchiveName: "zig-x86_64-linux-0.17.1.tar.xz",
			SHA256:      "3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a",
			ArchiveType: "tar.xz", InstallDir: "zig", StripTopDir: true,
		}
		specs["appimagetool"] = ToolSpec{
			ID: "appimagetool", Name: "appimagetool", Version: "1.9.1",
			URL:         "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
			ArchiveName: "appimagetool-x86_64.AppImage",
			SHA256:      "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
			ArchiveType: "appimage", InstallDir: "appimagetool", StripTopDir: false,
		}
	case "linux/arm64":
		specs["cmake"] = ToolSpec{
			ID: "cmake", Name: "CMake", Version: "4.5.1",
			URL:         "https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-linux-aarch64.tar.gz",
			ArchiveName: "cmake-4.5.1-linux-aarch64.tar.gz",
			SHA256:      "3bec208f11ed96c173032eeaf97937c54dd246acefee22acd7794a6b4e70d536",
			ArchiveType: "tar.gz", InstallDir: "cmake", StripTopDir: true,
		}
		specs["ninja"] = ToolSpec{
			ID: "ninja", Name: "Ninja", Version: "1.13.2",
			URL:         "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux-aarch64.zip",
			ArchiveName: "ninja-linux-aarch64.zip",
			SHA256:      "fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc",
			ArchiveType: "zip", InstallDir: filepath.Join("ninja", "bin"), StripTopDir: false,
		}
		specs["python3"] = ToolSpec{
			ID: "python3", Name: "Python 3.13", Version: "3.13.18",
			URL:         "https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.13.18%2B20261110-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz",
			ArchiveName: "cpython-3.13.18+20261110-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz",
			SHA256:      "55c00325f7c1fddf325dfc621aabdfbbac4dc54adda224af88ec411dc12db239",
			ArchiveType: "tar.gz", InstallDir: "python", StripTopDir: true,
		}
		specs["compiler"] = ToolSpec{
			ID: "compiler", Name: "Zig C/C++ Toolchain (Clang 22)", Version: "0.17.1",
			URL:         "https://ziglang.org/download/0.17.1/zig-aarch64-linux-0.17.1.tar.xz",
			ArchiveName: "zig-aarch64-linux-0.17.1.tar.xz",
			SHA256:      "a1546d5646eb184437918d6635ca6b762a0e889eac09cef720ee7a1ed3fe7795",
			ArchiveType: "tar.xz", InstallDir: "zig", StripTopDir: true,
		}
		specs["appimagetool"] = ToolSpec{
			ID: "appimagetool", Name: "appimagetool", Version: "1.9.1",
			URL:         "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-aarch64.AppImage",
			ArchiveName: "appimagetool-aarch64.AppImage",
			SHA256:      "f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158",
			ArchiveType: "appimage", InstallDir: "appimagetool", StripTopDir: false,
		}
	case "windows/amd64":
		specs["cmake"] = ToolSpec{
			ID: "cmake", Name: "CMake", Version: "4.5.1",
			URL:         "https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-windows-x86_64.zip",
			ArchiveName: "cmake-4.5.1-windows-x86_64.zip",
			SHA256:      "bfe6eadb245dc8213e0d3feea316b520cb2aae92f5667ffd76059ba6b05c430c",
			ArchiveType: "zip", InstallDir: "cmake", StripTopDir: true,
		}
		specs["ninja"] = ToolSpec{
			ID: "ninja", Name: "Ninja", Version: "1.13.2",
			URL:         "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
			ArchiveName: "ninja-win.zip",
			SHA256:      "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65",
			ArchiveType: "zip", InstallDir: filepath.Join("ninja", "bin"), StripTopDir: false,
		}
		specs["python3"] = ToolSpec{
			ID: "python3", Name: "Python 3.13", Version: "3.13.18",
			URL:         "https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.13.18%2B20261110-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
			ArchiveName: "cpython-3.13.18+20261110-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
			SHA256:      "2e0fed09d48989f64c9305da9ff2d37a41c0d13ab2aa6283405b2b620dfbf805",
			ArchiveType: "tar.gz", InstallDir: "python", StripTopDir: true,
		}
		specs["compiler"] = ToolSpec{
			ID: "compiler", Name: "LLVM-MinGW C/C++ Toolchain (UCRT)", Version: "20261117",
			URL:         "https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip",
			ArchiveName: "llvm-mingw-20261117-ucrt-x86_64.zip",
			SHA256:      "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412",
			ArchiveType: "zip", InstallDir: "llvm-mingw", StripTopDir: true,
		}
	case "windows/arm64":
		specs["cmake"] = ToolSpec{
			ID: "cmake", Name: "CMake", Version: "4.5.1",
			URL:         "https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-windows-arm64.zip",
			ArchiveName: "cmake-4.5.1-windows-arm64.zip",
			SHA256:      "6782f11382941bb85c406eb3bd32a702222d8956535ae9cec5ee77395be2cc78",
			ArchiveType: "zip", InstallDir: "cmake", StripTopDir: true,
		}
		specs["ninja"] = ToolSpec{
			ID: "ninja", Name: "Ninja", Version: "1.13.2",
			URL:         "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-winarm64.zip",
			ArchiveName: "ninja-winarm64.zip",
			SHA256:      "e52f0bdef9dfb1003229dbd6508a508c4073fd017247002adc66e5e806cb0391",
			ArchiveType: "zip", InstallDir: filepath.Join("ninja", "bin"), StripTopDir: false,
		}
		specs["python3"] = ToolSpec{
			ID: "python3", Name: "Python 3.13", Version: "3.13.18",
			URL:         "https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.13.18%2B20261110-aarch64-pc-windows-msvc-install_only_stripped.tar.gz",
			ArchiveName: "cpython-3.13.18+20261110-aarch64-pc-windows-msvc-install_only_stripped.tar.gz",
			SHA256:      "0a8840071810ddb8565256cd406b466d4e9dc6dc523395a5477880dbc4f67eb3",
			ArchiveType: "tar.gz", InstallDir: "python", StripTopDir: true,
		}
		specs["compiler"] = ToolSpec{
			ID: "compiler", Name: "LLVM-MinGW C/C++ Toolchain (UCRT)", Version: "20261117",
			URL:         "https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-aarch64.zip",
			ArchiveName: "llvm-mingw-20261117-ucrt-aarch64.zip",
			SHA256:      "c7f56de1d7aca15196c19083a799205390397b8b87a825aec3485e1e4f3c2cdd",
			ArchiveType: "zip", InstallDir: "llvm-mingw", StripTopDir: true,
		}
	}

	return specs
}

// DownloadableMissingTools returns the IDs of all missing tools in report
// that have an official userspace download available.
func DownloadableMissingTools(report CheckReport) []string {
	specs := ToolSpecsFor(runtime.GOOS, runtime.GOARCH)
	var ids []string
	for _, item := range report.Items {
		if !item.Found {
			if _, ok := specs[item.ID]; ok {
				ids = append(ids, item.ID)
			}
		}
	}
	return ids
}

// InstallUserspaceTools downloads, verifies, and installs the requested tools
// into UserToolsDir() in 100% userspace.
func InstallUserspaceTools(ctx context.Context, ids []string, log func(string, ...interface{})) error {
	if len(ids) == 0 {
		return nil
	}
	if log == nil {
		log = func(string, ...interface{}) {}
	}
	specs := ToolSpecsFor(runtime.GOOS, runtime.GOARCH)
	root := UserToolsDir()
	downloads := filepath.Join(root, "downloads")
	if err := os.MkdirAll(downloads, 0755); err != nil {
		return err
	}
	defer os.RemoveAll(downloads)

	// Install python3 first so its tarfile/lzma module can unpack .tar.xz on
	// systems lacking external xz tools.
	ordered := orderToolIDs(ids)
	for _, id := range ordered {
		spec, ok := specs[id]
		if !ok {
			return fmt.Errorf("no userspace package defined for %q on %s/%s", id, runtime.GOOS, runtime.GOARCH)
		}
		if err := installOneTool(ctx, root, downloads, spec, log); err != nil {
			return err
		}
	}
	return nil
}

func orderToolIDs(ids []string) []string {
	priority := map[string]int{"python3": 0, "cmake": 1, "ninja": 2, "compiler": 3, "appimagetool": 4}
	out := slices.Clone(ids)
	slices.SortStableFunc(out, func(a, b string) int {
		pa, okA := priority[a]
		pb, okB := priority[b]
		if !okA {
			pa = 99
		}
		if !okB {
			pb = 99
		}
		return pa - pb
	})
	return out
}

func installOneTool(ctx context.Context, root, downloads string, spec ToolSpec, log func(string, ...interface{})) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	archivePath := filepath.Join(downloads, spec.ArchiveName)
	log("Downloading %s %s from official source...", spec.Name, spec.Version)
	if err := downloadVerified(ctx, spec.URL, archivePath, spec.SHA256); err != nil {
		return fmt.Errorf("downloading %s: %w", spec.Name, err)
	}

	destDir := filepath.Join(root, spec.InstallDir)
	_ = os.RemoveAll(destDir)
	if err := os.MkdirAll(destDir, 0755); err != nil {
		return err
	}

	log("Installing %s into %s...", spec.Name, destDir)
	switch spec.ArchiveType {
	case "tar.gz":
		if err := extractTarGz(archivePath, destDir, spec.StripTopDir); err != nil {
			return fmt.Errorf("extracting %s: %w", spec.Name, err)
		}
	case "zip":
		if err := extractZip(archivePath, destDir, spec.StripTopDir); err != nil {
			return fmt.Errorf("extracting %s: %w", spec.Name, err)
		}
	case "tar.xz":
		if err := extractTarXz(ctx, archivePath, destDir, spec.StripTopDir); err != nil {
			return fmt.Errorf("extracting %s: %w", spec.Name, err)
		}
	case "appimage":
		if err := installAppImageTool(ctx, archivePath, destDir); err != nil {
			return fmt.Errorf("installing %s: %w", spec.Name, err)
		}
	default:
		return fmt.Errorf("unsupported archive type %q", spec.ArchiveType)
	}

	if spec.ID == "compiler" && runtime.GOOS != "windows" {
		if err := createZigWrappers(root, destDir); err != nil {
			return fmt.Errorf("creating compiler wrappers: %w", err)
		}
	}

	_ = os.Remove(archivePath)
	log("%s %s ready.", spec.Name, spec.Version)
	return nil
}

func createZigWrappers(root, zigDir string) error {
	zigBin := filepath.Join(zigDir, "zig")
	if _, err := os.Stat(zigBin); err != nil {
		return fmt.Errorf("zig binary not found at %s", zigBin)
	}
	binDir := filepath.Join(root, "toolchain", "bin")
	_ = os.RemoveAll(binDir)
	if err := os.MkdirAll(binDir, 0755); err != nil {
		return err
	}

	wrappers := map[string]string{
		"cc":      fmt.Sprintf("#!/bin/sh\nexec %q cc -fno-sanitize=undefined \"$@\"\n", zigBin),
		"clang":   fmt.Sprintf("#!/bin/sh\nexec %q cc -fno-sanitize=undefined \"$@\"\n", zigBin),
		"c++":     fmt.Sprintf("#!/bin/sh\nexec %q c++ -fno-sanitize=undefined \"$@\"\n", zigBin),
		"clang++": fmt.Sprintf("#!/bin/sh\nexec %q c++ -fno-sanitize=undefined \"$@\"\n", zigBin),
		"ar":      fmt.Sprintf("#!/bin/sh\nexec %q ar \"$@\"\n", zigBin),
		"ranlib":  fmt.Sprintf("#!/bin/sh\nexec %q ranlib \"$@\"\n", zigBin),
	}
	for name, content := range wrappers {
		p := filepath.Join(binDir, name)
		if err := os.WriteFile(p, []byte(content), 0755); err != nil {
			return err
		}
	}
	return nil
}

func installAppImageTool(ctx context.Context, archivePath, destDir string) error {
	rawPath := filepath.Join(destDir, "appimagetool.AppImage")
	if err := os.Rename(archivePath, rawPath); err != nil {
		in, openErr := os.Open(archivePath)
		if openErr != nil {
			return openErr
		}
		out, createErr := os.OpenFile(rawPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0755)
		if createErr != nil {
			in.Close()
			return createErr
		}
		_, copyErr := io.Copy(out, in)
		in.Close()
		out.Close()
		if copyErr != nil {
			return copyErr
		}
	}
	_ = os.Chmod(rawPath, 0755)

	// Extract once so appimagetool runs even in containers or systems without FUSE.
	extractCmd := proc.CommandContext(ctx, rawPath, "--appimage-extract")
	extractCmd.Dir = destDir
	wrapperPath := filepath.Join(destDir, "appimagetool")
	if err := extractCmd.Run(); err == nil {
		appRun := filepath.Join(destDir, "squashfs-root", "AppRun")
		if _, statErr := os.Stat(appRun); statErr == nil {
			_ = os.Remove(rawPath)
			script := fmt.Sprintf("#!/bin/sh\nexec %q \"$@\"\n", appRun)
			return os.WriteFile(wrapperPath, []byte(script), 0755)
		}
	}

	script := fmt.Sprintf("#!/bin/sh\nAPPIMAGE_EXTRACT_AND_RUN=1 exec %q \"$@\"\n", rawPath)
	return os.WriteFile(wrapperPath, []byte(script), 0755)
}

func downloadVerified(ctx context.Context, url, dest, expectedSHA256 string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %s", resp.Status)
	}

	tmp := dest + ".part"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	hasher := sha256.New()
	if _, err := io.Copy(io.MultiWriter(out, hasher), resp.Body); err != nil {
		out.Close()
		_ = os.Remove(tmp)
		return err
	}
	if err := out.Close(); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if expectedSHA256 != "" {
		actual := hex.EncodeToString(hasher.Sum(nil))
		if !strings.EqualFold(actual, expectedSHA256) {
			_ = os.Remove(tmp)
			return fmt.Errorf("SHA-256 mismatch for %s: got %s, expected %s", filepath.Base(dest), actual, expectedSHA256)
		}
	}
	return os.Rename(tmp, dest)
}

func stripArchiveTopComponent(name string, strip bool) (string, bool) {
	clean := filepath.ToSlash(filepath.Clean(name))
	clean = strings.TrimPrefix(clean, "./")
	if clean == "" || clean == "." {
		return "", false
	}
	if strip {
		idx := strings.IndexByte(clean, '/')
		if idx < 0 {
			return "", false
		}
		clean = clean[idx+1:]
		if clean == "" || clean == "." {
			return "", false
		}
	}
	if strings.HasPrefix(clean, "../") || clean == ".." || filepath.IsAbs(clean) {
		return "", false
	}
	return filepath.FromSlash(clean), true
}

func extractTarGz(archivePath, destDir string, stripTop bool) error {
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

	tr := tar.NewReader(gz)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		rel, ok := stripArchiveTopComponent(hdr.Name, stripTop)
		if !ok {
			continue
		}
		target := filepath.Join(destDir, rel)
		if !isUnderDir(target, destDir) {
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
			if !isUnderDir(linkTarget, destDir) {
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
			_ = os.Chmod(target, mode)
		}
	}
	return nil
}

func extractZip(archivePath, destDir string, stripTop bool) error {
	zr, err := zip.OpenReader(archivePath)
	if err != nil {
		return err
	}
	defer zr.Close()

	for _, f := range zr.File {
		rel, ok := stripArchiveTopComponent(f.Name, stripTop)
		if !ok {
			continue
		}
		target := filepath.Join(destDir, rel)
		if !isUnderDir(target, destDir) {
			continue
		}

		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
			continue
		}
		if f.Mode()&os.ModeSymlink != 0 {
			rc, err := f.Open()
			if err != nil {
				return err
			}
			linkBytes, err := io.ReadAll(rc)
			rc.Close()
			if err != nil {
				return err
			}
			linkName := string(linkBytes)
			if filepath.IsAbs(linkName) {
				continue
			}
			linkTarget := filepath.Clean(filepath.Join(filepath.Dir(target), linkName))
			if !isUnderDir(linkTarget, destDir) {
				continue
			}
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			_ = os.Remove(target)
			if err := os.Symlink(linkName, target); err != nil {
				return err
			}
			continue
		}

		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		mode := f.Mode() & os.ModePerm
		if mode == 0 {
			mode = 0644
		}
		out, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
		if err != nil {
			rc.Close()
			return err
		}
		_, copyErr := io.Copy(out, rc)
		rc.Close()
		closeErr := out.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
		_ = os.Chmod(target, mode)
	}
	return nil
}

func extractTarXz(ctx context.Context, archivePath, destDir string, stripTop bool) error {
	if _, err := exec.LookPath("tar"); err == nil {
		args := []string{"-xJf", archivePath, "-C", destDir}
		if stripTop {
			args = append(args, "--strip-components=1")
		}
		if err := proc.CommandContext(ctx, "tar", args...).Run(); err == nil {
			return nil
		}
	}
	python, _, ok := FindPython()
	if !ok {
		return fmt.Errorf("cannot extract %s: neither 'tar' with xz support nor Python 3 was found", filepath.Base(archivePath))
	}
	script := `import os, pathlib, sys, tarfile
archive, dest, strip = sys.argv[1], pathlib.Path(sys.argv[2]).resolve(), sys.argv[3] == "1"
with tarfile.open(archive, "r:xz") as tf:
    for m in tf.getmembers():
        parts = pathlib.PurePosixPath(m.name).parts
        if parts and parts[0] == ".":
            parts = parts[1:]
        if strip:
            parts = parts[1:]
        if not parts:
            continue
        rel = pathlib.Path(*parts)
        if rel.is_absolute() or ".." in rel.parts:
            continue
        m.name = rel.as_posix()
        tf.extract(m, dest, filter="data")
`
	stripFlag := "0"
	if stripTop {
		stripFlag = "1"
	}
	out, err := python.Command(ctx, "-c", script, archivePath, destDir, stripFlag).CombinedOutput()
	if err != nil {
		return fmt.Errorf("%w: %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}

// RemoveUserspaceTools deletes the entire UserToolsDir() directory.
func RemoveUserspaceTools() error {
	return os.RemoveAll(UserToolsDir())
}

// UserspaceToolsSize returns the total bytes currently occupied by UserToolsDir().
func UserspaceToolsSize() int64 {
	return DirSize(UserToolsDir())
}

// DirSize sums the sizes of all regular files under root. Returns 0 if missing.
func DirSize(root string) int64 {
	var total int64
	_ = filepath.WalkDir(root, func(_ string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if info, infoErr := d.Info(); infoErr == nil {
			total += info.Size()
		}
		return nil
	})
	return total
}

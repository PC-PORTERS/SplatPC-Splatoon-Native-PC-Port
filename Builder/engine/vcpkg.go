package engine

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"splatpc-builder/prereq"
)

// EnsureWindowsVcpkg verifies or bootstraps vcpkg and ensures required C++ libraries
// (zlib, curl, volk, vulkan-headers) are installed for Windows builds.
func EnsureWindowsVcpkg(ctx context.Context, repoRoot string, buildDir string, log func(string, ...interface{})) (string, error) {
	if runtime.GOOS != "windows" {
		return "", nil
	}

	exe, tc, found := prereq.FindVcpkg(repoRoot)
	if found && prereq.CheckVcpkgPackages(tc) {
		log("Using existing vcpkg toolchain: %s", tc)
		return tc, nil
	}

	vcpkgRoot := filepath.Join(buildDir, "vcpkg")
	// If path contains brackets or special characters, fallback to LOCALAPPDATA
	if strings.ContainsAny(vcpkgRoot, "[]") {
		localApp := os.Getenv("LOCALAPPDATA")
		if localApp != "" {
			vcpkgRoot = filepath.Join(localApp, "SplatoonPort", "vcpkg")
		}
	}
	_ = os.MkdirAll(filepath.Dir(vcpkgRoot), 0755)

	vcpkgExe := filepath.Join(vcpkgRoot, "vcpkg.exe")
	if !fileExists(vcpkgExe) {
		if !fileExists(filepath.Join(vcpkgRoot, ".git")) {
			log("Cloning vcpkg for Windows C++ dependencies...")
			gitPath, _, ok := prereq.FindGit()
			if !ok {
				gitPath = "git"
			}
			cmdClone := exec.CommandContext(ctx, gitPath, "clone", "--depth", "1", "https://github.com/microsoft/vcpkg.git", vcpkgRoot)
			if err := runCmdWithLog(cmdClone, log); err != nil {
				return "", fmt.Errorf("cloning vcpkg: %w", err)
			}
		}

		bootstrapBat := filepath.Join(vcpkgRoot, "bootstrap-vcpkg.bat")
		if fileExists(bootstrapBat) {
			log("Bootstrapping vcpkg...")
			cmdBoot := exec.CommandContext(ctx, "cmd.exe", "/c", bootstrapBat, "-disableMetrics")
			if err := runCmdWithLog(cmdBoot, log); err != nil {
				return "", fmt.Errorf("bootstrapping vcpkg: %w", err)
			}
		}
	}

	if !fileExists(vcpkgExe) {
		if found && fileExists(exe) {
			vcpkgExe = exe
			vcpkgRoot = filepath.Dir(exe)
		} else {
			return "", fmt.Errorf("vcpkg.exe not found at %s", vcpkgExe)
		}
	}

	tc = filepath.Join(vcpkgRoot, "scripts", "buildsystems", "vcpkg.cmake")
	if !prereq.CheckVcpkgPackages(tc) {
		log("Installing vcpkg packages: zlib, curl, volk, vulkan-headers (triplet x64-windows)...")
		cmdInstall := exec.CommandContext(ctx, vcpkgExe, "install", "zlib", "curl", "volk", "vulkan-headers",
			"--triplet", "x64-windows", "--disable-metrics")
		if err := runCmdWithLog(cmdInstall, log); err != nil {
			return "", fmt.Errorf("installing vcpkg packages: %w", err)
		}
	}

	return tc, nil
}

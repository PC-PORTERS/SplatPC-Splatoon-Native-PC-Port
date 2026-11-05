package prereq

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
)

type CheckItem struct {
	ID         string
	Name       string
	Category   string // "Tool", "Compiler", "Library"
	Required   bool
	Found      bool
	Version    string
	Path       string
	ErrorMsg   string
	InstallCmd string
}

type CheckReport struct {
	Items          []CheckItem
	AllRequiredMet bool
	OSName         string
	Distro         string
	InstallScript  string
}

func DetectDistro() string {
	if runtime.GOOS != "linux" {
		return runtime.GOOS
	}
	data, err := os.ReadFile("/etc/os-release")
	if err != nil {
		return "linux"
	}
	content := string(data)
	if strings.Contains(content, "ID=arch") || strings.Contains(content, "ID_LIKE=arch") {
		return "arch"
	}
	if strings.Contains(content, "ID=ubuntu") || strings.Contains(content, "ID=debian") || strings.Contains(content, "ID_LIKE=debian") {
		return "debian"
	}
	if strings.Contains(content, "ID=fedora") || strings.Contains(content, "ID_LIKE=rhel") {
		return "fedora"
	}
	if strings.Contains(content, "ID=opensuse") || strings.Contains(content, "ID_LIKE=suse") {
		return "opensuse"
	}
	return "linux"
}

func RunChecks() CheckReport {
	report := CheckReport{
		Items:  make([]CheckItem, 0),
		OSName: runtime.GOOS,
		Distro: DetectDistro(),
	}

	// 1. CMake
	cmakeItem := CheckItem{
		ID:       "cmake",
		Name:     "CMake (>= 3.20)",
		Category: "Tool",
		Required: true,
	}
	if path, err := exec.LookPath("cmake"); err == nil {
		cmakeItem.Found = true
		cmakeItem.Path = path
		if out, err := exec.Command("cmake", "--version").Output(); err == nil {
			v := parseVersion(string(out))
			cmakeItem.Version = v
			if isVersionOlder(v, "3.20") {
				cmakeItem.Found = false
				cmakeItem.ErrorMsg = fmt.Sprintf("CMake %s is too old (>= 3.20 required)", v)
			}
		}
	} else {
		cmakeItem.ErrorMsg = "CMake is not installed or not in PATH"
	}
	report.Items = append(report.Items, cmakeItem)

	// 2. Python 3
	pyItem := CheckItem{
		ID:       "python3",
		Name:     "Python 3 (>= 3.12)",
		Category: "Tool",
		Required: true,
	}
	pyBin := "python3"
	if runtime.GOOS == "windows" {
		if _, err := exec.LookPath("python3"); err != nil {
			pyBin = "python"
		}
	}
	if path, err := exec.LookPath(pyBin); err == nil {
		pyItem.Path = path
		if out, err := exec.Command(pyBin, "--version").Output(); err == nil {
			v := parseVersion(string(out))
			pyItem.Version = v
			if isVersionOlder(v, "3.12") {
				pyItem.Found = false
				pyItem.ErrorMsg = fmt.Sprintf("Python %s is too old (>= 3.12 required for tar filters)", v)
			} else {
				pyItem.Found = true
			}
		}
	} else {
		pyItem.ErrorMsg = "Python 3 is not installed or not in PATH"
	}
	report.Items = append(report.Items, pyItem)

	// 3. C++ Compiler
	compilerItem := CheckItem{
		ID:       "compiler",
		Name:     "C++ Compiler (GCC / Clang / MSVC)",
		Category: "Compiler",
		Required: true,
	}
	checkCompilers(&compilerItem)
	report.Items = append(report.Items, compilerItem)

	// 4. Ninja (Recommended) or Make
	ninjaItem := CheckItem{
		ID:       "ninja",
		Name:     "Ninja Build System",
		Category: "Tool",
		Required: false,
	}
	if path, err := exec.LookPath("ninja"); err == nil {
		ninjaItem.Found = true
		ninjaItem.Path = path
		if out, err := exec.Command("ninja", "--version").Output(); err == nil {
			ninjaItem.Version = strings.TrimSpace(string(out))
		}
	} else {
		ninjaItem.ErrorMsg = "Optional (speeds up compilation significantly)"
	}
	report.Items = append(report.Items, ninjaItem)

	// 5. Vulkan Headers / SDK
	vulkanItem := CheckItem{
		ID:       "vulkan",
		Name:     "Vulkan Headers / SDK",
		Category: "Library",
		Required: true,
	}
	checkVulkan(&vulkanItem)
	report.Items = append(report.Items, vulkanItem)

	// 6. Linux specific libraries
	if runtime.GOOS == "linux" {
		// zlib
		zlibItem := CheckItem{
			ID:       "zlib",
			Name:     "zlib development headers",
			Category: "Library",
			Required: true,
		}
		if fileExists("/usr/include/zlib.h") || fileExists("/usr/local/include/zlib.h") || pkgConfigExists("zlib") {
			zlibItem.Found = true
		} else {
			zlibItem.ErrorMsg = "Missing zlib headers (install zlib / zlib-devel / zlib1g-dev)"
		}
		report.Items = append(report.Items, zlibItem)

		// libcurl
		curlItem := CheckItem{
			ID:       "libcurl",
			Name:     "libcurl development headers",
			Category: "Library",
			Required: true,
		}
		if fileExists("/usr/include/curl/curl.h") || fileExists("/usr/local/include/curl/curl.h") || pkgConfigExists("libcurl") {
			curlItem.Found = true
		} else {
			curlItem.ErrorMsg = "Missing libcurl headers (install libcurl / curl-devel / libcurl4-openssl-dev)"
		}
		report.Items = append(report.Items, curlItem)

		// X11 / Wayland
		displayItem := CheckItem{
			ID:       "display_headers",
			Name:     "X11 / Wayland development headers",
			Category: "Library",
			Required: false,
		}
		if fileExists("/usr/include/X11/Xlib.h") || fileExists("/usr/include/wayland-client.h") {
			displayItem.Found = true
		} else {
			displayItem.ErrorMsg = "Missing X11/Wayland headers; SDL2 may build without a window backend"
		}
		report.Items = append(report.Items, displayItem)
	}

	// 7. Graphics Hardware & HD Texturing capability
	gpu := DetectGPU()
	gpuItem := CheckItem{
		ID:       "gpu_nvidia",
		Name:     "Graphics Card (GPU)",
		Category: "Hardware",
		Required: false,
		Found:    true,
		Version:  gpu.Name,
	}
	if gpu.IsNvidia {
		gpuItem.Name = "NVIDIA GPU (CUDA / HD Textures)"
		gpuItem.ErrorMsg = "Compatible NVIDIA GPU detected: " + gpu.Name
	} else {
		gpuItem.Name = "Graphics Card (GPU)"
		gpuItem.ErrorMsg = gpu.StatusMessage
	}
	report.Items = append(report.Items, gpuItem)

	// Calculate overall required status
	report.AllRequiredMet = true
	for _, item := range report.Items {
		if item.Required && !item.Found {
			report.AllRequiredMet = false
			break
		}
	}

	report.InstallScript = generateInstallCommand(report.Distro, report.Items)
	return report
}

func checkCompilers(item *CheckItem) {
	compilers := []string{"g++", "clang++", "cl"}
	for _, c := range compilers {
		if path, err := exec.LookPath(c); err == nil {
			item.Found = true
			item.Path = path
			if out, err := exec.Command(c, "--version").Output(); err == nil {
				lines := strings.Split(string(out), "\n")
				if len(lines) > 0 {
					item.Version = strings.TrimSpace(lines[0])
				}
			} else {
				item.Version = c
			}
			return
		}
	}
	item.ErrorMsg = "No C++ compiler found (install g++, clang, or MSVC)"
}

func checkVulkan(item *CheckItem) {
	if os.Getenv("VULKAN_SDK") != "" {
		item.Found = true
		item.Path = os.Getenv("VULKAN_SDK")
		item.Version = "SDK"
		return
	}
	// Check pkg-config or cmake package config
	if pkgConfigExists("vulkan") || pkgConfigExists("VulkanHeaders") {
		item.Found = true
		item.Version = "System Headers"
		return
	}
	// Common system header paths
	paths := []string{
		"/usr/include/vulkan/vulkan.h",
		"/usr/local/include/vulkan/vulkan.h",
		"/usr/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake",
		"/usr/lib/cmake/VulkanHeaders/VulkanHeadersConfig.cmake",
	}
	for _, p := range paths {
		if fileExists(p) {
			item.Found = true
			item.Path = filepath.Dir(p)
			return
		}
	}
	item.ErrorMsg = "Vulkan Headers not found (install vulkan-headers / Vulkan-Headers)"
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func pkgConfigExists(pkg string) bool {
	cmd := exec.Command("pkg-config", "--exists", pkg)
	return cmd.Run() == nil
}

func parseVersion(str string) string {
	re := regexp.MustCompile(`[0-9]+(\.[0-9]+)+`)
	match := re.FindString(str)
	return match
}

func isVersionOlder(curr, req string) bool {
	currParts := strings.Split(curr, ".")
	reqParts := strings.Split(req, ".")
	for i := 0; i < len(currParts) && i < len(reqParts); i++ {
		c, _ := strconv.Atoi(currParts[i])
		r, _ := strconv.Atoi(reqParts[i])
		if c < r {
			return true
		}
		if c > r {
			return false
		}
	}
	return len(currParts) < len(reqParts)
}

func generateInstallCommand(distro string, items []CheckItem) string {
	var missing []string
	for _, it := range items {
		if !it.Found {
			missing = append(missing, it.ID)
		}
	}
	if len(missing) == 0 {
		return ""
	}

	switch distro {
	case "arch":
		return "sudo pacman -S --needed cmake gcc ninja python vulkan-headers zlib curl libx11"
	case "debian":
		return "sudo apt update && sudo apt install -y cmake g++ ninja-build python3 curl libvulkan-dev zlib1g-dev libcurl4-openssl-dev libx11-dev"
	case "fedora":
		return "sudo dnf install -y cmake gcc-c++ ninja-build python3 curl vulkan-headers zlib-devel libcurl-devel libX11-devel"
	case "windows":
		return "winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.12 LLVM.LLVM LunarG.VulkanSDK"
	default:
		return "Please install cmake, g++/clang++, ninja, python3 (>=3.12), vulkan-headers, zlib, libcurl"
	}
}

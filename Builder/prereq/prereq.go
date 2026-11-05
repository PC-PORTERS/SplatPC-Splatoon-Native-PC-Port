package prereq

import (
	"os"
	"os/exec"
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
	// Proactively register discovered tool directories into PATH and environment
	EnsureEnvironment()

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
	if path, ver, ok := FindCMake(); ok {
		cmakeItem.Found = true
		cmakeItem.Path = path
		cmakeItem.Version = ver
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
	if py, ok := FindPython(); ok {
		pyItem.Found = true
		pyItem.Path = py.Path
		pyItem.Version = py.Version
	} else {
		pyItem.ErrorMsg = "Python 3 (>= 3.12) is not installed or not in PATH"
	}
	report.Items = append(report.Items, pyItem)

	// 3. C++ Compiler
	compilerItem := CheckItem{
		ID:       "compiler",
		Name:     "C++ Compiler (GCC / Clang / MSVC)",
		Category: "Compiler",
		Required: true,
	}
	name, path, ver, ok, errMsg := FindCompiler()
	if ok {
		compilerItem.Found = true
		compilerItem.Name = name
		compilerItem.Path = path
		compilerItem.Version = ver
	} else {
		compilerItem.ErrorMsg = errMsg
	}
	report.Items = append(report.Items, compilerItem)

	// 4. Ninja (Recommended) or Make
	ninjaItem := CheckItem{
		ID:       "ninja",
		Name:     "Ninja Build System",
		Category: "Tool",
		Required: false,
	}
	if path, ver, ok := FindNinja(); ok {
		ninjaItem.Found = true
		ninjaItem.Path = path
		ninjaItem.Version = ver
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
	if path, ver, ok := FindVulkan(); ok {
		vulkanItem.Found = true
		vulkanItem.Path = path
		vulkanItem.Version = ver
	} else {
		vulkanItem.ErrorMsg = "Vulkan Headers / SDK not found"
	}
	report.Items = append(report.Items, vulkanItem)

	// 6. Windows-specific tools and dependencies
	if runtime.GOOS == "windows" {
		gitItem := CheckItem{
			ID:       "git",
			Name:     "Git Version Control",
			Category: "Tool",
			Required: true,
		}
		if path, ver, ok := FindGit(); ok {
			gitItem.Found = true
			gitItem.Path = path
			gitItem.Version = ver
		} else {
			gitItem.ErrorMsg = "Git is required to fetch vcpkg dependencies (install Git for Windows)"
		}
		report.Items = append(report.Items, gitItem)

		_, tc, foundVcpkg := FindVcpkg("")
		hasPkgs := CheckVcpkgPackages(tc)
		vcpkgItem := CheckItem{
			ID:       "vcpkg_libs",
			Name:     "Windows Libraries (zlib, curl via vcpkg)",
			Category: "Library",
			Required: false,
		}
		if foundVcpkg && hasPkgs {
			vcpkgItem.Found = true
			vcpkgItem.Version = "Packages installed"
		} else {
			vcpkgItem.Found = true
			vcpkgItem.Version = "Auto-configured during build"
		}
		report.Items = append(report.Items, vcpkgItem)
	}

	// 7. Linux specific libraries
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

	// 8. Graphics Hardware & HD Texturing capability
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
		var wingetPkgs []string
		for _, id := range missing {
			switch id {
			case "cmake":
				wingetPkgs = append(wingetPkgs, "Kitware.CMake")
			case "python3":
				wingetPkgs = append(wingetPkgs, "Python.Python.3.12")
			case "ninja":
				wingetPkgs = append(wingetPkgs, "Ninja-build.Ninja")
			case "git":
				wingetPkgs = append(wingetPkgs, "Git.Git")
			case "vulkan":
				wingetPkgs = append(wingetPkgs, "LunarG.VulkanSDK")
			case "compiler":
				wingetPkgs = append(wingetPkgs, "Microsoft.VisualStudio.2022.BuildTools")
			}
		}
		if len(wingetPkgs) > 0 {
			return "winget install " + strings.Join(wingetPkgs, " ")
		}
		return "winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.12 LunarG.VulkanSDK"
	default:
		return "Please install cmake, g++/clang++/MSVC, ninja, python3 (>=3.12), vulkan-headers, zlib, libcurl"
	}
}

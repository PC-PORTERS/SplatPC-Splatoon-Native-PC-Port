package prereq

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"

	"splatpc-builder/proc"
)

type CheckItem struct {
	ID           string
	Name         string
	Category     string // "Tool", "Compiler", "Library"
	Required     bool
	Found        bool
	Userspace    bool
	Downloadable bool
	Version      string
	Path         string
	ErrorMsg     string
	InstallCmd   string
}

type CheckReport struct {
	Items               []CheckItem
	AllRequiredMet      bool
	MissingDownloadable []string
	OSName              string
	Distro              string
	// InstallSteps install the missing required tools on this system their
	// commands one per line are InstallScripts
	InstallSteps  []InstallStep
	InstallScript string
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

	toolsRoot := UserToolsDir()

	// 1. CMake
	cmakeItem := CheckItem{
		ID:           "cmake",
		Name:         "CMake (>= 3.20)",
		Category:     "Tool",
		Required:     true,
		Downloadable: true,
	}
	checkCMake(&cmakeItem, toolsRoot)
	report.Items = append(report.Items, cmakeItem)

	// 2. Python 3
	pyItem := CheckItem{
		ID:           "python3",
		Name:         "Python 3 (>= 3.12)",
		Category:     "Tool",
		Required:     true,
		Downloadable: true,
	}
	if python, versionLine, ok := FindPython(); ok {
		pyItem.Path = strings.Join(python, " ")
		v := parseVersion(versionLine)
		pyItem.Version = v
		if isVersionOlder(v, "3.12") {
			pyItem.ErrorMsg = fmt.Sprintf("Python %s is too old (>= 3.12 required for tar filters)", v)
		} else {
			pyItem.Found = true
			if isUnderDir(python[0], toolsRoot) {
				pyItem.Userspace = true
				pyItem.Version += " (userspace)"
			}
		}
	} else {
		pyItem.ErrorMsg = "Python 3 is not installed or not in PATH (python3, python and py -3 were tried)"
	}
	report.Items = append(report.Items, pyItem)

	// 3. C++ Compiler
	compilerItem := CheckItem{
		ID:           "compiler",
		Name:         "C++ Compiler (GCC / Clang / MSVC)",
		Category:     "Compiler",
		Required:     true,
		Downloadable: true,
	}
	checkCompilers(&compilerItem, toolsRoot)
	report.Items = append(report.Items, compilerItem)

	// 4. Ninja (Recommended) or Make
	ninjaItem := CheckItem{
		ID:           "ninja",
		Name:         "Ninja Build System",
		Category:     "Tool",
		Required:     false,
		Downloadable: true,
	}
	if path, ok := LookPath("ninja"); ok {
		ninjaItem.Found = true
		ninjaItem.Path = path
		if out, err := proc.Command(path, "--version").Output(); err == nil {
			ninjaItem.Version = strings.TrimSpace(string(out))
		}
		if isUnderDir(path, toolsRoot) {
			ninjaItem.Userspace = true
			if ninjaItem.Version != "" {
				ninjaItem.Version += " (userspace)"
			} else {
				ninjaItem.Version = "userspace"
			}
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

	// 6. Linux specific libraries & tools
	if runtime.GOOS == "linux" {
		// appimagetool (Optional for AppImage packaging)
		appImageItem := CheckItem{
			ID:           "appimagetool",
			Name:         "AppImageTool (Linux AppImage)",
			Category:     "Tool",
			Required:     false,
			Downloadable: true,
		}
		if path, ok := LookPath("appimagetool"); ok {
			appImageItem.Found = true
			appImageItem.Path = path
			if isUnderDir(path, toolsRoot) {
				appImageItem.Userspace = true
				appImageItem.Version = "userspace"
			} else {
				appImageItem.Version = "System"
			}
		} else {
			appImageItem.ErrorMsg = "Optional (needed for standalone Linux AppImage packaging)"
		}
		report.Items = append(report.Items, appImageItem)

		// zlib (built in userspace when absent on system)
		zlibItem := CheckItem{
			ID:       "zlib",
			Name:     "zlib development headers",
			Category: "Library",
			Required: true,
			Found:    true,
		}
		if HasSystemZlib() && !UsingUserspaceCompiler() {
			zlibItem.Version = "System"
		} else {
			zlibItem.Userspace = true
			zlibItem.Version = "built by the Builder (userspace)"
		}
		report.Items = append(report.Items, zlibItem)

		// libcurl (built in userspace when absent on system)
		curlItem := CheckItem{
			ID:       "libcurl",
			Name:     "libcurl development headers",
			Category: "Library",
			Required: true,
			Found:    true,
		}
		if HasSystemCurl() && !UsingUserspaceCompiler() {
			curlItem.Version = "System"
		} else {
			curlItem.Userspace = true
			curlItem.Version = "built by the Builder (userspace)"
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
	report.MissingDownloadable = DownloadableMissingTools(report)

	missing := missingRequired(report.Items)
	var partialVS *VisualStudio
	if runtime.GOOS == "windows" && slices.Contains(missing, "compiler") {
		if vs, ok := findVisualStudioWithoutCpp(); ok {
			partialVS = &vs
		}
	}
	report.InstallSteps = installSteps(report.Distro, missing, partialVS)
	report.InstallScript = installScript(report.InstallSteps, missing)
	return report
}

func checkCMake(item *CheckItem, toolsRoot string) {
	var candidates []string
	if path, ok := LookPath("cmake"); ok {
		candidates = append(candidates, path)
	}
	if !UserspaceOnly() {
		if sysPath, err := exec.LookPath("cmake"); err == nil && !slices.Contains(candidates, sysPath) {
			candidates = append(candidates, sysPath)
		}
	}
	if len(candidates) == 0 {
		item.ErrorMsg = "CMake is not installed or not in PATH"
		return
	}
	var vs VisualStudio
	var hasVS bool
	if runtime.GOOS == "windows" && !UserspaceOnly() {
		vs, hasVS = FindVisualStudio()
	}

	var firstErr string
	var firstPath string
	var firstVer string
	for _, path := range candidates {
		out, err := proc.Command(path, "--version").Output()
		if err != nil {
			continue
		}
		v := parseVersion(string(out))
		if firstPath == "" {
			firstPath = path
			firstVer = v
		}
		if isVersionOlder(v, "3.20") {
			if firstErr == "" {
				firstErr = fmt.Sprintf("CMake %s is too old (>= 3.20 required)", v)
			}
			continue
		}
		if hasVS {
			if msg := cmakeSupportsVisualStudio(v, vs); msg != "" {
				if firstErr == "" {
					firstErr = msg
				}
				continue
			}
		}
		item.Found = true
		item.Path = path
		item.Version = v
		if isUnderDir(path, toolsRoot) {
			item.Userspace = true
			item.Version += " (userspace)"
		}
		return
	}
	item.Path = firstPath
	item.Version = firstVer
	if firstErr != "" {
		item.ErrorMsg = firstErr
	} else {
		item.ErrorMsg = "CMake could not be executed"
	}
}

func checkCompilers(item *CheckItem, toolsRoot string) {
	if !UserspaceOnly() {
		// Prefer system compilers or Visual Studio first; fall back to userspace toolchain.
		compilers := []string{"g++", "clang++", "cl"}
		for _, c := range compilers {
			if path, err := exec.LookPath(c); err == nil && !isUnderDir(path, toolsRoot) {
				setCompilerFound(item, path, c, false)
				return
			}
		}
		if runtime.GOOS == "windows" {
			if vs, ok := FindVisualStudio(); ok {
				item.Found = true
				item.Path = vs.Path
				item.Version = "MSVC (" + vs.DisplayName + " " + vs.Version + ")"
				return
			}
		}
	}
	if path, ok := LookPath("clang++"); ok && isUnderDir(path, toolsRoot) {
		setCompilerFound(item, path, "clang++", true)
		return
	}
	if runtime.GOOS == "windows" {
		item.ErrorMsg = "No C++ compiler found (Visual Studio Build Tools or userspace LLVM-MinGW)"
		return
	}
	item.ErrorMsg = "No C++ compiler found (system g++/clang++ or userspace Zig toolchain)"
}

func setCompilerFound(item *CheckItem, path, cmdName string, userspace bool) {
	item.Found = true
	item.Path = path
	item.Userspace = userspace
	if out, err := proc.Command(path, "--version").Output(); err == nil {
		lines := strings.Split(string(out), "\n")
		if len(lines) > 0 {
			item.Version = strings.TrimSpace(lines[0])
		}
	}
	if item.Version == "" {
		item.Version = cmdName
	}
	if userspace {
		item.Version += " (userspace)"
	}
}

// HasSystemVulkan reports whether Vulkan headers are available from the OS or VULKAN_SDK.
func HasSystemVulkan() bool {
	if runtime.GOOS == "windows" || UserspaceOnly() {
		return false
	}
	if os.Getenv("VULKAN_SDK") != "" {
		return true
	}
	if pkgConfigExists("vulkan") || pkgConfigExists("VulkanHeaders") {
		return true
	}
	paths := []string{
		"/usr/include/vulkan/vulkan.h",
		"/usr/local/include/vulkan/vulkan.h",
		"/usr/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake",
		"/usr/lib/cmake/VulkanHeaders/VulkanHeadersConfig.cmake",
	}
	for _, p := range paths {
		if fileExists(p) {
			return true
		}
	}
	return false
}

// HasSystemZlib reports whether zlib development headers are present on the OS.
func HasSystemZlib() bool {
	if runtime.GOOS != "linux" || UserspaceOnly() {
		return false
	}
	return fileExists("/usr/include/zlib.h") || fileExists("/usr/local/include/zlib.h") || pkgConfigExists("zlib")
}

// HasSystemCurl reports whether libcurl development headers are present on the OS.
func HasSystemCurl() bool {
	if runtime.GOOS != "linux" || UserspaceOnly() {
		return false
	}
	return fileExists("/usr/include/curl/curl.h") || fileExists("/usr/local/include/curl/curl.h") || pkgConfigExists("libcurl")
}

func checkVulkan(item *CheckItem) {
	if runtime.GOOS == "windows" {
		// The Builder downloads and builds Vulkan-Headers itself on Windows.
		item.Found = true
		item.Userspace = true
		item.Version = "built by the Builder (userspace)"
		return
	}
	if !UsingUserspaceCompiler() {
		if os.Getenv("VULKAN_SDK") != "" {
			item.Found = true
			item.Path = os.Getenv("VULKAN_SDK")
			item.Version = "SDK"
			return
		}
		if pkgConfigExists("vulkan") || pkgConfigExists("VulkanHeaders") {
			item.Found = true
			item.Version = "System Headers"
			return
		}
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
				item.Version = "System Headers"
				return
			}
		}
	}
	item.Found = true
	item.Userspace = true
	item.Version = "built by the Builder (userspace)"
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func pkgConfigExists(pkg string) bool {
	cmd := proc.Command("pkg-config", "--exists", pkg)
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

package prereq

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

type PythonInfo struct {
	Executable string
	Args       []string
	Version    string
	Path       string
}

func (p PythonInfo) Command(ctx context.Context, args ...string) *exec.Cmd {
	fullArgs := append(append([]string{}, p.Args...), args...)
	if ctx != nil {
		return exec.CommandContext(ctx, p.Executable, fullArgs...)
	}
	return exec.Command(p.Executable, fullArgs...)
}

type vsWhereEntry struct {
	InstanceID          string `json:"instanceId"`
	InstallationPath    string `json:"installationPath"`
	InstallationVersion string `json:"installationVersion"`
	DisplayName         string `json:"displayName"`
	ProductPath         string `json:"productPath"`
}

var (
	cachedVS        *vsWhereEntry
	cachedVSChecked bool
	cachedPython    *PythonInfo
	cachedCMake     string
	cachedNinja     string
)

func findVSInstallation() (vsWhereEntry, bool) {
	if cachedVSChecked {
		if cachedVS != nil {
			return *cachedVS, true
		}
		return vsWhereEntry{}, false
	}
	cachedVSChecked = true

	if runtime.GOOS != "windows" {
		return vsWhereEntry{}, false
	}

	progFiles86 := os.Getenv("ProgramFiles(x86)")
	progFiles := os.Getenv("ProgramFiles")

	vswhereCandidates := []string{
		filepath.Join(progFiles86, "Microsoft Visual Studio", "Installer", "vswhere.exe"),
		filepath.Join(progFiles, "Microsoft Visual Studio", "Installer", "vswhere.exe"),
		`C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`,
		`C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe`,
	}

	for _, vswhere := range vswhereCandidates {
		if vswhere != "" && fileExists(vswhere) {
			cmd := exec.Command(vswhere, "-version", "[17.0,)", "-latest", "-products", "*", "-format", "json")
			out, err := cmd.Output()
			if err == nil && len(out) > 0 {
				var entries []vsWhereEntry
				if json.Unmarshal(out, &entries) == nil && len(entries) > 0 {
					cachedVS = &entries[0]
					return entries[0], true
				}
			}
		}
	}

	// Fallback scan of typical Visual Studio 2022 directories
	vsRoots := []string{
		filepath.Join(progFiles, "Microsoft Visual Studio", "2022"),
		filepath.Join(progFiles86, "Microsoft Visual Studio", "2022"),
		`C:\Program Files\Microsoft Visual Studio\2022`,
		`C:\Program Files (x86)\Microsoft Visual Studio\2022`,
	}
	editions := []string{"Community", "Professional", "Enterprise", "BuildTools"}
	for _, root := range vsRoots {
		if root == "" {
			continue
		}
		for _, ed := range editions {
			p := filepath.Join(root, ed)
			if fileExists(p) {
				entry := vsWhereEntry{
					InstallationPath:    p,
					InstallationVersion: "17.0",
					DisplayName:         "Visual Studio 2022 " + ed,
				}
				cachedVS = &entry
				return entry, true
			}
		}
	}

	return vsWhereEntry{}, false
}

func vsHasVCTools(vsPath string) (bool, string) {
	msvcRoot := filepath.Join(vsPath, "VC", "Tools", "MSVC")
	if entries, err := os.ReadDir(msvcRoot); err == nil && len(entries) > 0 {
		for i := len(entries) - 1; i >= 0; i-- {
			if entries[i].IsDir() {
				ver := entries[i].Name()
				clPath := filepath.Join(msvcRoot, ver, "bin", "Hostx64", "x64", "cl.exe")
				if fileExists(clPath) {
					return true, clPath
				}
				return true, filepath.Join(msvcRoot, ver)
			}
		}
	}
	return false, ""
}

func FindCompiler() (displayName string, path string, version string, found bool, errReason string) {
	if runtime.GOOS == "windows" {
		if vs, ok := findVSInstallation(); ok {
			hasVC, vcPath := vsHasVCTools(vs.InstallationPath)
			if hasVC {
				return "Visual Studio 2022 (MSVC)", vs.InstallationPath, vs.DisplayName + " (" + vs.InstallationVersion + ")", true, ""
			}
			return "Visual Studio 2022 (MSVC missing)", vcPath, "", false,
				fmt.Sprintf("Found %s, but 'Desktop development with C++' workload is missing. Add it via Visual Studio Installer.", vs.DisplayName)
		}

		if p, err := exec.LookPath("cl"); err == nil {
			return "MSVC (cl.exe)", p, "cl.exe", true, ""
		}

		clangCandidates := []string{"clang++", `C:\Program Files\LLVM\bin\clang++.exe`}
		for _, cand := range clangCandidates {
			if p, err := exec.LookPath(cand); err == nil || fileExists(cand) {
				if err == nil {
					cand = p
				}
				out, err := exec.Command(cand, "--version").Output()
				v := "clang++"
				if err == nil {
					lines := strings.Split(string(out), "\n")
					if len(lines) > 0 {
						v = strings.TrimSpace(lines[0])
					}
				}
				return "LLVM Clang++", cand, v, true, ""
			}
		}

		gccCandidates := []string{"g++", `C:\msys64\ucrt64\bin\g++.exe`, `C:\mingw64\bin\g++.exe`}
		for _, cand := range gccCandidates {
			if p, err := exec.LookPath(cand); err == nil || fileExists(cand) {
				if err == nil {
					cand = p
				}
				out, err := exec.Command(cand, "--version").Output()
				v := "g++"
				if err == nil {
					lines := strings.Split(string(out), "\n")
					if len(lines) > 0 {
						v = strings.TrimSpace(lines[0])
					}
				}
				return "GCC (g++)", cand, v, true, ""
			}
		}

		return "C++ Compiler", "", "", false, "No C++ compiler found (install Visual Studio 2022 with C++ workload or LLVM)"
	}

	compilers := []string{"g++", "clang++"}
	for _, c := range compilers {
		if path, err := exec.LookPath(c); err == nil {
			v := c
			if out, err := exec.Command(c, "--version").Output(); err == nil {
				lines := strings.Split(string(out), "\n")
				if len(lines) > 0 {
					v = strings.TrimSpace(lines[0])
				}
			}
			return c, path, v, true, ""
		}
	}
	return "C++ Compiler", "", "", false, "No C++ compiler found (install g++ or clang++)"
}

func FindCMake() (string, string, bool) {
	if cachedCMake != "" && fileExists(cachedCMake) {
		if out, err := exec.Command(cachedCMake, "--version").Output(); err == nil {
			v := parseVersion(string(out))
			if !isVersionOlder(v, "3.20") {
				return cachedCMake, v, true
			}
		}
	}

	candidates := []string{}
	if p, err := exec.LookPath("cmake"); err == nil {
		candidates = append(candidates, p)
	}

	if runtime.GOOS == "windows" {
		if vs, ok := findVSInstallation(); ok {
			candidates = append(candidates, filepath.Join(vs.InstallationPath, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe"))
		}
		progFiles := os.Getenv("ProgramFiles")
		progFiles86 := os.Getenv("ProgramFiles(x86)")
		localApp := os.Getenv("LOCALAPPDATA")
		candidates = append(candidates,
			filepath.Join(progFiles, "CMake", "bin", "cmake.exe"),
			filepath.Join(progFiles86, "CMake", "bin", "cmake.exe"),
			filepath.Join(localApp, "Programs", "CMake", "bin", "cmake.exe"),
			`C:\Program Files\CMake\bin\cmake.exe`,
			`C:\Program Files (x86)\CMake\bin\cmake.exe`,
		)
	}

	for _, cand := range candidates {
		if cand == "" || !fileExists(cand) {
			continue
		}
		out, err := exec.Command(cand, "--version").Output()
		if err == nil {
			v := parseVersion(string(out))
			if !isVersionOlder(v, "3.20") {
				cachedCMake = cand
				return cand, v, true
			}
		}
	}
	return "", "", false
}

func FindNinja() (string, string, bool) {
	if cachedNinja != "" && fileExists(cachedNinja) {
		if out, err := exec.Command(cachedNinja, "--version").Output(); err == nil {
			return cachedNinja, strings.TrimSpace(string(out)), true
		}
	}

	candidates := []string{}
	if p, err := exec.LookPath("ninja"); err == nil {
		candidates = append(candidates, p)
	}

	if runtime.GOOS == "windows" {
		if vs, ok := findVSInstallation(); ok {
			candidates = append(candidates, filepath.Join(vs.InstallationPath, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja", "ninja.exe"))
		}
		progFiles := os.Getenv("ProgramFiles")
		localApp := os.Getenv("LOCALAPPDATA")
		candidates = append(candidates,
			filepath.Join(progFiles, "Ninja", "ninja.exe"),
			filepath.Join(localApp, "Programs", "ninja", "ninja.exe"),
			`C:\Program Files\Ninja\ninja.exe`,
			`C:\ProgramData\chocolatey\bin\ninja.exe`,
			`C:\msys64\ucrt64\bin\ninja.exe`,
			`C:\msys64\mingw64\bin\ninja.exe`,
		)
	}

	for _, cand := range candidates {
		if cand == "" || !fileExists(cand) {
			continue
		}
		out, err := exec.Command(cand, "--version").Output()
		if err == nil {
			cachedNinja = cand
			return cand, strings.TrimSpace(string(out)), true
		}
	}
	return "", "", false
}

func FindPython() (PythonInfo, bool) {
	if cachedPython != nil {
		return *cachedPython, true
	}

	type cand struct {
		exe  string
		args []string
	}
	candidates := []cand{}

	if p, err := exec.LookPath("python3"); err == nil {
		candidates = append(candidates, cand{exe: p})
	}
	if p, err := exec.LookPath("python"); err == nil {
		candidates = append(candidates, cand{exe: p})
	}
	if p, err := exec.LookPath("py"); err == nil {
		candidates = append(candidates, cand{exe: p, args: []string{"-3"}})
	}

	if runtime.GOOS == "windows" {
		candidates = append(candidates,
			cand{exe: `C:\Windows\py.exe`, args: []string{"-3"}},
			cand{exe: `C:\Windows\System32\py.exe`, args: []string{"-3"}},
		)

		localApp := os.Getenv("LOCALAPPDATA")
		progFiles := os.Getenv("ProgramFiles")
		progFiles86 := os.Getenv("ProgramFiles(x86)")

		for _, v := range []string{"Python314", "Python313", "Python312"} {
			if localApp != "" {
				candidates = append(candidates, cand{exe: filepath.Join(localApp, "Programs", "Python", v, "python.exe")})
			}
			if progFiles != "" {
				candidates = append(candidates, cand{exe: filepath.Join(progFiles, v, "python.exe")})
				candidates = append(candidates, cand{exe: filepath.Join(progFiles, "Python", v, "python.exe")})
			}
			if progFiles86 != "" {
				candidates = append(candidates, cand{exe: filepath.Join(progFiles86, v, "python.exe")})
			}
			candidates = append(candidates, cand{exe: filepath.Join("C:\\", v, "python.exe")})
		}

		if localApp != "" {
			candidates = append(candidates,
				cand{exe: filepath.Join(localApp, "Microsoft", "WindowsApps", "python3.exe")},
				cand{exe: filepath.Join(localApp, "Microsoft", "WindowsApps", "python.exe")},
			)
		}
	}

	for _, c := range candidates {
		if c.exe == "" {
			continue
		}
		if filepath.IsAbs(c.exe) && !fileExists(c.exe) {
			continue
		}

		cmdArgs := append(append([]string{}, c.args...), "-c", "import sys; print(f'{sys.version_info[0]}.{sys.version_info[1]}.{sys.version_info[2]}')")
		out, err := exec.Command(c.exe, cmdArgs...).Output()
		if err == nil {
			v := strings.TrimSpace(string(out))
			if !isVersionOlder(v, "3.12") {
				realPath := c.exe
				if p, err := exec.LookPath(c.exe); err == nil {
					realPath = p
				}
				info := PythonInfo{
					Executable: c.exe,
					Args:       c.args,
					Version:    v,
					Path:       realPath,
				}
				cachedPython = &info
				return info, true
			}
		}
	}

	return PythonInfo{}, false
}

func FindGit() (string, string, bool) {
	candidates := []string{}
	if p, err := exec.LookPath("git"); err == nil {
		candidates = append(candidates, p)
	}

	if runtime.GOOS == "windows" {
		progFiles := os.Getenv("ProgramFiles")
		progFiles86 := os.Getenv("ProgramFiles(x86)")
		localApp := os.Getenv("LOCALAPPDATA")
		candidates = append(candidates,
			filepath.Join(progFiles, "Git", "cmd", "git.exe"),
			filepath.Join(progFiles, "Git", "bin", "git.exe"),
			filepath.Join(localApp, "Programs", "Git", "cmd", "git.exe"),
			filepath.Join(progFiles86, "Git", "cmd", "git.exe"),
			`C:\Program Files\Git\cmd\git.exe`,
		)
	}

	for _, cand := range candidates {
		if cand == "" || !fileExists(cand) {
			continue
		}
		out, err := exec.Command(cand, "--version").Output()
		if err == nil {
			return cand, parseVersion(string(out)), true
		}
	}
	return "", "", false
}

func FindVulkan() (string, string, bool) {
	if env := os.Getenv("VULKAN_SDK"); env != "" {
		h1 := filepath.Join(env, "Include", "vulkan", "vulkan.h")
		h2 := filepath.Join(env, "include", "vulkan", "vulkan.h")
		if fileExists(h1) || fileExists(h2) {
			return env, "SDK", true
		}
	}

	if runtime.GOOS == "windows" {
		roots := []string{`C:\VulkanSDK`, filepath.Join(os.Getenv("ProgramFiles"), "VulkanSDK")}
		for _, root := range roots {
			if root == "" {
				continue
			}
			if entries, err := os.ReadDir(root); err == nil && len(entries) > 0 {
				for i := len(entries) - 1; i >= 0; i-- {
					if entries[i].IsDir() {
						sdkDir := filepath.Join(root, entries[i].Name())
						h1 := filepath.Join(sdkDir, "Include", "vulkan", "vulkan.h")
						h2 := filepath.Join(sdkDir, "include", "vulkan", "vulkan.h")
						if fileExists(h1) || fileExists(h2) {
							return sdkDir, entries[i].Name(), true
						}
					}
				}
			}
		}
	} else {
		if pkgConfigExists("vulkan") || pkgConfigExists("VulkanHeaders") {
			return "pkg-config", "System Headers", true
		}
		paths := []string{
			"/usr/include/vulkan/vulkan.h",
			"/usr/local/include/vulkan/vulkan.h",
			"/usr/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake",
			"/usr/lib/cmake/VulkanHeaders/VulkanHeadersConfig.cmake",
		}
		for _, p := range paths {
			if fileExists(p) {
				return filepath.Dir(p), "System Headers", true
			}
		}
	}

	return "", "", false
}

func FindVcpkg(repoRoot string) (vcpkgExe string, toolchainPath string, found bool) {
	candidates := []string{
		os.Getenv("VCPKG_ROOT"),
		os.Getenv("VCPKG_INSTALLATION_ROOT"),
		filepath.Join(repoRoot, "build", "vcpkg"),
		filepath.Join(os.Getenv("LOCALAPPDATA"), "SplatoonPort", "vcpkg"),
		`C:\vcpkg`,
	}
	if p, err := exec.LookPath("vcpkg"); err == nil {
		candidates = append([]string{filepath.Dir(p)}, candidates...)
	}

	for _, cand := range candidates {
		if cand == "" {
			continue
		}
		tc := filepath.Join(cand, "scripts", "buildsystems", "vcpkg.cmake")
		if fileExists(tc) {
			exe := filepath.Join(cand, "vcpkg.exe")
			if !fileExists(exe) {
				exe = filepath.Join(cand, "vcpkg")
			}
			return exe, tc, true
		}
	}
	return "", "", false
}

func CheckVcpkgPackages(toolchainPath string) bool {
	if toolchainPath == "" {
		return false
	}
	vcpkgDir := filepath.Dir(filepath.Dir(filepath.Dir(toolchainPath)))
	zlibHeader := filepath.Join(vcpkgDir, "installed", "x64-windows", "include", "zlib.h")
	curlHeader := filepath.Join(vcpkgDir, "installed", "x64-windows", "include", "curl", "curl.h")
	return fileExists(zlibHeader) && fileExists(curlHeader)
}

func PrependToPath(dirs ...string) {
	current := os.Getenv("PATH")
	sep := string(os.PathListSeparator)
	var toAdd []string
	for _, d := range dirs {
		if d == "" {
			continue
		}
		abs, err := filepath.Abs(d)
		if err == nil {
			d = abs
		}
		if !strings.Contains(current, d) {
			toAdd = append(toAdd, d)
		}
	}
	if len(toAdd) > 0 {
		newPath := strings.Join(toAdd, sep) + sep + current
		_ = os.Setenv("PATH", newPath)
	}
}

func EnsureEnvironment() {
	var dirsToAdd []string

	if cmakePath, _, ok := FindCMake(); ok && filepath.IsAbs(cmakePath) {
		dirsToAdd = append(dirsToAdd, filepath.Dir(cmakePath))
	}
	if ninjaPath, _, ok := FindNinja(); ok && filepath.IsAbs(ninjaPath) {
		dirsToAdd = append(dirsToAdd, filepath.Dir(ninjaPath))
	}
	if py, ok := FindPython(); ok && filepath.IsAbs(py.Path) {
		dirsToAdd = append(dirsToAdd, filepath.Dir(py.Path))
	}
	if gitPath, _, ok := FindGit(); ok && filepath.IsAbs(gitPath) {
		dirsToAdd = append(dirsToAdd, filepath.Dir(gitPath))
	}
	if vkPath, _, ok := FindVulkan(); ok && filepath.IsAbs(vkPath) {
		if runtime.GOOS == "windows" {
			_ = os.Setenv("VULKAN_SDK", vkPath)
			dirsToAdd = append(dirsToAdd, filepath.Join(vkPath, "Bin"))
		}
	}

	PrependToPath(dirsToAdd...)
}

func GetCMakePath() string {
	if path, _, ok := FindCMake(); ok {
		return path
	}
	return "cmake"
}

func GetNinjaPath() string {
	if path, _, ok := FindNinja(); ok {
		return path
	}
	return "ninja"
}

func GetPythonCommand(ctx context.Context, args ...string) *exec.Cmd {
	if py, ok := FindPython(); ok {
		return py.Command(ctx, args...)
	}
	if runtime.GOOS == "windows" {
		if ctx != nil {
			return exec.CommandContext(ctx, "python", args...)
		}
		return exec.Command("python", args...)
	}
	if ctx != nil {
		return exec.CommandContext(ctx, "python3", args...)
	}
	return exec.Command("python3", args...)
}

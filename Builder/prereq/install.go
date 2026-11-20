package prereq

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// InstallStep installs one missing tool.
type InstallStep struct {
	Name    string
	Command string
}

const wingetFlags = "-e --accept-source-agreements --accept-package-agreements"

// linuxPackages names the package that provides each check, per package manager.
var linuxPackages = map[string]map[string]string{
	"debian": {"cmake": "cmake", "python3": "python3", "compiler": "g++", "vulkan": "libvulkan-dev",
		"zlib": "zlib1g-dev", "libcurl": "libcurl4-openssl-dev"},
	"fedora": {"cmake": "cmake", "python3": "python3", "compiler": "gcc-c++", "vulkan": "vulkan-headers",
		"zlib": "zlib-devel", "libcurl": "libcurl-devel"},
	"arch": {"cmake": "cmake", "python3": "python", "compiler": "gcc", "vulkan": "vulkan-headers",
		"zlib": "zlib", "libcurl": "curl"},
	"opensuse": {"cmake": "cmake", "python3": "python3", "compiler": "gcc-c++", "vulkan": "vulkan-headers",
		"zlib": "zlib-devel", "libcurl": "libcurl-devel"},
}

var linuxInstall = map[string]string{
	"debian":   "sudo apt update && sudo apt install -y",
	"fedora":   "sudo dnf install -y",
	"arch":     "sudo pacman -S --needed --noconfirm",
	"opensuse": "sudo zypper install -y",
}

// missingRequired lists the IDs of the required checks that failed.
func missingRequired(items []CheckItem) []string {
	var ids []string
	for _, it := range items {
		if it.Required && !it.Found {
			ids = append(ids, it.ID)
		}
	}
	return ids
}

// installSteps returns what installs the missing tools on this system: winget
// on Windows, the distribution's package manager on Linux. partialVS is a
// Visual Studio without the C++ workload, which gets the workload added
// instead of a second install. Nil when nothing can be installed this way.
func installSteps(distro string, missing []string, partialVS *VisualStudio) []InstallStep {
	if len(missing) == 0 {
		return nil
	}
	if distro == "windows" {
		return windowsInstallSteps(missing, partialVS)
	}
	packages, known := linuxPackages[distro]
	if !known {
		return nil
	}
	var names []string
	for _, id := range missing {
		if name, ok := packages[id]; ok {
			names = append(names, name)
		}
	}
	if len(names) == 0 {
		return nil
	}
	return []InstallStep{{Name: strings.Join(names, ", "), Command: linuxInstall[distro] + " " + strings.Join(names, " ")}}
}

func windowsInstallSteps(missing []string, partialVS *VisualStudio) []InstallStep {
	var steps []InstallStep
	for _, id := range missing {
		switch id {
		case "cmake":
			steps = append(steps, InstallStep{"CMake", "winget install --id Kitware.CMake " + wingetFlags})
		case "python3":
			// PrependPath puts python.exe on PATH; the py launcher comes either way.
			steps = append(steps, InstallStep{"Python 3.13",
				"winget install --id Python.Python.3.13 " + wingetFlags + ` --override "/quiet PrependPath=1"`})
		case "compiler":
			if partialVS != nil {
				installer := filepath.Join(os.Getenv("ProgramFiles(x86)"), "Microsoft Visual Studio", "Installer", "setup.exe")
				steps = append(steps, InstallStep{"C++ tools for " + partialVS.DisplayName,
					fmt.Sprintf(`"%s" modify --installPath "%s" --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart`,
						installer, partialVS.Path)})
			} else {
				steps = append(steps, InstallStep{"Visual Studio 2022 Build Tools (C++)",
					"winget install --id Microsoft.VisualStudio.2022.BuildTools " + wingetFlags +
						` --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`})
			}
		}
	}
	return steps
}

// manualInstallHint is shown when no install command is known for this system.
const manualInstallHint = "Install with your package manager: cmake, a C++ compiler (g++ or clang++), python3 (3.12 or newer), " +
	"and the Vulkan headers, zlib and libcurl development packages"

func installScript(steps []InstallStep, missing []string) string {
	if len(steps) == 0 {
		if len(missing) == 0 {
			return ""
		}
		return manualInstallHint
	}
	lines := make([]string, len(steps))
	for i, s := range steps {
		lines[i] = s.Command
	}
	return strings.Join(lines, "\n")
}

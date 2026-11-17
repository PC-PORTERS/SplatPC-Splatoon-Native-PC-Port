package prereq

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const DefaultVisualStudioGenerator = "Visual Studio 17 2022"

// VisualStudio is an install of Visual Studio or of the Build Tools with the
// x64 C++ toolset.
type VisualStudio struct {
	Path        string `json:"installationPath"`
	Version     string `json:"installationVersion"`
	DisplayName string `json:"displayName"`
}

// FindVisualStudio asks vswhere for the newest install with the C++ toolset.
// cl.exe is only on PATH inside a Developer Command Prompt, vswhere always
// knows. "-products *" is needed to see Build Tools-only installs.
func FindVisualStudio() (VisualStudio, bool) {
	vswhere := filepath.Join(os.Getenv("ProgramFiles(x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
	out, err := exec.Command(vswhere, "-latest", "-products", "*",
		"-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json", "-utf8").Output()
	if err != nil {
		return VisualStudio{}, false
	}
	return parseVswhere(out)
}

func parseVswhere(out []byte) (VisualStudio, bool) {
	var installs []VisualStudio
	if err := json.Unmarshal(out, &installs); err != nil || len(installs) == 0 || installs[0].Path == "" {
		return VisualStudio{}, false
	}
	return installs[0], true
}

// minimumCMakeFor lists the first CMake release with each generator.
var minimumCMakeFor = map[string]string{
	"Visual Studio 18 2026": "4.2",
}

// cmakeSupportsVisualStudio explains why this CMake version cannot build
// with vs, or returns "".
func cmakeSupportsVisualStudio(cmakeVersion string, vs VisualStudio) string {
	generator := vs.CMakeGenerator()
	if minimum, ok := minimumCMakeFor[generator]; ok && isVersionOlder(cmakeVersion, minimum) {
		return fmt.Sprintf("CMake %s cannot build with %s; install CMake %s or newer", cmakeVersion, generator, minimum)
	}
	return ""
}

// CMakeGenerator names the CMake generator of this Visual Studio version.
func (vs VisualStudio) CMakeGenerator() string {
	switch strings.SplitN(vs.Version, ".", 2)[0] {
	case "16":
		return "Visual Studio 16 2019"
	case "18":
		return "Visual Studio 18 2026"
	default:
		return DefaultVisualStudioGenerator
	}
}

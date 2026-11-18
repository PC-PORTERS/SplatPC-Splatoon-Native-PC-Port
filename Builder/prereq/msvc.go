package prereq

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"splatpc-builder/proc"
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
	return vswhere("-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
}

// findVisualStudioWithoutCpp finds an install that lacks the C++ toolset, so
// the install command can add it there instead of installing a second one.
func findVisualStudioWithoutCpp() (VisualStudio, bool) {
	if _, ok := FindVisualStudio(); ok {
		return VisualStudio{}, false
	}
	return vswhere()
}

func vswhere(args ...string) (VisualStudio, bool) {
	exe := filepath.Join(os.Getenv("ProgramFiles(x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
	args = append([]string{"-latest", "-products", "*", "-format", "json", "-utf8"}, args...)
	out, err := proc.Command(exe, args...).Output()
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

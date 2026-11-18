package prereq

import (
	"strings"
	"testing"
)

func TestOnlyMissingRequiredToolsAreInstalled(t *testing.T) {
	items := []CheckItem{
		{ID: "cmake", Required: true, Found: true},
		{ID: "python3", Required: true, Found: false},
		{ID: "ninja", Required: false, Found: false},
	}
	missing := missingRequired(items)
	if len(missing) != 1 || missing[0] != "python3" {
		t.Fatalf("missingRequired = %v, expected [python3]", missing)
	}
	steps := installSteps("windows", missing, nil)
	if len(steps) != 1 || !strings.Contains(steps[0].Command, "Python.Python.3.13") {
		t.Errorf("steps = %+v, expected only Python", steps)
	}
}

func TestWindowsAddsCppToExistingVisualStudio(t *testing.T) {
	vs := VisualStudio{Path: `C:\VS\18\BuildTools`, DisplayName: "Visual Studio Build Tools 2026"}
	steps := installSteps("windows", []string{"compiler"}, &vs)
	if len(steps) != 1 || !strings.Contains(steps[0].Command, `modify --installPath "C:\VS\18\BuildTools"`) ||
		strings.Contains(steps[0].Command, "winget") {
		t.Errorf("steps = %+v, expected the Visual Studio Installer to add the C++ workload", steps)
	}
	steps = installSteps("windows", []string{"compiler"}, nil)
	if len(steps) != 1 || !strings.Contains(steps[0].Command, "Microsoft.VisualStudio.2022.BuildTools") {
		t.Errorf("steps = %+v, expected a new Build Tools install", steps)
	}
}

func TestLinuxUsesTheDistributionsPackageManager(t *testing.T) {
	cases := map[string]string{
		"debian":   "sudo apt update && sudo apt install -y g++ zlib1g-dev",
		"fedora":   "sudo dnf install -y gcc-c++ zlib-devel",
		"arch":     "sudo pacman -S --needed --noconfirm gcc zlib",
		"opensuse": "sudo zypper install -y gcc-c++ zlib-devel",
	}
	for distro, want := range cases {
		steps := installSteps(distro, []string{"compiler", "zlib"}, nil)
		if len(steps) != 1 || steps[0].Command != want {
			t.Errorf("%s: steps = %+v, expected %q", distro, steps, want)
		}
	}
}

func TestUnknownSystemGetsAHintInsteadOfACommand(t *testing.T) {
	missing := []string{"cmake"}
	steps := installSteps("linux", missing, nil)
	if steps != nil {
		t.Errorf("steps = %+v, expected none for an unknown distribution", steps)
	}
	if script := installScript(steps, missing); script != manualInstallHint {
		t.Errorf("script = %q, expected the manual hint", script)
	}
	if script := installScript(nil, nil); script != "" {
		t.Errorf("script = %q, expected nothing when nothing is missing", script)
	}
}

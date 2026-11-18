package prereq

import (
	"os/exec"
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

func TestLinuxOpensTheDefaultTerminalFirst(t *testing.T) {
	installed := func(names ...string) func(string) (string, error) {
		return func(name string) (string, error) {
			for _, n := range names {
				if n == name {
					return "/usr/bin/" + name, nil
				}
			}
			return "", exec.ErrNotFound
		}
	}
	cases := []struct {
		installed []string
		want      string
	}{
		{[]string{"xterm", "xdg-terminal-exec"}, "xdg-terminal-exec bash /tmp/s.sh"},
		{[]string{"xterm", "kitty"}, "kitty bash /tmp/s.sh"},
		{[]string{"ptyxis"}, "ptyxis -- bash /tmp/s.sh"},
		{[]string{"alacritty"}, "alacritty -e bash /tmp/s.sh"},
		{[]string{"foot"}, "foot bash /tmp/s.sh"},
		{[]string{"xterm"}, "xterm -e bash /tmp/s.sh"},
	}
	for _, c := range cases {
		got := strings.Join(linuxTerminalCommand(installed(c.installed...), "/tmp/s.sh"), " ")
		if got != c.want {
			t.Errorf("with %v installed: %q, expected %q", c.installed, got, c.want)
		}
	}
	if got := linuxTerminalCommand(installed(), "/tmp/s.sh"); got != nil {
		t.Errorf("without a terminal: %q, expected none", got)
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

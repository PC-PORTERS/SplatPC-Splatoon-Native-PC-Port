package prereq

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"splatpc-builder/proc"
)

// Python is the command line that starts Python 3, such as ["py", "-3"].
type Python []string

// pythonCandidates lists the ways Python 3 is started. The python.org
// installer for Windows only adds python.exe and the py launcher, and the
// python3.exe there may be the Microsoft Store stub.
func pythonCandidates(goos string) []Python {
	var candidates []Python
	if goos == "windows" {
		userPy := filepath.Join(UserToolsDir(), "python", "python.exe")
		if _, err := os.Stat(userPy); err == nil {
			candidates = append(candidates, Python{userPy})
		}
	} else {
		userPy := filepath.Join(UserToolsDir(), "python", "bin", "python3")
		if _, err := os.Stat(userPy); err == nil {
			candidates = append(candidates, Python{userPy})
		}
	}
	if UserspaceOnly() {
		return candidates
	}
	candidates = append(candidates, Python{"python3"}, Python{"python"})
	if goos == "windows" {
		candidates = append(candidates, Python{"py", "-3"})
	}
	return candidates
}

func isPython3(versionOutput string) bool {
	return strings.HasPrefix(strings.TrimSpace(versionOutput), "Python 3.")
}

// FindPython returns the first candidate that runs Python 3 >= 3.12 (or the
// first Python 3 found if all are older), and its "Python 3.x.y" version line.
func FindPython() (Python, string, bool) {
	var fallback Python
	var fallbackVer string
	candidates := pythonCandidates(runtime.GOOS)
	// A Windows Store / Python install manager alias (...\WindowsApps\python3.exe) runs in an app
	// container whose AppData is virtualized: it cannot see the CMake installed in UserToolsDir, and
	// the dependency build then fails with "file not found". Such aliases are tried last.
	sort.SliceStable(candidates, func(i, j int) bool {
		return !isStoreAlias(candidates[i][0]) && isStoreAlias(candidates[j][0])
	})
	for _, candidate := range candidates {
		if _, err := exec.LookPath(candidate[0]); err != nil {
			continue
		}
		args := append(append([]string{}, candidate[1:]...), "--version")
		out, err := proc.Command(candidate[0], args...).CombinedOutput()
		if err == nil && isPython3(string(out)) {
			verLine := strings.TrimSpace(string(out))
			if !isVersionOlder(parseVersion(verLine), "3.12") {
				return candidate, verLine, true
			}
			if fallback == nil {
				fallback = candidate
				fallbackVer = verLine
			}
		}
	}
	if fallback != nil {
		return fallback, fallbackVer, true
	}
	return nil, "", false
}

func isStoreAlias(name string) bool {
	p, err := exec.LookPath(name)
	return err == nil && strings.Contains(strings.ToLower(p), `\windowsapps\`)
}

// Command runs a Python script or module with this interpreter.
func (p Python) Command(ctx context.Context, args ...string) *exec.Cmd {
	return proc.CommandContext(ctx, p[0], append(append([]string{}, p[1:]...), args...)...)
}

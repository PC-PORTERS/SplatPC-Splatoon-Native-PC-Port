package prereq

import (
	"context"
	"os/exec"
	"runtime"
	"strings"
)

// Python is the command line that starts Python 3, such as ["py", "-3"].
type Python []string

// pythonCandidates lists the ways Python 3 is started. The python.org
// installer for Windows only adds python.exe and the py launcher, and the
// python3.exe there may be the Microsoft Store stub.
func pythonCandidates(goos string) []Python {
	candidates := []Python{{"python3"}, {"python"}}
	if goos == "windows" {
		candidates = append(candidates, Python{"py", "-3"})
	}
	return candidates
}

func isPython3(versionOutput string) bool {
	return strings.HasPrefix(strings.TrimSpace(versionOutput), "Python 3.")
}

// FindPython returns the first candidate that really runs Python 3, and its
// "Python 3.x.y" version line.
func FindPython() (Python, string, bool) {
	for _, candidate := range pythonCandidates(runtime.GOOS) {
		if _, err := exec.LookPath(candidate[0]); err != nil {
			continue
		}
		args := append(append([]string{}, candidate[1:]...), "--version")
		out, err := exec.Command(candidate[0], args...).CombinedOutput()
		if err == nil && isPython3(string(out)) {
			return candidate, strings.TrimSpace(string(out)), true
		}
	}
	return nil, "", false
}

// Command runs a Python script or module with this interpreter.
func (p Python) Command(ctx context.Context, args ...string) *exec.Cmd {
	return exec.CommandContext(ctx, p[0], append(append([]string{}, p[1:]...), args...)...)
}

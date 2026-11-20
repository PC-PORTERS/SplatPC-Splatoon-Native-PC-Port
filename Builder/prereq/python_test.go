package prereq

import (
	"context"
	"slices"
	"strings"
	"testing"
)

func TestPythonCandidatesIncludeTheWindowsLauncher(t *testing.T) {
	windows := pythonCandidates("windows")
	if !slices.ContainsFunc(windows, func(p Python) bool { return slices.Equal(p, Python{"py", "-3"}) }) {
		t.Errorf("expected py -3 on Windows, got %v", windows)
	}
	if !slices.Equal(windows[1], Python{"python"}) {
		t.Errorf("expected python to be tried after python3, got %v", windows)
	}
	for _, p := range pythonCandidates("linux") {
		if p[0] == "py" {
			t.Errorf("expected no py launcher on Linux")
		}
	}
}

func TestIsPython3(t *testing.T) {
	cases := map[string]bool{
		"Python 3.13.14\r\n": true,
		"Python 2.7.18":      false,
		"":                   false, // The Microsoft Store stub prints nothing.
		"Python was not found; run without arguments to install from the Microsoft Store": false,
	}
	for out, want := range cases {
		if got := isPython3(out); got != want {
			t.Errorf("isPython3(%q) = %v, expected %v", out, got, want)
		}
	}
}

func TestPythonCommandKeepsTheLauncherArguments(t *testing.T) {
	cmd := Python{"py", "-3"}.Command(context.Background(), "script.py", "--rpx", "Gambit.rpx")
	if got := strings.Join(cmd.Args, " "); got != "py -3 script.py --rpx Gambit.rpx" {
		t.Errorf("unexpected command line %q", got)
	}
}

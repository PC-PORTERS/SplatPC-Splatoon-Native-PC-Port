package ui

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"fyne.io/fyne/v2/test"

	"splatpc-builder/engine"
)

func writeGame(t *testing.T, dir string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(engine.GameExecutable(dir), []byte("game"), 0755); err != nil {
		t.Fatal(err)
	}
}

func TestFindInstalledBuildWithoutGame(t *testing.T) {
	prefs := test.NewApp().Preferences()
	if _, ok := findInstalledBuild(prefs, t.TempDir()); ok {
		t.Errorf("expected no build in an empty folder")
	}
}

func TestFindInstalledBuildInTheDefaultFolder(t *testing.T) {
	// Builds made before the Builder remembered them are found too.
	prefs := test.NewApp().Preferences()
	dir := t.TempDir()
	writeGame(t, dir)
	build, ok := findInstalledBuild(prefs, dir)
	if !ok || build.dir != dir {
		t.Fatalf("expected the build in %s, got %+v (found=%v)", dir, build, ok)
	}
	if build.duration != 0 || build.builtAt.IsZero() {
		t.Errorf("expected the exe's date and no duration, got %+v", build)
	}
}

func TestFindInstalledBuildPrefersTheLastBuild(t *testing.T) {
	prefs := test.NewApp().Preferences()
	defaultDir, chosenDir := t.TempDir(), t.TempDir()
	writeGame(t, defaultDir)
	writeGame(t, chosenDir)
	builtAt := time.Date(2026, 11, 17, 13, 11, 0, 0, time.Local)
	rememberBuild(prefs, chosenDir, builtAt, 37*time.Minute+13*time.Second)

	build, ok := findInstalledBuild(prefs, defaultDir)
	if !ok || build.dir != chosenDir {
		t.Fatalf("expected the last build in %s, got %+v (found=%v)", chosenDir, build, ok)
	}
	if got := build.describe(); got != "17 Nov 2026, 13:11 (took 37m13s)" {
		t.Errorf("unexpected description %q", got)
	}
}

func TestFindInstalledBuildFallsBackWhenTheLastBuildIsGone(t *testing.T) {
	prefs := test.NewApp().Preferences()
	defaultDir := t.TempDir()
	writeGame(t, defaultDir)
	rememberBuild(prefs, filepath.Join(t.TempDir(), "deleted"), time.Now(), time.Minute)

	build, ok := findInstalledBuild(prefs, defaultDir)
	if !ok || build.dir != defaultDir {
		t.Errorf("expected the default folder, got %+v (found=%v)", build, ok)
	}
	if strings.Contains(build.describe(), "took") {
		t.Errorf("expected no duration for a build that wasn't the last one, got %q", build.describe())
	}
}

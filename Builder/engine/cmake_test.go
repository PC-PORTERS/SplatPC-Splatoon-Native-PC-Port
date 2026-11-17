package engine

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func TestBuildArgsAlwaysPassTheConfiguration(t *testing.T) {
	for _, generator := range []string{"Visual Studio 17 2022", "Ninja", "Unix Makefiles"} {
		args := cmakeProject{generator: generator, jobs: 24}.buildArgs("build/game", "Gambit")
		i := slices.Index(args, "--config")
		if i < 0 || i+1 >= len(args) || args[i+1] != buildConfig {
			t.Errorf("%s: expected --config %s in %v", generator, buildConfig, args)
		}
		if !slices.Contains(args, "24") || !slices.Contains(args, "Gambit") {
			t.Errorf("%s: expected the target and job count in %v", generator, args)
		}
	}
}

func TestConfigureArgsSetTheBuildTypeAndKeepDefines(t *testing.T) {
	args := cmakeProject{generator: "Ninja"}.configureArgs("src", "out", "-DFOO=1", "-DBAR=2")
	want := []string{"-S", "src", "-B", "out", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + buildConfig, "-DFOO=1", "-DBAR=2"}
	if !slices.Equal(args, want) {
		t.Errorf("expected %v, got %v", want, args)
	}
}

func TestBuiltExecutableFindsMultiConfigOutput(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, buildConfig, exeName("rebrewu"))
	writeDummy(t, path)
	found, err := builtExecutable(dir, "rebrewu")
	if err != nil || found != path {
		t.Errorf("expected %s, got %q (%v)", path, found, err)
	}
}

func TestBuiltExecutableFindsSingleConfigOutput(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, exeName("Splatoon"))
	writeDummy(t, path)
	found, err := builtExecutable(dir, "Splatoon")
	if err != nil || found != path {
		t.Errorf("expected %s, got %q (%v)", path, found, err)
	}
}

func TestBuiltExecutableIgnoresDebugOutput(t *testing.T) {
	dir := t.TempDir()
	writeDummy(t, filepath.Join(dir, "Debug", exeName("Splatoon")))
	_, err := builtExecutable(dir, "Splatoon")
	if err == nil || !strings.Contains(err.Error(), filepath.Join(dir, buildConfig)) {
		t.Errorf("expected an error naming the %s folder, got %v", buildConfig, err)
	}
}

func writeDummy(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("# dummy"), 0644); err != nil {
		t.Fatal(err)
	}
}

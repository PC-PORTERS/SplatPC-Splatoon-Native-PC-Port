package engine

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMeasureCleanupAndCleanupAfterBuild(t *testing.T) {
	repoRoot := t.TempDir()
	toolsDir := t.TempDir()
	t.Setenv("SPLATPC_TOOLS_DIR", toolsDir)

	outputDir := filepath.Join(repoRoot, "Game")
	writeDummy(t, filepath.Join(toolsDir, "cmake", "bin", "cmake"))
	writeDummy(t, filepath.Join(repoRoot, "build", "downloads", "SDL2.zip"))
	writeDummy(t, filepath.Join(repoRoot, "build", "deps", "volk", "lib", "libvolk.a"))
	writeDummy(t, filepath.Join(repoRoot, "build", "deps-build", "tmp.o"))
	writeDummy(t, filepath.Join(repoRoot, "build", "cache", "deps", "marker"))
	writeDummy(t, filepath.Join(repoRoot, "build", "imgui-source", "imgui.cpp"))
	writeDummy(t, filepath.Join(repoRoot, "recompiler", "third_party", "nlohmann", "json.hpp"))
	writeDummy(t, filepath.Join(repoRoot, "recompiler", "third_party", "miniaudio.h"))
	writeDummy(t, filepath.Join(repoRoot, "build", "game", "CMakeCache.txt"))
	writeDummy(t, filepath.Join(repoRoot, "build", "rebrewu", "bin", "rebrewu"))
	writeDummy(t, filepath.Join(repoRoot, "build", "generated", "Gambit", "Gambit_part0.cpp"))
	writeDummy(t, filepath.Join(repoRoot, "build", "build.log"))
	writeDummy(t, filepath.Join(outputDir, "Splatoon"))
	writeDummy(t, filepath.Join(outputDir, "save", "save.dat"))

	stats := MeasureCleanup(repoRoot, outputDir)
	if stats.ToolsBytes == 0 || stats.DepsBytes == 0 || stats.BuildArtifactsBytes == 0 {
		t.Fatalf("expected non-zero cleanup stats, got %+v", stats)
	}

	// 1. Delete dependencies only: keep intermediate build artifacts, build.log, and Game/.
	freedDeps, err := CleanupAfterBuild(repoRoot, outputDir, false, nil)
	if err != nil {
		t.Fatalf("CleanupAfterBuild(false) failed: %v", err)
	}
	if freedDeps <= 0 {
		t.Errorf("expected freedDeps > 0, got %d", freedDeps)
	}
	for _, removed := range []string{
		filepath.Join(toolsDir, "cmake", "bin", "cmake"),
		filepath.Join(repoRoot, "build", "downloads"),
		filepath.Join(repoRoot, "build", "deps"),
		filepath.Join(repoRoot, "build", "imgui-source"),
		filepath.Join(repoRoot, "recompiler", "third_party", "nlohmann"),
		filepath.Join(repoRoot, "recompiler", "third_party", "miniaudio.h"),
	} {
		if _, err := os.Stat(removed); !os.IsNotExist(err) {
			t.Errorf("expected %s to be deleted", removed)
		}
	}
	for _, kept := range []string{
		filepath.Join(repoRoot, "build", "game", "CMakeCache.txt"),
		filepath.Join(repoRoot, "build", "build.log"),
		filepath.Join(outputDir, "Splatoon"),
		filepath.Join(outputDir, "save", "save.dat"),
	} {
		if _, err := os.Stat(kept); err != nil {
			t.Errorf("expected %s to be kept: %v", kept, err)
		}
	}

	// 2. Delete dependencies + intermediate build files: keep build.log and Game/.
	freedBuild, err := CleanupAfterBuild(repoRoot, outputDir, true, nil)
	if err != nil {
		t.Fatalf("CleanupAfterBuild(true) failed: %v", err)
	}
	if freedBuild <= 0 {
		t.Errorf("expected freedBuild > 0, got %d", freedBuild)
	}
	for _, removed := range []string{
		filepath.Join(repoRoot, "build", "game"),
		filepath.Join(repoRoot, "build", "rebrewu"),
		filepath.Join(repoRoot, "build", "generated"),
	} {
		if _, err := os.Stat(removed); !os.IsNotExist(err) {
			t.Errorf("expected %s to be deleted", removed)
		}
	}
	for _, kept := range []string{
		filepath.Join(repoRoot, "build", "build.log"),
		filepath.Join(outputDir, "Splatoon"),
		filepath.Join(outputDir, "save", "save.dat"),
	} {
		if _, err := os.Stat(kept); err != nil {
			t.Errorf("expected %s to be preserved: %v", kept, err)
		}
	}
}

func TestCleanupProtectsOutputDirInsideBuild(t *testing.T) {
	repoRoot := t.TempDir()
	t.Setenv("SPLATPC_TOOLS_DIR", t.TempDir())

	// Edge case: user outputs into build/game/Game.
	outputDir := filepath.Join(repoRoot, "build", "game", "Game")
	writeDummy(t, filepath.Join(outputDir, "Splatoon"))
	writeDummy(t, filepath.Join(repoRoot, "build", "downloads", "archive.tar.gz"))

	if _, err := CleanupAfterBuild(repoRoot, outputDir, true, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(outputDir, "Splatoon")); err != nil {
		t.Errorf("expected output inside build/ to stay protected: %v", err)
	}
	if _, err := os.Stat(filepath.Join(repoRoot, "build", "downloads")); !os.IsNotExist(err) {
		t.Errorf("expected build/downloads to still be deleted")
	}
}

func TestFormatBytes(t *testing.T) {
	cases := map[int64]string{
		512:                    "512 B",
		2048:                   "2 KB",
		5 * 1024 * 1024:        "5.0 MB",
		3 * 1024 * 1024 * 1024: "3.00 GB",
	}
	for input, want := range cases {
		if got := FormatBytes(input); got != want {
			t.Errorf("FormatBytes(%d) = %q, want %q", input, got, want)
		}
	}
}

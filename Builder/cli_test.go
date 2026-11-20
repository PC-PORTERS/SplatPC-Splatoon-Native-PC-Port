package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

func TestRunPrepareNeedsAGameFolder(t *testing.T) {
	if got := runPrepare(t.TempDir(), false, ""); got != exitUnsupportedROM {
		t.Errorf("Expected exit code %d without a game folder, got %d", exitUnsupportedROM, got)
	}
	if got := runPrepare(t.TempDir(), false, filepath.Join(t.TempDir(), "missing")); got != exitUnsupportedROM {
		t.Errorf("Expected exit code %d for a missing folder, got %d", exitUnsupportedROM, got)
	}
}

func TestRunPrepareRejectsAnUnsupportedCopy(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not in PATH")
	}
	dump := t.TempDir()
	for _, folder := range []string{"code", "content", "meta"} {
		if err := os.MkdirAll(filepath.Join(dump, folder), 0755); err != nil {
			t.Fatal(err)
		}
	}
	// Not a real RPX: no manifest describes it.
	if err := os.WriteFile(filepath.Join(dump, "code", "Gambit.rpx"), []byte("not an rpx"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dump, "meta", "meta.xml"), []byte("<menu><title_version>16</title_version></menu>"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := runPrepare("..", false, dump); got != exitUnsupportedROM {
		t.Errorf("Expected exit code %d for an unsupported copy, got %d", exitUnsupportedROM, got)
	}
}

func TestRunPlayNeedsABuiltGame(t *testing.T) {
	if got := runPlay(t.TempDir(), false); got != exitNotBuiltYet {
		t.Errorf("Expected exit code %d before the build, got %d", exitNotBuiltYet, got)
	}
}

func TestRunPlayReturnsTheGameExitCode(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the stand-in game is a shell script")
	}
	repoRoot := t.TempDir()
	gameDir := filepath.Join(repoRoot, "Game")
	if err := os.MkdirAll(gameDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(gameDir, "Splatoon"), []byte("#!/bin/sh\nexit 7\n"), 0755); err != nil {
		t.Fatal(err)
	}
	if got := runPlay(repoRoot, false); got != 7 {
		t.Errorf("Expected the game's exit code 7, got %d", got)
	}
}

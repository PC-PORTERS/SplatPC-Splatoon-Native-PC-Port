package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"time"

	"splatpc-builder/engine"
	"splatpc-builder/prereq"
	"splatpc-builder/roms"
	"splatpc-builder/settings"
)

// Exit codes of the headless modes, shown as a message instead of a crash
// report by the launcher. The game itself only ever exits 0 or 1.
const (
	exitSetupFailed    = 1
	exitMissingTools   = 2
	exitUnsupportedROM = 3
	exitNotBuiltYet    = 4
)

// runPrepare builds the port without a window, for the launcher's setup
// screen: it prints progress lines as it goes and returns the exit code.
func runPrepare(repoRoot string, standalone bool, gamePath string) int {
	fmt.Println("progress 0.00 Checking your copy of the game")
	if gamePath == "" {
		fmt.Fprintln(os.Stderr, "No game folder given; pass --game <path>.")
		return exitUnsupportedROM
	}
	info := roms.ValidateDump(gamePath)
	if info.ErrorMessage != "" {
		fmt.Fprintln(os.Stderr, info.ErrorMessage)
		return exitUnsupportedROM
	}
	// The build matches code/Gambit.rpx against the sealed manifests too; do
	// it here so a wrong copy fails the setup with a clear exit code instead
	// of hours into the build.
	if out, err := selectManifest(repoRoot, gamePath); err != nil && !errors.Is(err, exec.ErrNotFound) {
		fmt.Fprintf(os.Stderr, "Unsupported game version:\n%s\n", out)
		return exitUnsupportedROM
	}

	if report := prereq.RunChecks(); !report.AllRequiredMet {
		fmt.Fprintln(os.Stderr, "Missing build tools:")
		for _, item := range report.Items {
			if item.Required && !item.Found {
				fmt.Fprintf(os.Stderr, "  %s: %s\n", item.Name, item.ErrorMsg)
			}
		}
		fmt.Fprintln(os.Stderr, report.InstallScript)
		return exitMissingTools
	}

	cfg := settings.DefaultConfiguration(repoRoot)
	cfg.OutputDir = settings.OutputDirFor(repoRoot, standalone)
	cfg.BaseDumpPath = gamePath
	// A rebuild keeps what the player set in the launcher: settings.ini is
	// rewritten from the configuration below, so start from the current file.
	_ = settings.LoadSettingsIni(filepath.Join(cfg.OutputDir, "settings.ini"), &cfg)

	exit := exitSetupFailed
	engine.RunBuild(context.Background(), repoRoot, cfg, nil, engine.BuildCallbacks{
		OnProgress: func(step engine.StepInfo) {
			fmt.Printf("progress %.2f %s - %s\n", step.Progress, step.Title, step.Description)
		},
		OnLog: func(line string) {
			fmt.Println(line)
		},
		OnDone: func(success bool, err error, _ time.Duration, _ string) {
			if success {
				exit = 0
				return
			}
			fmt.Fprintln(os.Stderr, err)
			exit = exitSetupFailed
		},
	})
	return exit
}

// runPlay starts the built game the way the launcher scripts do and waits for
// it, so the session can be backed up as soon as it closes.
func runPlay(repoRoot string, standalone bool) int {
	cfg := settings.DefaultConfiguration(repoRoot)
	cfg.OutputDir = settings.OutputDirFor(repoRoot, standalone)
	_ = settings.LoadSettingsIni(filepath.Join(cfg.OutputDir, "settings.ini"), &cfg)

	exeName := "Splatoon"
	if runtime.GOOS == "windows" {
		exeName = "Splatoon.exe"
	}
	if _, err := os.Stat(filepath.Join(cfg.OutputDir, exeName)); err != nil {
		fmt.Fprintf(os.Stderr, "The game is not built yet (%s); run the setup first.\n", cfg.OutputDir)
		return exitNotBuiltYet
	}

	cmd, err := engine.LaunchGame(cfg.OutputDir, cfg, func(format string, args ...interface{}) {
		fmt.Printf(format+"\n", args...)
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return exitSetupFailed
	}
	if err := cmd.Wait(); err != nil {
		if code := cmd.ProcessState.ExitCode(); code > 0 {
			return code
		}
		fmt.Fprintln(os.Stderr, err)
		return exitSetupFailed
	}
	return 0
}

// selectManifest asks plt_resolver.py which manifest describes the dump's
// code/Gambit.rpx, the same call the build makes in its first step.
func selectManifest(repoRoot, gamePath string) ([]byte, error) {
	cmd := exec.Command("python3", filepath.Join(repoRoot, "recompiler", "tools", "plt_resolver.py"),
		"select", "--rpx", filepath.Join(gamePath, "code", "Gambit.rpx"),
		"--manifest-dir", filepath.Join(repoRoot, "Manifest"))
	return cmd.CombinedOutput()
}

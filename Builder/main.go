package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"

	"splatpc-builder/prereq"
	"splatpc-builder/ui"
)

func findRepoRoot() string {
	checkPath := func(startDir string) (string, bool) {
		curr := startDir
		for {
			if fileExists(filepath.Join(curr, "setup.sh")) && fileExists(filepath.Join(curr, "Manifest")) {
				return curr, true
			}
			parent := filepath.Dir(curr)
			if parent == curr {
				break
			}
			curr = parent
		}
		return "", false
	}

	if cwd, err := os.Getwd(); err == nil {
		if root, ok := checkPath(cwd); ok {
			return root
		}
	}

	if exe, err := os.Executable(); err == nil {
		if root, ok := checkPath(filepath.Dir(exe)); ok {
			return root
		}
	}

	return "."
}

func fileExists(p string) bool {
	s, err := os.Stat(p)
	return err == nil && (s.IsDir() || s.Mode().IsRegular())
}

func main() {
	checkOnly := flag.Bool("check", false, "Check system prerequisites and exit")
	flag.Parse()

	if *checkOnly {
		report := prereq.RunChecks()
		fmt.Printf("=== SplatPC Builder - Prerequisite Check ===\n")
		for _, it := range report.Items {
			status := "UNAVAILABLE"
			if it.Found {
				status = fmt.Sprintf("OK (%s)", it.Version)
			}
			fmt.Printf("• %-35s : %s\n", it.Name, status)
		}
		if !report.AllRequiredMet {
			fmt.Printf("\nMissing tools detected! Recommended install command:\n%s\n", report.InstallScript)
			os.Exit(1)
		}
		fmt.Println("\nAll system prerequisites are satisfied.")
		os.Exit(0)
	}

	repoRoot := findRepoRoot()

	a := app.NewWithID("com.splatpc.builder")
	a.Settings().SetTheme(&ui.SplatTheme{})

	w := a.NewWindow("SplatPC - Splatoon PC Native Builder & Setup")
	w.Resize(fyne.NewSize(960, 680))
	w.CenterOnScreen()

	wizard := ui.NewWizard(a, w, repoRoot)
	w.SetContent(wizard.Container())

	w.ShowAndRun()
}

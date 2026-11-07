package main

import (
	"flag"
	"fmt"
	"os"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"

	"splatpc-builder/embedded"
	"splatpc-builder/prereq"
	"splatpc-builder/ui"
)

//go:generate python3 ../scripts/bundle_builder_assets.py

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

	repoRoot, isStandalone, err := embedded.ResolveRepoRoot()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Warning resolving builder environment: %v\n", err)
	}

	a := app.NewWithID("com.splatpc.builder")
	a.Settings().SetTheme(&ui.SplatTheme{})

	w := a.NewWindow("SplatPC - Splatoon PC Native Builder & Setup")
	w.Resize(fyne.NewSize(960, 680))
	w.CenterOnScreen()

	wizard := ui.NewWizard(a, w, repoRoot, isStandalone)
	w.SetContent(wizard.Container())

	w.ShowAndRun()
}


package ui

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/engine"
	"splatpc-builder/prereq"
	"splatpc-builder/roms"
	"splatpc-builder/settings"
)

type StepID int

const (
	StepPrereq      StepID = -1
	StepRoms        StepID = 1
	StepSettings    StepID = 2
	StepBuildConfig StepID = 3
	StepBuilding    StepID = 5
	StepDone        StepID = 6
)

type Wizard struct {
	app      fyne.App
	window   fyne.Window
	repoRoot string

	config   settings.Configuration
	overlays []roms.RomLayer

	currentStep StepID
	cancelFunc  context.CancelFunc

	mainContainer *fyne.Container
	stepperBar    *fyne.Container
	stepContent   *fyne.Container

	prereqView      *StepPrereqView
	romsView        *StepRomsView
	settingsView    *StepSettingsView
	buildConfigView *StepBuildConfigView
	buildingView    *StepBuildingView
	doneView        *StepDoneView
}

func NewWizard(a fyne.App, w fyne.Window, repoRoot string, isStandalone bool) *Wizard {
	cfg := settings.DefaultConfiguration(repoRoot)
	if isStandalone {
		if appImage := os.Getenv("APPIMAGE"); appImage != "" {
			cfg.OutputDir = filepath.Join(filepath.Dir(appImage), "Game")
		} else if exe, err := os.Executable(); err == nil {
			exeDir := filepath.Dir(exe)
			if !strings.HasPrefix(exeDir, os.TempDir()) {
				cfg.OutputDir = filepath.Join(exeDir, "Game")
			}
		} else if cwd, err := os.Getwd(); err == nil {
			cfg.OutputDir = filepath.Join(cwd, "Game")
		}
	}

	wiz := &Wizard{
		app:      a,
		window:   w,
		repoRoot: repoRoot,
		config:   cfg,
		overlays: make([]roms.RomLayer, 0),
	}

	// Auto-import existing settings.ini if present in output folder or repo
	initialIniCandidates := []string{
		filepath.Join(wiz.config.OutputDir, "settings.ini"),
		filepath.Join(repoRoot, "Game", "settings.ini"),
		filepath.Join(repoRoot, "settings.ini"),
	}
	for _, iniPath := range initialIniCandidates {
		if _, err := os.Stat(iniPath); err == nil {
			_ = settings.LoadSettingsIni(iniPath, &wiz.config)
			break
		}
	}

	wiz.initViews()
	wiz.settingsView.SyncUIFromConfig()
	wiz.initUI()

	// Initial check: if prerequisites are missing, show Step -1; otherwise, go to Step 1 directly.
	report := prereq.RunChecks()
	if !report.AllRequiredMet {
		wiz.GoToStep(StepPrereq)
	} else {
		wiz.GoToStep(StepRoms)
	}

	return wiz
}

func (w *Wizard) initViews() {
	w.prereqView = NewStepPrereqView(w)
	w.romsView = NewStepRomsView(w)
	w.settingsView = NewStepSettingsView(w)
	w.buildConfigView = NewStepBuildConfigView(w)
	w.buildingView = NewStepBuildingView(w)
	w.doneView = NewStepDoneView(w)
}

func (w *Wizard) initUI() {
	w.stepContent = container.NewStack()
	w.stepperBar = container.NewHBox()

	header := container.NewVBox(
		w.stepperBar,
		widget.NewSeparator(),
	)

	w.mainContainer = container.NewBorder(
		header,
		nil,
		nil,
		nil,
		w.stepContent,
	)
}

func (w *Wizard) updateStepper() {
	w.stepperBar.Objects = nil

	steps := []struct {
		id    StepID
		label string
	}{
		{StepPrereq, "[-1] Prerequisites"},
		{StepRoms, "[1] ROMs & Output"},
		{StepSettings, "[2] Settings"},
		{StepBuildConfig, "[3] Build Config"},
		{StepBuilding, "[5] Build"},
		{StepDone, "[6] Play"},
	}

	for _, s := range steps {
		stepID := s.id
		text := s.label
		isCurrent := stepID == w.currentStep

		// Display-only indicator badge: clicking the top bar is disabled
		btn := widget.NewButton(text, nil)
		btn.Disable()

		if isCurrent {
			btn.Importance = widget.HighImportance
		}

		w.stepperBar.Add(btn)
	}
	w.stepperBar.Refresh()
}

func (w *Wizard) GoToStep(step StepID) {
	w.currentStep = step
	w.updateStepper()

	w.stepContent.Objects = nil
	switch step {
	case StepPrereq:
		w.stepContent.Add(w.prereqView.Container())
	case StepRoms:
		w.stepContent.Add(w.romsView.Container())
	case StepSettings:
		w.stepContent.Add(w.settingsView.Container())
	case StepBuildConfig:
		w.stepContent.Add(w.buildConfigView.Container())
	case StepBuilding:
		w.stepContent.Add(w.buildingView.Container())
	case StepDone:
		w.stepContent.Add(w.doneView.Container())
	}
	w.stepContent.Refresh()
}

func (w *Wizard) StartBuild() {
	if w.config.BaseDumpPath == "" {
		dialog.ShowError(fmt.Errorf("please specify the base ROM in Step 1"), w.window)
		w.GoToStep(StepRoms)
		return
	}

	w.GoToStep(StepBuilding)
	w.buildingView.Reset()

	ctx, cancel := context.WithCancel(context.Background())
	w.cancelFunc = cancel

	go func() {
		engine.RunBuild(ctx, w.repoRoot, w.config, w.overlays, engine.BuildCallbacks{
			OnProgress: func(info engine.StepInfo) {
				w.buildingView.UpdateProgress(info)
			},
			OnLog: func(line string) {
				w.buildingView.AppendLog(line)
			},
			OnDone: func(success bool, err error, elapsed time.Duration, appImagePath string) {
				fyne.Do(func() {
					w.buildingView.StopTimer()
					if success {
						w.doneView.SetSuccess(elapsed, appImagePath)
						w.GoToStep(StepDone)
					} else {
						if err != nil && ctx.Err() == context.Canceled {
							dialog.ShowInformation("Cancelled", "Build was cancelled by the user.", w.window)
							w.GoToStep(StepBuildConfig)
						} else {
							dialog.ShowError(fmt.Errorf("Build failed: %v", err), w.window)
						}
					}
				})
			},
		})
	}()
}

func (w *Wizard) CancelBuild() {
	if w.cancelFunc != nil {
		w.cancelFunc()
	}
}

func (w *Wizard) Container() fyne.CanvasObject {
	return w.mainContainer
}

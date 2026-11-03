package ui

import (
	"fmt"
	"runtime"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type StepBuildConfigView struct {
	wizard *Wizard

	container  *fyne.Container
	jobsSlider *widget.Slider
	jobsLabel  *widget.Label
	cleanCheck *widget.Check
	systemInfo *widget.Label

	prevBtn  *widget.Button
	buildBtn *widget.Button
}

func NewStepBuildConfigView(w *Wizard) *StepBuildConfigView {
	v := &StepBuildConfigView{
		wizard: w,
	}
	v.initUI()
	return v
}

func (v *StepBuildConfigView) initUI() {
	title := widget.NewLabelWithStyle("Step 3: Compilation Configuration", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	desc := widget.NewLabel("Configure parallel compile jobs and build caching options before generating the native game.")

	cfg := &v.wizard.config
	maxCPU := runtime.NumCPU()

	v.jobsLabel = widget.NewLabelWithStyle(fmt.Sprintf("%d cores / threads selected", cfg.ParallelJobs), fyne.TextAlignLeading, fyne.TextStyle{Bold: true})

	v.jobsSlider = widget.NewSlider(1, float64(maxCPU))
	v.jobsSlider.Step = 1
	v.jobsSlider.Value = float64(cfg.ParallelJobs)
	v.jobsSlider.OnChanged = func(val float64) {
		cfg.ParallelJobs = int(val)
		v.jobsLabel.SetText(fmt.Sprintf("%d cores / threads selected", cfg.ParallelJobs))
	}

	v.cleanCheck = widget.NewCheck("Full clean rebuild (--clean)", func(b bool) {
		cfg.CleanBuild = b
	})
	v.cleanCheck.SetChecked(cfg.CleanBuild)

	v.systemInfo = widget.NewLabel(fmt.Sprintf(
		"System Information:\n• CPU: %d logical cores detected\n• OS: %s / %s\n• Recommended jobs: %d (calibrated to RAM: ~3 GB RAM per job for C++ recompiler)",
		maxCPU, runtime.GOOS, runtime.GOARCH, cfg.ParallelJobs,
	))

	cardJobs := widget.NewCard("Parallel Compilation Jobs", "",
		container.NewVBox(
			container.NewHBox(
				widget.NewLabelWithStyle("Number of jobs:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
				v.jobsLabel,
			),
			v.jobsSlider,
			widget.NewLabelWithStyle("Note: Defaults to maximum CPU cores capped by available RAM to prevent OOM.", fyne.TextAlignLeading, fyne.TextStyle{Italic: true}),
			widget.NewSeparator(),
			v.systemInfo,
		),
	)

	cardClean := widget.NewCard("Build Cache Options", "",
		container.NewVBox(
			v.cleanCheck,
			widget.NewLabelWithStyle("Removes previous build/ directory to recompile every module from scratch.", fyne.TextAlignLeading, fyne.TextStyle{Italic: true}),
		),
	)

	v.prevBtn = widget.NewButtonWithIcon("← Settings", theme.NavigateBackIcon(), func() {
		v.wizard.GoToStep(StepSettings)
	})

	v.buildBtn = widget.NewButtonWithIcon("🚀 Start Splatoon Compilation!", theme.MediaPlayIcon(), func() {
		v.wizard.StartBuild()
	})
	v.buildBtn.Importance = widget.HighImportance

	topBar := container.NewVBox(
		title,
		desc,
		widget.NewSeparator(),
	)

	bottomBar := container.NewBorder(
		nil, nil,
		v.prevBtn,
		v.buildBtn,
	)

	v.container = container.NewBorder(
		topBar,
		bottomBar,
		nil,
		nil,
		container.NewVScroll(
			container.NewVBox(
				cardJobs,
				widget.NewSeparator(),
				cardClean,
			),
		),
	)
}

func (v *StepBuildConfigView) Container() fyne.CanvasObject {
	return v.container
}

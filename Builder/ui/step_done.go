package ui

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/engine"
	"splatpc-builder/settings"
)

type StepDoneView struct {
	wizard *Wizard

	container     *fyne.Container
	title         *widget.Label
	description   *widget.Label
	summary       *fyne.Container
	hdCard        *widget.Card
	appImageCard  *widget.Card
	appImageLabel *widget.Label
	cleanupCard   *widget.Card
	cleanupLabel  *widget.Label
	cleanDepsBtn  *widget.Button
	cleanAllBtn   *widget.Button

	launchAppImage *widget.Button

	currentAppImagePath string
}

func NewStepDoneView(w *Wizard) *StepDoneView {
	v := &StepDoneView{
		wizard: w,
	}
	v.initUI()
	return v
}

// wrappedLabel is a label that wraps instead of widening the window, for
// long paths and sentences.
func wrappedLabel(text string) *widget.Label {
	label := widget.NewLabel(text)
	label.Wrapping = fyne.TextWrapWord
	return label
}

func (v *StepDoneView) initUI() {
	v.title = widget.NewLabelWithStyle("", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})
	v.description = widget.NewLabelWithStyle("", fyne.TextAlignCenter, fyne.TextStyle{})
	header := container.NewVBox(
		container.NewCenter(container.NewHBox(widget.NewIcon(theme.ConfirmIcon()), v.title)),
		v.description,
		widget.NewSeparator(),
	)

	launchBtn := widget.NewButtonWithIcon("Launch Splatoon", theme.MediaPlayIcon(), func() {
		_, err := engine.LaunchGame(v.wizard.config.OutputDir, v.wizard.config, func(format string, args ...interface{}) {
			fmt.Printf(format+"\n", args...)
		})
		if err != nil {
			dialog.ShowError(err, v.wizard.window)
		}
	})
	launchBtn.Importance = widget.HighImportance

	v.launchAppImage = widget.NewButtonWithIcon("Launch AppImage", theme.MediaFastForwardIcon(), func() {
		if v.currentAppImagePath != "" {
			if err := exec.Command(v.currentAppImagePath).Start(); err != nil {
				dialog.ShowError(fmt.Errorf("failed to launch AppImage: %w", err), v.wizard.window)
			}
		}
	})
	v.launchAppImage.Hide()

	openFolderBtn := widget.NewButtonWithIcon("Open Game Folder", theme.FolderOpenIcon(), func() {
		if err := engine.OpenFolder(v.wizard.config.OutputDir); err != nil {
			dialog.ShowError(err, v.wizard.window)
		}
	})
	settingsBtn := widget.NewButtonWithIcon("Back to Settings / Recompile", theme.SettingsIcon(), func() {
		v.wizard.GoToStep(StepSettings)
	})

	v.summary = container.New(layout.NewFormLayout())

	upscaleBtn := widget.NewButtonWithIcon("Upscale Dumped Textures", theme.MediaPlayIcon(), func() {
		err := engine.UpscaleTextures(v.wizard.repoRoot, v.wizard.config.OutputDir, func(format string, args ...interface{}) {
			fmt.Printf(format+"\n", args...)
		})
		if err != nil {
			dialog.ShowError(err, v.wizard.window)
		} else {
			dialog.ShowInformation("HD Upscaling Complete", "Finished upscaling textures! They will load automatically next time you play.", v.wizard.window)
		}
	})
	v.hdCard = widget.NewCard("HD Texturing (Real-ESRGAN)", "", container.NewVBox(
		wrappedLabel("Play the game first so textures are recorded into texture-dump/, then upscale them here."),
		container.NewHBox(upscaleBtn),
	))
	v.hdCard.Hide()

	v.appImageLabel = wrappedLabel("")
	v.appImageCard = widget.NewCard("Linux AppImage", "", v.appImageLabel)
	v.appImageCard.Hide()

	v.cleanupLabel = wrappedLabel("")
	v.cleanDepsBtn = widget.NewButtonWithIcon("Delete Dependencies", theme.DeleteIcon(), func() {
		v.runCleanup(false)
	})
	v.cleanAllBtn = widget.NewButtonWithIcon("Delete Dependencies & Build Files", theme.DeleteIcon(), func() {
		v.runCleanup(true)
	})
	v.cleanAllBtn.Importance = widget.DangerImportance
	v.cleanupCard = widget.NewCard("Free Disk Space (Build Cleanup)",
		"Delete downloaded tools and dependencies now that Splatoon is built. Your game and saves are untouched.",
		container.NewVBox(
			v.cleanupLabel,
			container.NewHBox(v.cleanDepsBtn, v.cleanAllBtn),
		),
	)
	v.cleanupCard.Hide()

	bottomBar := container.NewVBox(
		widget.NewSeparator(),
		container.NewPadded(container.NewVBox(
			launchBtn,
			v.launchAppImage,
			container.NewGridWithColumns(2, openFolderBtn, settingsBtn),
		)),
	)

	content := container.NewVBox(
		widget.NewCard("Installation Summary", "", v.summary),
		v.hdCard,
		v.appImageCard,
		v.cleanupCard,
	)
	v.container = container.NewBorder(header, bottomBar, nil, nil, container.NewVScroll(container.NewPadded(content)))
}

func languageName(id int) string {
	for _, l := range settings.SupportedLanguages {
		if l.ID == id {
			return l.Name
		}
	}
	return fmt.Sprintf("ID %d", id)
}

// SetSuccess shows the build that just finished.
func (v *StepDoneView) SetSuccess(elapsed time.Duration, appImagePath string) {
	v.show("Build completed successfully", "Your native Splatoon PC port is ready to play.",
		[2]string{"Build time", elapsed.Round(time.Second).String()}, appImagePath)
}

// SetInstalled shows a build the Builder found when it started.
func (v *StepDoneView) SetInstalled(build installedBuild) {
	v.show("Splatoon is already installed", "Play it, or go back to the settings to rebuild it.",
		[2]string{"Built", build.describe()}, "")
}

func (v *StepDoneView) show(title, description string, buildRow [2]string, appImagePath string) {
	cfg := v.wizard.config
	v.currentAppImagePath = appImagePath
	v.title.SetText(title)
	v.description.SetText(description)

	hdStatus := "Disabled"
	if cfg.UpscaleTextures {
		hdStatus = "Enabled"
	}

	onlineStatus := "Disabled (offline)"
	if cfg.EnableOnline && strings.TrimSpace(cfg.OnlineServer) != "" {
		onlineStatus = "Enabled (" + strings.TrimSpace(cfg.OnlineServer) + ")"
	}
	rows := [][2]string{
		{"Game folder", cfg.OutputDir},
		{"Executable", engine.GameExecutable(cfg.OutputDir)},
		{"Player name", cfg.PlayerName},
		{"Online", onlineStatus},
		{"Resolution", fmt.Sprintf("%s at %d FPS", cfg.Resolution, cfg.FrameRate)},
		{"Language", languageName(cfg.LanguageID)},
		{"Region", cfg.ConsoleRegion},
		{"HD texturing", hdStatus},
		buildRow,
	}
	v.summary.RemoveAll()
	for _, row := range rows {
		v.summary.Add(widget.NewLabelWithStyle(row[0], fyne.TextAlignLeading, fyne.TextStyle{Bold: true}))
		v.summary.Add(wrappedLabel(row[1]))
	}

	if cfg.UpscaleTextures {
		v.hdCard.Show()
	} else {
		v.hdCard.Hide()
	}

	if appImagePath != "" && filepath.Ext(appImagePath) == ".AppImage" {
		v.appImageLabel.SetText("Standalone AppImage is ready:\n" + appImagePath)
		v.appImageCard.Show()
		v.launchAppImage.Show()
	} else {
		v.appImageCard.Hide()
		v.launchAppImage.Hide()
	}

	v.refreshCleanupCard()
	v.container.Refresh()
}

func (v *StepDoneView) refreshCleanupCard() {
	stats := engine.MeasureCleanup(v.wizard.repoRoot, v.wizard.config.OutputDir)
	depsBytes := stats.TotalDepsBytes()
	allBytes := stats.TotalAllBytes()
	if allBytes == 0 {
		v.cleanupCard.Hide()
		return
	}
	v.cleanupLabel.SetText(fmt.Sprintf(
		"Downloaded userspace tools & dependencies: %s\nIntermediate build cache & objects: %s",
		engine.FormatBytes(depsBytes),
		engine.FormatBytes(stats.BuildArtifactsBytes),
	))
	if depsBytes > 0 {
		v.cleanDepsBtn.SetText(fmt.Sprintf("Delete Dependencies (%s)", engine.FormatBytes(depsBytes)))
		v.cleanDepsBtn.Enable()
		v.cleanDepsBtn.Show()
	} else {
		v.cleanDepsBtn.Hide()
	}
	v.cleanAllBtn.SetText(fmt.Sprintf("Delete Dependencies & Build Files (%s)", engine.FormatBytes(allBytes)))
	v.cleanAllBtn.Enable()
	v.cleanupCard.Show()
}

func (v *StepDoneView) runCleanup(includeBuildArtifacts bool) {
	stats := engine.MeasureCleanup(v.wizard.repoRoot, v.wizard.config.OutputDir)
	targetBytes := stats.TotalDepsBytes()
	scopeDesc := "downloaded userspace tools, dependency archives, and compiled dependency libraries"
	if includeBuildArtifacts {
		targetBytes = stats.TotalAllBytes()
		scopeDesc = "downloaded userspace tools, dependencies, AND intermediate C++/recompiler build directories"
	}
	msg := fmt.Sprintf("Delete %s?\n\nThis will free %s.\nYour built game in %s and your saves are NOT deleted.",
		scopeDesc, engine.FormatBytes(targetBytes), v.wizard.config.OutputDir)
	dialog.ShowConfirm("Confirm Cleanup", msg, func(confirm bool) {
		if !confirm {
			return
		}
		v.cleanDepsBtn.Disable()
		v.cleanAllBtn.Disable()
		go func() {
			freed, err := engine.CleanupAfterBuild(v.wizard.repoRoot, v.wizard.config.OutputDir, includeBuildArtifacts, nil)
			fyne.Do(func() {
				if err != nil {
					dialog.ShowError(err, v.wizard.window)
				} else {
					dialog.ShowInformation("Cleanup Complete", fmt.Sprintf("Freed %s of disk space.", engine.FormatBytes(freed)), v.wizard.window)
				}
				v.refreshCleanupCard()
				v.container.Refresh()
			})
		}()
	}, v.wizard.window)
}

func (v *StepDoneView) Container() fyne.CanvasObject {
	return v.container
}

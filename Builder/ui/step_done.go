package ui

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/engine"
)

type StepDoneView struct {
	wizard *Wizard

	container      *fyne.Container
	titleLabel     *widget.Label
	descLabel      *widget.Label
	summaryCard    *widget.Card
	hdCard         *widget.Card
	hdLabel        *widget.Label
	hdUpscaleBtn   *widget.Button
	appImageCard   *widget.Card
	appImageLabel  *widget.Label

	launchBtn      *widget.Button
	launchAppImage *widget.Button
	openFoldBtn    *widget.Button
	restartBtn     *widget.Button

	currentAppImagePath string
}

func NewStepDoneView(w *Wizard) *StepDoneView {
	v := &StepDoneView{
		wizard: w,
	}
	v.initUI()
	return v
}

func (v *StepDoneView) initUI() {
	v.titleLabel = widget.NewLabelWithStyle("🎉  Build Completed Successfully!", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})
	v.descLabel = widget.NewLabelWithStyle("Your native Splatoon PC port is ready to play.", fyne.TextAlignCenter, fyne.TextStyle{})

	v.launchBtn = widget.NewButtonWithIcon("🎮  Launch Splatoon (Direct)", theme.MediaPlayIcon(), func() {
		_, err := engine.LaunchGame(v.wizard.config.OutputDir, v.wizard.config, func(format string, args ...interface{}) {
			fmt.Printf(format+"\n", args...)
		})
		if err != nil {
			dialog.ShowError(err, v.wizard.window)
		}
	})
	v.launchBtn.Importance = widget.HighImportance

	v.launchAppImage = widget.NewButtonWithIcon("🚀  Launch AppImage", theme.MediaFastForwardIcon(), func() {
		if v.currentAppImagePath != "" {
			cmd := exec.Command(v.currentAppImagePath)
			if err := cmd.Start(); err != nil {
				dialog.ShowError(fmt.Errorf("failed to launch AppImage: %w", err), v.wizard.window)
			}
		}
	})
	v.launchAppImage.Importance = widget.HighImportance
	v.launchAppImage.Hide()

	v.openFoldBtn = widget.NewButtonWithIcon("📁  Open Game Folder", theme.FolderOpenIcon(), func() {
		if err := engine.OpenFolder(v.wizard.config.OutputDir); err != nil {
			dialog.ShowError(err, v.wizard.window)
		}
	})

	v.restartBtn = widget.NewButtonWithIcon("⚙️  Back to Settings / Recompile", theme.SettingsIcon(), func() {
		v.wizard.GoToStep(StepSettings)
	})

	v.summaryCard = widget.NewCard("Installation Summary", "", widget.NewLabel(""))

	v.hdLabel = widget.NewLabel("HD Texturing is enabled. Play the game to record textures into texture-dump/, then upscale them using Real-ESRGAN.")
	v.hdUpscaleBtn = widget.NewButtonWithIcon("✨ Upscale Dumped Textures to HD (Real-ESRGAN)", theme.MediaPlayIcon(), func() {
		err := engine.UpscaleTextures(v.wizard.repoRoot, v.wizard.config.OutputDir, func(format string, args ...interface{}) {
			fmt.Printf(format+"\n", args...)
		})
		if err != nil {
			dialog.ShowError(err, v.wizard.window)
		} else {
			dialog.ShowInformation("HD Upscaling Complete", "Finished upscaling textures! They will load automatically next time you play.", v.wizard.window)
		}
	})
	v.hdCard = widget.NewCard("🖼️ HD Texturing (Real-ESRGAN AI)", "", container.NewVBox(
		v.hdLabel,
		v.hdUpscaleBtn,
	))
	v.hdCard.Hide()

	v.appImageLabel = widget.NewLabel("")
	v.appImageCard = widget.NewCard("📦 Linux Standalone AppImage", "", v.appImageLabel)
	v.appImageCard.Hide()

	topHeader := container.NewVBox(
		container.NewPadded(v.titleLabel),
		v.descLabel,
		widget.NewSeparator(),
	)

	// Action buttons prominently laid out at the bottom
	actionButtonsRow := container.NewGridWithColumns(2,
		v.launchBtn,
		v.launchAppImage,
	)

	secondaryButtonsRow := container.NewGridWithColumns(2,
		v.openFoldBtn,
		v.restartBtn,
	)

	bottomBar := container.NewVBox(
		widget.NewSeparator(),
		container.NewPadded(
			container.NewVBox(
				actionButtonsRow,
				secondaryButtonsRow,
			),
		),
	)

	scrollContent := container.NewVBox(
		v.summaryCard,
		v.hdCard,
		v.appImageCard,
	)

	v.container = container.NewBorder(
		topHeader,
		bottomBar,
		nil,
		nil,
		container.NewVScroll(container.NewPadded(scrollContent)),
	)
}

func (v *StepDoneView) SetSuccess(elapsed time.Duration, appImagePath string) {
	cfg := v.wizard.config
	v.currentAppImagePath = appImagePath

	hdStatus := "Disabled"
	if cfg.UpscaleTextures {
		hdStatus = "Enabled (Real-ESRGAN)"
	}

	summaryText := fmt.Sprintf(
		"• Destination Directory: %s\n"+
			"• Executable Binary: %s\n"+
			"• Player Nickname: %s\n"+
			"• Render Resolution: %s @ %d FPS\n"+
			"• HD Texturing: %s\n"+
			"• Cafe Language ID: %d\n"+
			"• Total Elapsed Build Time: %.1f seconds",
		cfg.OutputDir,
		filepath.Join(cfg.OutputDir, "Splatoon"),
		cfg.PlayerName,
		cfg.Resolution,
		cfg.FrameRate,
		hdStatus,
		cfg.LanguageID,
		elapsed.Seconds(),
	)
	v.summaryCard.SetSubTitle(summaryText)

	if cfg.UpscaleTextures {
		v.hdCard.Show()
	} else {
		v.hdCard.Hide()
	}

	if appImagePath != "" && filepath.Ext(appImagePath) == ".AppImage" {
		v.appImageLabel.SetText(fmt.Sprintf("Standalone AppImage is ready:\n%s", appImagePath))
		v.appImageCard.Show()
		v.launchAppImage.Show()
	} else {
		v.appImageCard.Hide()
		v.launchAppImage.Hide()
	}

	v.container.Refresh()
}

func (v *StepDoneView) Container() fyne.CanvasObject {
	return v.container
}

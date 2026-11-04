package ui

import (
	"fmt"
	"runtime"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/settings"
)

type StepSettingsView struct {
	wizard *Wizard

	container *fyne.Container

	pseudoEntry      *widget.Entry
	langSelect       *widget.Select
	resSelect        *widget.Select
	fpsSelect        *widget.Select
	plaza60Check     *widget.Check
	shaderSelect     *widget.Select
	fullscreenCheck  *widget.Check
	gsyncCheck       *widget.Check
	gyroCheck        *widget.Check
	mouseDirectCheck *widget.Check
	fovSlider        *widget.Slider
	fovLabel         *widget.Label
	volumeSlider     *widget.Slider
	volumeLabel      *widget.Label
	appImageCheck    *widget.Check
	hdTexturesCheck  *widget.Check
	hdTexturesStatus *widget.Label

	importBtn *widget.Button
	prevBtn   *widget.Button
	nextBtn   *widget.Button
}

func NewStepSettingsView(w *Wizard) *StepSettingsView {
	v := &StepSettingsView{
		wizard: w,
	}
	v.initUI()
	v.SyncUIFromConfig()
	return v
}

func (v *StepSettingsView) initUI() {
	title := widget.NewLabelWithStyle("Step 2: Settings, Profile & Gameplay Options", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	desc := widget.NewLabel("Customize your player nickname, language, resolution, controls, and PC port enhancements.")

	cfg := &v.wizard.config

	v.importBtn = widget.NewButtonWithIcon("Import Existing settings.ini...", theme.DocumentIcon(), func() {
		dialog.ShowFileOpen(func(uri fyne.URIReadCloser, err error) {
			if err == nil && uri != nil {
				filePath := uri.URI().Path()
				if err := settings.LoadSettingsIni(filePath, &v.wizard.config); err != nil {
					dialog.ShowError(fmt.Errorf("failed to load settings.ini: %w", err), v.wizard.window)
					return
				}
				v.SyncUIFromConfig()
				dialog.ShowInformation("Settings Imported", fmt.Sprintf("Successfully imported settings from:\n%s", filePath), v.wizard.window)
			}
		}, v.wizard.window)
	})

	// 1. Profile & Nickname
	v.pseudoEntry = widget.NewEntry()
	v.pseudoEntry.SetText(cfg.PlayerName)
	v.pseudoEntry.SetPlaceHolder("E.g. Inkling (10 characters max)")
	v.pseudoEntry.OnChanged = func(val string) {
		cfg.PlayerName = val
	}
	pseudoHelp := widget.NewLabelWithStyle("Name displayed in-game (replaces the default 'default' nickname)", fyne.TextAlignLeading, fyne.TextStyle{Italic: true})

	var langNames []string
	selectedIndex := 0
	for i, l := range settings.SupportedLanguages {
		langNames = append(langNames, l.Name)
		if l.ID == cfg.LanguageID {
			selectedIndex = i
		}
	}
	v.langSelect = widget.NewSelect(langNames, func(selected string) {
		for _, l := range settings.SupportedLanguages {
			if l.Name == selected {
				cfg.LanguageID = l.ID
				break
			}
		}
	})
	v.langSelect.SetSelectedIndex(selectedIndex)

	accountCard := widget.NewCard("Player Profile & Language", "",
		container.NewVBox(
			container.NewBorder(nil, nil, nil, v.importBtn, widget.NewLabel("")),
			widget.NewLabelWithStyle("In-game Nickname (Mii / Profile):", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			v.pseudoEntry,
			pseudoHelp,
			widget.NewSeparator(),
			widget.NewLabelWithStyle("Game Language (Wii U Cafe Language):", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			v.langSelect,
		),
	)

	// 2. Display & Graphics
	v.resSelect = widget.NewSelect([]string{"720p", "1080p", "1440p", "native"}, func(s string) {
		cfg.Resolution = s
	})
	v.resSelect.SetSelected(cfg.Resolution)

	v.fpsSelect = widget.NewSelect([]string{"30 FPS", "60 FPS", "120 FPS", "180 FPS"}, func(s string) {
		switch s {
		case "30 FPS":
			cfg.FrameRate = 30
		case "60 FPS":
			cfg.FrameRate = 60
		case "120 FPS":
			cfg.FrameRate = 120
		case "180 FPS":
			cfg.FrameRate = 180
		}
	})
	v.fpsSelect.SetSelected(fmt.Sprintf("%d FPS", cfg.FrameRate))

	v.plaza60Check = widget.NewCheck("Plaza 60 FPS (unlocks native 30 FPS plaza cap)", func(b bool) {
		cfg.Plaza60FPS = b
	})
	v.plaza60Check.SetChecked(cfg.Plaza60FPS)

	v.fullscreenCheck = widget.NewCheck("Fullscreen", func(b bool) {
		cfg.Fullscreen = b
	})
	v.fullscreenCheck.SetChecked(cfg.Fullscreen)

	v.gsyncCheck = widget.NewCheck("G-Sync / FreeSync (immediate flip presentation)", func(b bool) {
		cfg.GSync = b
	})
	v.gsyncCheck.SetChecked(cfg.GSync)

	v.shaderSelect = widget.NewSelect([]string{
		"Off (Original clean game at 60 FPS)",
		"Low (Lightweight enhanced shaders)",
		"Balanced (Enhanced shaders)",
		"High (Full volumetric clouds, sky and light shafts)",
	}, func(s string) {
		switch s {
		case "Off (Original clean game at 60 FPS)":
			cfg.ShaderQuality = 0
		case "Low (Lightweight enhanced shaders)":
			cfg.ShaderQuality = 1
		case "Balanced (Enhanced shaders)":
			cfg.ShaderQuality = 2
		case "High (Full volumetric clouds, sky and light shafts)":
			cfg.ShaderQuality = 3
		}
	})
	v.shaderSelect.SetSelectedIndex(cfg.ShaderQuality)

	v.hdTexturesCheck = widget.NewCheck("Enable HD Texturing (Real-ESRGAN AI Upscaling)", func(b bool) {
		cfg.UpscaleTextures = b
	})
	v.hdTexturesCheck.SetChecked(cfg.UpscaleTextures)

	v.hdTexturesStatus = widget.NewLabelWithStyle("", fyne.TextAlignLeading, fyne.TextStyle{Italic: true})
	if cfg.HasNvidiaGPU {
		v.hdTexturesStatus.SetText(fmt.Sprintf("✅ Compatible NVIDIA GPU detected (%s) - HD texturing activated automatically", cfg.DetectedGPUName))
	} else {
		gpuDesc := cfg.DetectedGPUName
		if gpuDesc == "" {
			gpuDesc = "None detected"
		}
		v.hdTexturesStatus.SetText(fmt.Sprintf("ℹ️ GPU: %s - HD texturing disabled (NVIDIA CUDA recommended for fast upscaling)", gpuDesc))
	}

	graphicsCard := widget.NewCard("Graphics & Display Settings (README)", "",
		container.NewVBox(
			container.NewGridWithColumns(2,
				container.NewVBox(widget.NewLabelWithStyle("Render Resolution:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}), v.resSelect),
				container.NewVBox(widget.NewLabelWithStyle("Frame Rate Cap:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}), v.fpsSelect),
			),
			widget.NewSeparator(),
			widget.NewLabelWithStyle("Shader Graphics Preset:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			v.shaderSelect,
			widget.NewSeparator(),
			container.NewGridWithColumns(3,
				v.plaza60Check,
				v.fullscreenCheck,
				v.gsyncCheck,
			),
			widget.NewSeparator(),
			widget.NewLabelWithStyle("HD Textures (Real-ESRGAN AI Upscaling):", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			v.hdTexturesCheck,
			v.hdTexturesStatus,
		),
	)

	// 3. Gameplay, Controls & Audio
	v.gyroCheck = widget.NewCheck("Gyroscope Motion Aiming (Controller)", func(b bool) {
		cfg.Gyro = b
	})
	v.gyroCheck.SetChecked(cfg.Gyro)

	v.mouseDirectCheck = widget.NewCheck("Direct Mouse Aiming (1:1 motion aiming, no stick emulation)", func(b bool) {
		cfg.MouseDirect = b
	})
	v.mouseDirectCheck.SetChecked(cfg.MouseDirect)

	v.fovLabel = widget.NewLabel(fmt.Sprintf("%d° (Default: 55°)", cfg.CameraFOV))
	v.fovSlider = widget.NewSlider(40, 90)
	v.fovSlider.Value = float64(cfg.CameraFOV)
	v.fovSlider.OnChanged = func(val float64) {
		cfg.CameraFOV = int(val)
		v.fovLabel.SetText(fmt.Sprintf("%d°", cfg.CameraFOV))
	}

	v.volumeLabel = widget.NewLabel(fmt.Sprintf("%d%%", cfg.Volume))
	v.volumeSlider = widget.NewSlider(0, 100)
	v.volumeSlider.Value = float64(cfg.Volume)
	v.volumeSlider.OnChanged = func(val float64) {
		cfg.Volume = int(val)
		v.volumeLabel.SetText(fmt.Sprintf("%d%%", cfg.Volume))
	}

	controlsCard := widget.NewCard("Controls, Camera & Audio", "",
		container.NewVBox(
			container.NewGridWithColumns(2,
				v.gyroCheck,
				v.mouseDirectCheck,
			),
			widget.NewSeparator(),
			container.NewGridWithColumns(2,
				container.NewVBox(
					container.NewHBox(widget.NewLabelWithStyle("Camera Field of View (FOV):", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}), v.fovLabel),
					v.fovSlider,
				),
				container.NewVBox(
					container.NewHBox(widget.NewLabelWithStyle("Master Volume:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}), v.volumeLabel),
					v.volumeSlider,
				),
			),
		),
	)

	// 4. Packaging & Platform
	v.appImageCheck = widget.NewCheck("Generate single standalone Linux AppImage", func(b bool) {
		cfg.CreateAppImage = b
	})
	v.appImageCheck.SetChecked(cfg.CreateAppImage)

	if runtime.GOOS != "linux" {
		v.appImageCheck.Disable()
		v.appImageCheck.SetChecked(false)
		cfg.CreateAppImage = false
	}

	platformCard := widget.NewCard("Distribution & Platform Options", "",
		container.NewVBox(
			v.appImageCheck,
			widget.NewLabelWithStyle("Creates a portable, self-contained .AppImage file for Linux.", fyne.TextAlignLeading, fyne.TextStyle{Italic: true}),
		),
	)

	v.prevBtn = widget.NewButtonWithIcon("← ROMs", theme.NavigateBackIcon(), func() {
		v.wizard.GoToStep(StepRoms)
	})

	v.nextBtn = widget.NewButtonWithIcon("Build Options →", theme.NavigateNextIcon(), func() {
		v.wizard.GoToStep(StepBuildConfig)
	})
	v.nextBtn.Importance = widget.HighImportance

	topBar := container.NewVBox(
		title,
		desc,
		widget.NewSeparator(),
	)

	bottomBar := container.NewBorder(
		nil, nil,
		v.prevBtn,
		v.nextBtn,
	)

	v.container = container.NewBorder(
		topBar,
		bottomBar,
		nil,
		nil,
		container.NewVScroll(
			container.NewVBox(
				accountCard,
				widget.NewSeparator(),
				graphicsCard,
				widget.NewSeparator(),
				controlsCard,
				widget.NewSeparator(),
				platformCard,
			),
		),
	)
}

func (v *StepSettingsView) SyncUIFromConfig() {
	cfg := v.wizard.config
	v.pseudoEntry.SetText(cfg.PlayerName)

	for i, l := range settings.SupportedLanguages {
		if l.ID == cfg.LanguageID {
			v.langSelect.SetSelectedIndex(i)
			break
		}
	}

	v.resSelect.SetSelected(cfg.Resolution)
	v.fpsSelect.SetSelected(fmt.Sprintf("%d FPS", cfg.FrameRate))
	v.plaza60Check.SetChecked(cfg.Plaza60FPS)
	v.fullscreenCheck.SetChecked(cfg.Fullscreen)
	v.gsyncCheck.SetChecked(cfg.GSync)
	v.shaderSelect.SetSelectedIndex(cfg.ShaderQuality)

	v.hdTexturesCheck.SetChecked(cfg.UpscaleTextures)
	if cfg.HasNvidiaGPU {
		v.hdTexturesStatus.SetText(fmt.Sprintf("✅ Compatible NVIDIA GPU detected (%s) - HD texturing activated automatically", cfg.DetectedGPUName))
	} else {
		gpuDesc := cfg.DetectedGPUName
		if gpuDesc == "" {
			gpuDesc = "None detected"
		}
		v.hdTexturesStatus.SetText(fmt.Sprintf("ℹ️ GPU: %s - HD texturing disabled (NVIDIA CUDA recommended for fast upscaling)", gpuDesc))
	}
	v.gyroCheck.SetChecked(cfg.Gyro)
	v.mouseDirectCheck.SetChecked(cfg.MouseDirect)
	v.fovSlider.SetValue(float64(cfg.CameraFOV))
	v.fovLabel.SetText(fmt.Sprintf("%d°", cfg.CameraFOV))
	v.volumeSlider.SetValue(float64(cfg.Volume))
	v.volumeLabel.SetText(fmt.Sprintf("%d%%", cfg.Volume))
	v.appImageCheck.SetChecked(cfg.CreateAppImage)
}

func (v *StepSettingsView) Container() fyne.CanvasObject {
	return v.container
}

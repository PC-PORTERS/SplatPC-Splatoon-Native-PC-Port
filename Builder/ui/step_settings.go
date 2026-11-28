package ui

import (
	"fmt"
	"runtime"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/roms"
	"splatpc-builder/settings"
)

type StepSettingsView struct {
	wizard        *Wizard
	syncingOnline bool

	container *fyne.Container

	pseudoEntry       *widget.Entry
	langSelect        *widget.Select
	onlineCheck       *widget.Check
	onlineServerEntry *widget.Entry
	serverSelect      *widget.Select
	serverPresets     []settings.ServerPreset
	registerEntry     *widget.Entry
	accountEntry      *widget.Entry
	bossEntry         *widget.Entry
	nncs1Entry        *widget.Entry
	nncs2Entry        *widget.Entry
	syncingServer     bool
	onlineErrorLabel  *widget.Label
	resSelect             *widget.Select
	fpsSelect             *widget.Select
	plaza60Check          *widget.Check
	shaderSelect          *widget.Select
	anisoSelect           *widget.Select
	dofCheck              *widget.Check
	aaSelect              *widget.Select
	upscalerSelect        *widget.Select
	motionBlurCheck       *widget.Check
	fullscreenCheck       *widget.Check
	gsyncCheck            *widget.Check
	gyroCheck             *widget.Check
	mouseDirectCheck      *widget.Check
	disableAutoPitchCheck *widget.Check
	fovSlider             *widget.Slider
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

	// 2. Online Multiplayer (Community Server)
	// Server choice: one preset per scripts/server*.txt, or a custom (self-hosted) server whose
	// addresses are edited under "Advanced". Editing a field of a preset turns the choice into Custom.
	v.serverPresets = settings.ServerPresets(v.wizard.repoRoot)
	const customServer = "Custom (self-hosted)"
	names := make([]string, 0, len(v.serverPresets)+1)
	for _, p := range v.serverPresets {
		names = append(names, p.Name)
	}
	names = append(names, customServer)
	field := func(target *string, placeholder string) *widget.Entry {
		e := widget.NewEntry()
		e.SetPlaceHolder(placeholder)
		e.OnChanged = func(text string) {
			*target = strings.TrimSpace(text)
			v.refreshServerSelect()
		}
		return e
	}
	v.onlineServerEntry = field(&cfg.OnlineServer, settings.FallbackOnlineServer)
	v.registerEntry = field(&cfg.OnlineRegister, "https://host/register (community server sign-up)")
	v.accountEntry = field(&cfg.OnlineAccount, "https://account.host (account server: PC account + NEX token)")
	v.bossEntry = field(&cfg.OnlineBoss, "https://host/path (SpotPass: rotation, Splatfest)")
	v.nncs1Entry = field(&cfg.OnlineNncs1, "NAT check server 1 (IPv4)")
	v.nncs2Entry = field(&cfg.OnlineNncs2, "NAT check server 2 (IPv4)")
	v.serverSelect = widget.NewSelect(names, func(name string) {
		if v.syncingServer {
			return
		}
		for _, p := range v.serverPresets {
			if p.Name == name {
				cfg.ApplyPreset(p)
				v.syncServerFields()
				return
			}
		}
	})

	v.onlineErrorLabel = widget.NewLabelWithStyle(
		"⚠️ Online requires the latest game version (v288). Add the v288 update in Step 1 (ROMs) to enable online play.",
		fyne.TextAlignLeading,
		fyne.TextStyle{Bold: true},
	)
	v.onlineErrorLabel.Wrapping = fyne.TextWrapWord
	v.onlineErrorLabel.Importance = widget.DangerImportance
	v.onlineErrorLabel.Hide()

	v.onlineCheck = widget.NewCheck("Enable Online (requires v288)", func(b bool) {
		if v.syncingOnline {
			return
		}
		if b && !roms.HasV288(v.wizard.config.BaseDumpPath, v.wizard.overlays, v.wizard.config.OutputDir) {
			v.wizard.config.EnableOnline = false
			v.syncingOnline = true
			v.onlineCheck.SetChecked(false)
			v.syncingOnline = false
			v.setOnlineFieldsEnabled(false)
			v.onlineErrorLabel.Show()
			if v.wizard.window != nil {
				dialog.ShowError(
					fmt.Errorf("Online multiplayer requires the latest game version (v288).\n\nGo back to Step 1 (ROMs) and add the official v288 update (or select a merged v288 dump)."),
					v.wizard.window,
				)
			}
			return
		}
		v.wizard.config.EnableOnline = b
		v.setOnlineFieldsEnabled(b)
		if b {
			v.onlineErrorLabel.Hide()
		}
	})

	onlineHelp := widget.NewLabelWithStyle("Requires Splatoon v288. Automatic authentication linked to your PC (online_identity.ini). In-game friends overlay: Press F4.", fyne.TextAlignLeading, fyne.TextStyle{Italic: true})
	onlineHelp.Wrapping = fyne.TextWrapWord

	advanced := widget.NewAccordion(widget.NewAccordionItem("Advanced: self-hosted server addresses", container.NewVBox(
		widget.NewLabel("Server Address (Host:Port):"), v.onlineServerEntry,
		widget.NewLabel("Sign-up URL (community server):"), v.registerEntry,
		widget.NewLabel("Account server URL:"), v.accountEntry,
		widget.NewLabel("SpotPass URL:"), v.bossEntry,
		widget.NewLabel("NAT check servers:"), v.nncs1Entry, v.nncs2Entry,
	)))

	onlineCard := widget.NewCard("🌐 Online Multiplayer", "",
		container.NewVBox(
			v.onlineCheck,
			v.onlineErrorLabel,
			widget.NewLabelWithStyle("Server:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			v.serverSelect,
			advanced,
			onlineHelp,
		),
	)

	// 3. Display & Graphics
	resToLabel := func(res string) string {
		switch strings.ToLower(res) {
		case "4k", "2160p", "3840x2160":
			return "4K (3840x2160)"
		case "8k", "4320p", "7680x4320":
			return "8K (7680x4320)"
		default:
			return res
		}
	}
	v.resSelect = widget.NewSelect([]string{"720p", "1080p", "1440p", "4K (3840x2160)", "8K (7680x4320)", "native"}, func(s string) {
		switch s {
		case "4K (3840x2160)":
			cfg.Resolution = "4k"
		case "8K (7680x4320)":
			cfg.Resolution = "8k"
		default:
			cfg.Resolution = s
		}
	})
	v.resSelect.SetSelected(resToLabel(cfg.Resolution))

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

	anisoLabels := []string{"Auto (game)", "2x", "4x", "8x", "16x"}
	anisoIndexFor := func(af int) int {
		switch af {
		case 2:
			return 1
		case 4:
			return 2
		case 8:
			return 3
		case 16:
			return 4
		default:
			return 0
		}
	}
	v.anisoSelect = widget.NewSelect(anisoLabels, func(s string) {
		switch s {
		case "2x":
			cfg.AnisotropicFiltering = 2
		case "4x":
			cfg.AnisotropicFiltering = 4
		case "8x":
			cfg.AnisotropicFiltering = 8
		case "16x":
			cfg.AnisotropicFiltering = 16
		default:
			cfg.AnisotropicFiltering = 0
		}
	})
	v.anisoSelect.SetSelectedIndex(anisoIndexFor(cfg.AnisotropicFiltering))

	v.dofCheck = widget.NewCheck("Depth of Field (background blur)", func(b bool) {
		cfg.DepthOfField = b
	})
	v.dofCheck.SetChecked(cfg.DepthOfField)

	v.aaSelect = widget.NewSelect([]string{"Off", "FXAA", "SMAA"}, func(s string) {
		switch s {
		case "Off":
			cfg.AAMode = "off"
			cfg.AntiAliasing = false
		case "SMAA":
			cfg.AAMode = "smaa"
			cfg.AntiAliasing = true
		default:
			cfg.AAMode = "fxaa"
			cfg.AntiAliasing = true
		}
	})
	switch strings.ToLower(cfg.AAMode) {
	case "off":
		v.aaSelect.SetSelected("Off")
	case "smaa":
		v.aaSelect.SetSelected("SMAA")
	default:
		v.aaSelect.SetSelected("FXAA")
	}

	v.upscalerSelect = widget.NewSelect([]string{"Off (Bilinear)", "FSR 1.0 (EASU + RCAS)", "Anime4K"}, func(s string) {
		switch s {
		case "FSR 1.0 (EASU + RCAS)":
			cfg.Upscaler = "fsr"
		case "Anime4K":
			cfg.Upscaler = "anime4k"
		default:
			cfg.Upscaler = "off"
		}
	})
	switch strings.ToLower(cfg.Upscaler) {
	case "fsr":
		v.upscalerSelect.SetSelected("FSR 1.0 (EASU + RCAS)")
	case "anime4k":
		v.upscalerSelect.SetSelected("Anime4K")
	default:
		v.upscalerSelect.SetSelected("Off (Bilinear)")
	}

	v.motionBlurCheck = widget.NewCheck("Motion Blur", func(b bool) {
		cfg.MotionBlur = b
	})
	v.motionBlurCheck.SetChecked(cfg.MotionBlur)

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
			widget.NewLabelWithStyle("Anisotropic Filtering (Texture Sharpness):", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			v.anisoSelect,
			container.NewGridWithColumns(2,
				container.NewVBox(widget.NewLabelWithStyle("Anti-Aliasing:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}), v.aaSelect),
				container.NewVBox(widget.NewLabelWithStyle("Spatial Upscaler:", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}), v.upscalerSelect),
			),
			container.NewGridWithColumns(2,
				v.dofCheck,
				v.motionBlurCheck,
			),
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

	v.disableAutoPitchCheck = widget.NewCheck("Disable Auto Downward Camera Tilt (stops camera pulling down while moving)", func(b bool) {
		cfg.DisableAutoPitch = b
	})
	v.disableAutoPitchCheck.SetChecked(cfg.DisableAutoPitch)

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
			v.disableAutoPitchCheck,
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

	v.prevBtn = widget.NewButtonWithIcon("ROMs", theme.NavigateBackIcon(), func() {
		v.wizard.GoToStep(StepRoms)
	})

	v.nextBtn = widget.NewButtonWithIcon("Build Options", theme.NavigateNextIcon(), func() {
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
				onlineCard,
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
	cfg := &v.wizard.config
	v.pseudoEntry.SetText(cfg.PlayerName)

	hasV288 := roms.HasV288(cfg.BaseDumpPath, v.wizard.overlays, cfg.OutputDir)
	hasSource := cfg.BaseDumpPath != "" || roms.HasMetaFile(cfg.OutputDir)
	if !hasV288 {
		if hasSource {
			cfg.EnableOnline = false
		}
		v.onlineErrorLabel.Show()
	} else {
		v.onlineErrorLabel.Hide()
	}

	effectiveOnline := cfg.EnableOnline && hasV288
	v.syncingOnline = true
	v.onlineCheck.SetChecked(effectiveOnline)
	v.syncingOnline = false
	v.syncServerFields()
	v.setOnlineFieldsEnabled(effectiveOnline)

	for i, l := range settings.SupportedLanguages {
		if l.ID == cfg.LanguageID {
			v.langSelect.SetSelectedIndex(i)
			break
		}
	}

	resLabel := cfg.Resolution
	switch strings.ToLower(cfg.Resolution) {
	case "4k", "2160p", "3840x2160":
		resLabel = "4K (3840x2160)"
	case "8k", "4320p", "7680x4320":
		resLabel = "8K (7680x4320)"
	}
	v.resSelect.SetSelected(resLabel)
	v.fpsSelect.SetSelected(fmt.Sprintf("%d FPS", cfg.FrameRate))
	v.plaza60Check.SetChecked(cfg.Plaza60FPS)
	v.fullscreenCheck.SetChecked(cfg.Fullscreen)
	v.gsyncCheck.SetChecked(cfg.GSync)
	v.shaderSelect.SetSelectedIndex(cfg.ShaderQuality)
	switch cfg.AnisotropicFiltering {
	case 2:
		v.anisoSelect.SetSelectedIndex(1)
	case 4:
		v.anisoSelect.SetSelectedIndex(2)
	case 8:
		v.anisoSelect.SetSelectedIndex(3)
	case 16:
		v.anisoSelect.SetSelectedIndex(4)
	default:
		v.anisoSelect.SetSelectedIndex(0)
	}
	v.dofCheck.SetChecked(cfg.DepthOfField)
	switch strings.ToLower(cfg.AAMode) {
	case "off":
		v.aaSelect.SetSelected("Off")
	case "smaa":
		v.aaSelect.SetSelected("SMAA")
	default:
		if cfg.AntiAliasing {
			v.aaSelect.SetSelected("FXAA")
		} else {
			v.aaSelect.SetSelected("Off")
		}
	}
	switch strings.ToLower(cfg.Upscaler) {
	case "fsr":
		v.upscalerSelect.SetSelected("FSR 1.0 (EASU + RCAS)")
	case "anime4k":
		v.upscalerSelect.SetSelected("Anime4K")
	default:
		v.upscalerSelect.SetSelected("Off (Bilinear)")
	}
	v.motionBlurCheck.SetChecked(cfg.MotionBlur)

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
	v.disableAutoPitchCheck.SetChecked(cfg.DisableAutoPitch)
	v.fovSlider.SetValue(float64(cfg.CameraFOV))
	v.fovLabel.SetText(fmt.Sprintf("%d°", cfg.CameraFOV))
	v.volumeSlider.SetValue(float64(cfg.Volume))
	v.volumeLabel.SetText(fmt.Sprintf("%d%%", cfg.Volume))
	v.appImageCheck.SetChecked(cfg.CreateAppImage)
}

func (v *StepSettingsView) Container() fyne.CanvasObject {
	return v.container
}

// setOnlineFieldsEnabled enables or disables the server choice and every address field.
func (v *StepSettingsView) setOnlineFieldsEnabled(enabled bool) {
	if v.serverSelect == nil {
		return
	}
	for _, w := range []fyne.Disableable{v.serverSelect, v.onlineServerEntry, v.registerEntry, v.accountEntry,
		v.bossEntry, v.nncs1Entry, v.nncs2Entry} {
		if enabled {
			w.Enable()
		} else {
			w.Disable()
		}
	}
}

// syncServerFields shows the configuration's server addresses and the matching choice.
func (v *StepSettingsView) syncServerFields() {
	cfg := &v.wizard.config
	v.syncingServer = true
	defer func() { v.syncingServer = false }()
	values := []string{cfg.OnlineServer, cfg.OnlineRegister, cfg.OnlineAccount, cfg.OnlineBoss, cfg.OnlineNncs1, cfg.OnlineNncs2}
	for i, e := range []*widget.Entry{v.onlineServerEntry, v.registerEntry, v.accountEntry, v.bossEntry, v.nncs1Entry, v.nncs2Entry} {
		e.SetText(values[i]) // OnChanged writes the same value back
	}
	v.refreshServerSelectLocked()
}

// refreshServerSelect selects the preset matching the fields, or Custom, without applying anything.
func (v *StepSettingsView) refreshServerSelect() {
	if v.serverSelect == nil || v.syncingServer {
		return
	}
	v.syncingServer = true
	defer func() { v.syncingServer = false }()
	v.refreshServerSelectLocked()
}

func (v *StepSettingsView) refreshServerSelectLocked() {
	if i := v.wizard.config.PresetIndex(v.serverPresets); i >= 0 {
		v.serverSelect.SetSelectedIndex(i)
	} else {
		v.serverSelect.SetSelectedIndex(len(v.serverPresets))
	}
}

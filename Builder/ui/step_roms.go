package ui

import (
	"fmt"
	"os"
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/roms"
	"splatpc-builder/settings"
)

type StepRomsView struct {
	wizard *Wizard

	container       *fyne.Container
	baseDumpEntry   *widget.Entry
	baseBrowseBtn   *widget.Button
	baseStatusLbl   *widget.Label
	baseInfoCard    *widget.Card

	layersList      *fyne.Container
	addLayerBtn     *widget.Button
	layersCard      *widget.Card

	outputEntry     *widget.Entry
	outputBrowseBtn *widget.Button

	prevBtn *widget.Button
	nextBtn *widget.Button
}

func NewStepRomsView(w *Wizard) *StepRomsView {
	v := &StepRomsView{
		wizard:     w,
		layersList: container.NewVBox(),
	}
	v.initUI()
	v.refreshLayers()
	return v
}

func (v *StepRomsView) initUI() {
	title := widget.NewLabelWithStyle("Step 1: ROM Definitions, Updates & Output Folder", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	desc := widget.NewLabel("Specify your official base game dump (v16), overlay any updates (v288) or mods, and choose your output directory.")

	// Base ROM section
	v.baseDumpEntry = widget.NewEntry()
	v.baseDumpEntry.SetPlaceHolder("Path to base Splatoon dump folder (containing code, content, meta)")
	v.baseDumpEntry.OnChanged = func(val string) {
		v.wizard.config.BaseDumpPath = val
		v.validateBaseROM(val)
	}

	v.baseBrowseBtn = widget.NewButtonWithIcon("Browse...", theme.FolderOpenIcon(), func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err == nil && uri != nil {
				v.baseDumpEntry.SetText(uri.Path())
			}
		}, v.wizard.window)
	})

	v.baseStatusLbl = widget.NewLabel("Awaiting folder selection...")
	v.baseInfoCard = widget.NewCard("1. Official Base Game Dump (Splatoon v16)", "Dump containing code/, content/, and meta/ folders",
		container.NewVBox(
			container.NewBorder(nil, nil, nil, v.baseBrowseBtn, v.baseDumpEntry),
			v.baseStatusLbl,
		),
	)

	// Overlays section
	v.addLayerBtn = widget.NewButtonWithIcon("Add update or mod...", theme.ContentAddIcon(), func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err == nil && uri != nil {
				layer, err := roms.NewLayerFromPath(uri.Path())
				if err != nil {
					dialog.ShowError(err, v.wizard.window)
					return
				}
				v.wizard.overlays = append(v.wizard.overlays, layer)
				v.refreshLayers()
			}
		}, v.wizard.window)
	})

	infoNote := widget.NewLabelWithStyle(
		"💡 Merging Rule: You can layer multiple folders (mods, updates, packs).\nNote: Only the official v288 update modifies the built .rpx executable. Other mods supply content/meta assets.",
		fyne.TextAlignLeading,
		fyne.TextStyle{Italic: true},
	)

	v.layersCard = widget.NewCard("2. ROM Overlays & Mods (Priority Order)", "",
		container.NewVBox(
			infoNote,
			widget.NewSeparator(),
			v.layersList,
			container.NewHBox(v.addLayerBtn),
		),
	)

	// Output directory section
	v.outputEntry = widget.NewEntry()
	v.outputEntry.SetText(v.wizard.config.OutputDir)
	v.outputEntry.OnChanged = func(val string) {
		v.wizard.config.OutputDir = val
		iniPath := filepath.Join(val, "settings.ini")
		if _, err := os.Stat(iniPath); err == nil {
			if err := settings.LoadSettingsIni(iniPath, &v.wizard.config); err == nil {
				if v.wizard.settingsView != nil {
					v.wizard.settingsView.SyncUIFromConfig()
				}
			}
		}
	}

	v.outputBrowseBtn = widget.NewButtonWithIcon("Browse...", theme.FolderOpenIcon(), func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err == nil && uri != nil {
				v.outputEntry.SetText(uri.Path())
			}
		}, v.wizard.window)
	})

	outputCard := widget.NewCard("3. Destination Output Folder", "Directory where the ready-to-play Splatoon PC port will be installed",
		container.NewBorder(nil, nil, nil, v.outputBrowseBtn, v.outputEntry),
	)

	v.prevBtn = widget.NewButtonWithIcon("← Prerequisites", theme.NavigateBackIcon(), func() {
		v.wizard.GoToStep(StepPrereq)
	})

	v.nextBtn = widget.NewButtonWithIcon("Game Settings →", theme.NavigateNextIcon(), func() {
		v.wizard.GoToStep(StepSettings)
	})
	v.nextBtn.Importance = widget.HighImportance
	v.nextBtn.Disable()

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
				v.baseInfoCard,
				widget.NewSeparator(),
				v.layersCard,
				widget.NewSeparator(),
				outputCard,
			),
		),
	)
}

func (v *StepRomsView) validateBaseROM(path string) {
	if path == "" {
		v.baseStatusLbl.SetText("Please select your game dump folder.")
		v.nextBtn.Disable()
		return
	}

	info := roms.ValidateDump(path)
	if !info.IsValidDump {
		v.baseStatusLbl.SetText(fmt.Sprintf("❌ Error: %s", info.ErrorMessage))
		v.nextBtn.Disable()
		return
	}

	statusMsg := fmt.Sprintf("✅ Valid game dump detected: %s (Title: %s, Version: %s)",
		info.ProductCode, info.LongTitle, info.TitleVersion)
	if info.IsV16 {
		statusMsg += " [Certified v16 Base Dump]"
	}
	v.baseStatusLbl.SetText(statusMsg)
	v.nextBtn.Enable()
}

func (v *StepRomsView) refreshLayers() {
	v.layersList.Objects = nil

	if len(v.wizard.overlays) == 0 {
		v.layersList.Add(widget.NewLabelWithStyle("No overlays added yet. The base ROM will be used alone.", fyne.TextAlignCenter, fyne.TextStyle{Italic: true}))
	} else {
		for i, layer := range v.wizard.overlays {
			idx := i
			l := layer

			badge := "[Mod]"
			if l.IsV288 {
				badge = "⭐ [v288 Update (Active RPX)]"
			} else if l.HasRPX {
				badge = "⚠️ [Mod - RPX Ignored]"
			}

			lbl := widget.NewLabelWithStyle(fmt.Sprintf("#%d %s %s", idx+1, badge, l.Name), fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
			subLbl := widget.NewLabel(l.Path)

			var btnUp *widget.Button
			var btnDown *widget.Button

			btnUp = widget.NewButtonWithIcon("", theme.MoveUpIcon(), func() {
				if idx > 0 {
					v.wizard.overlays[idx], v.wizard.overlays[idx-1] = v.wizard.overlays[idx-1], v.wizard.overlays[idx]
					v.refreshLayers()
				}
			})
			if idx == 0 {
				btnUp.Disable()
			}

			btnDown = widget.NewButtonWithIcon("", theme.MoveDownIcon(), func() {
				if idx < len(v.wizard.overlays)-1 {
					v.wizard.overlays[idx], v.wizard.overlays[idx+1] = v.wizard.overlays[idx+1], v.wizard.overlays[idx]
					v.refreshLayers()
				}
			})
			if idx == len(v.wizard.overlays)-1 {
				btnDown.Disable()
			}

			btnDel := widget.NewButtonWithIcon("", theme.DeleteIcon(), func() {
				v.wizard.overlays = append(v.wizard.overlays[:idx], v.wizard.overlays[idx+1:]...)
				v.refreshLayers()
			})

			actions := container.NewHBox(btnUp, btnDown, btnDel)

			row := container.NewBorder(
				nil, nil,
				nil,
				actions,
				container.NewVBox(lbl, subLbl),
			)

			v.layersList.Add(row)
			if l.WarningMessage != "" {
				warnLbl := widget.NewLabelWithStyle("   └ "+l.WarningMessage, fyne.TextAlignLeading, fyne.TextStyle{Italic: true})
				v.layersList.Add(warnLbl)
			}
			v.layersList.Add(widget.NewSeparator())
		}
	}
	v.layersCard.Refresh()
}

func (v *StepRomsView) Container() fyne.CanvasObject {
	return v.container
}

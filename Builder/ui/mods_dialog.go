package ui

import (
	"context"
	"fmt"
	"net/url"
	"path/filepath"
	"sync"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/engine"
	"splatpc-builder/mods"
)

func (w *Wizard) modsButton() *widget.Button {
	return widget.NewButtonWithIcon("Mods", theme.DownloadIcon(), w.showModsManager)
}

type catalogRowState struct {
	entry        mods.ModEntry
	updateInfo   mods.UpdateInfo
	hasUpdateErr bool
	updateErrMsg string
	busy         bool

	statusLabel  *widget.Label
	progressBar  *widget.ProgressBar
	enabledCheck *widget.Check
	actionBtn    *widget.Button
	deleteBtn    *widget.Button
	refreshFn    func()
}

func (w *Wizard) showModsManager() {
	outDir := w.config.OutputDir
	modsDir := mods.ModsDir(outDir)
	catalog := mods.CatalogWithExtras(mods.DefaultExtrasPath())

	headerPath := wrappedLabel(fmt.Sprintf("Mods folder: %s", modsDir))
	statusBar := widget.NewLabel("")
	statusBar.TextStyle = fyne.TextStyle{Italic: true}

	openModsBtn := widget.NewButtonWithIcon("Open Folder", theme.FolderOpenIcon(), func() {
		if err := engine.OpenFolder(modsDir); err != nil {
			dialog.ShowError(err, w.window)
		}
	})

	rowStates := make([]*catalogRowState, 0, len(catalog))
	otherModsBox := container.NewVBox()

	var updateAllBtn *widget.Button
	var checkUpdatesBtn *widget.Button

	refreshUpdateAllButton := func() {
		updates := 0
		for _, r := range rowStates {
			if r.updateInfo.UpdateAvailable {
				updates++
			}
		}
		if updates > 0 {
			updateAllBtn.SetText(fmt.Sprintf("Update All (%d)", updates))
			updateAllBtn.Enable()
		} else {
			updateAllBtn.SetText("Update All")
			updateAllBtn.Disable()
		}
	}

	var refreshOtherMods func()
	refreshOtherMods = func() {
		otherModsBox.Objects = nil
		files, err := mods.ListInstalledFiles(outDir, catalog)
		if err != nil {
			otherModsBox.Add(widget.NewLabel(fmt.Sprintf("Could not scan mods folder: %v", err)))
			otherModsBox.Refresh()
			return
		}
		var extras []mods.LocalMod
		for _, f := range files {
			if f.CatalogID == "" {
				extras = append(extras, f)
			}
		}
		if len(extras) == 0 {
			otherModsBox.Add(widget.NewLabelWithStyle("No other mod files found in mods/.", fyne.TextAlignLeading, fyne.TextStyle{Italic: true}))
		} else {
			for _, f := range extras {
				local := f
				lbl := widget.NewLabel(fmt.Sprintf("%s  (%s)", local.FileName, engine.FormatBytes(local.SizeBytes)))
				delBtn := widget.NewButtonWithIcon("Delete", theme.DeleteIcon(), func() {
					msg := fmt.Sprintf("Delete %s from %s?\n\nThis permanently removes the mod file.", local.FileName, modsDir)
					dialog.ShowConfirm("Delete Mod", msg, func(confirm bool) {
						if !confirm {
							return
						}
						if err := mods.DeleteFile(outDir, local.FileName, catalog); err != nil {
							dialog.ShowError(err, w.window)
							return
						}
						refreshOtherMods()
						if w.doneView != nil {
							w.doneView.RefreshModsSummary()
						}
					}, w.window)
				})
				otherModsBox.Add(container.NewBorder(nil, nil, nil, delBtn, lbl))
			}
		}
		otherModsBox.Refresh()
	}

	runCheckUpdates := func() {
		checkUpdatesBtn.Disable()
		statusBar.SetText("Checking GitHub releases for mod updates...")
		go func() {
			var wg sync.WaitGroup
			results := make([]mods.UpdateInfo, len(rowStates))
			errs := make([]error, len(rowStates))
			for i, r := range rowStates {
				wg.Add(1)
				go func(idx int, entry mods.ModEntry) {
					defer wg.Done()
					info, err := mods.CheckUpdate(context.Background(), outDir, entry)
					results[idx] = info
					errs[idx] = err
				}(i, r.entry)
			}
			wg.Wait()
			fyne.Do(func() {
				checkUpdatesBtn.Enable()
				availCount := 0
				errCount := 0
				for i, r := range rowStates {
					if errs[i] != nil {
						r.hasUpdateErr = true
						r.updateErrMsg = errs[i].Error()
						errCount++
					} else {
						r.hasUpdateErr = false
						r.updateErrMsg = ""
						r.updateInfo = results[i]
						if results[i].UpdateAvailable {
							availCount++
						}
					}
					r.refreshFn()
				}
				refreshUpdateAllButton()
				switch {
				case availCount > 0:
					statusBar.SetText(fmt.Sprintf("%d update(s) available.", availCount))
				case errCount > 0:
					statusBar.SetText("Could not reach GitHub for some mods.")
				default:
					statusBar.SetText("All checked mods are up to date.")
				}
			})
		}()
	}

	checkUpdatesBtn = widget.NewButtonWithIcon("Check Updates", theme.ViewRefreshIcon(), runCheckUpdates)

	updateAllBtn = widget.NewButtonWithIcon("Update All", theme.DownloadIcon(), func() {
		var toUpdate []*catalogRowState
		for _, r := range rowStates {
			if r.updateInfo.UpdateAvailable && !r.busy {
				toUpdate = append(toUpdate, r)
			}
		}
		if len(toUpdate) == 0 {
			return
		}
		updateAllBtn.Disable()
		for _, r := range toUpdate {
			r.busy = true
			r.refreshFn()
		}
		go func() {
			for _, r := range toUpdate {
				row := r
				fyne.Do(func() {
					row.progressBar.SetValue(0)
					row.progressBar.Show()
					row.statusLabel.SetText(fmt.Sprintf("Updating to %s...", row.updateInfo.LatestVersion))
				})
				err := mods.InstallUpdate(context.Background(), outDir, row.entry, row.updateInfo, func(downloaded, total int64) {
					fyne.Do(func() {
						if total > 0 {
							row.progressBar.SetValue(float64(downloaded) / float64(total))
						}
					})
				})
				fyne.Do(func() {
					row.busy = false
					row.progressBar.Hide()
					if err != nil {
						dialog.ShowError(err, w.window)
					} else {
						row.updateInfo.InstalledVersion = row.updateInfo.LatestVersion
						row.updateInfo.UpdateAvailable = false
					}
					row.refreshFn()
					refreshUpdateAllButton()
					refreshOtherMods()
				})
			}
		}()
	})
	updateAllBtn.Importance = widget.HighImportance
	updateAllBtn.Disable()

	addFileBtn := widget.NewButtonWithIcon("Add Mod File...", theme.FileIcon(), func() {
		fd := dialog.NewFileOpen(func(uc fyne.URIReadCloser, err error) {
			if err != nil {
				dialog.ShowError(err, w.window)
				return
			}
			if uc == nil {
				return
			}
			srcPath := uc.URI().Path()
			_ = uc.Close()
			installedName, installErr := mods.InstallLocalFile(outDir, srcPath)
			if installErr != nil {
				dialog.ShowError(installErr, w.window)
				return
			}
			for _, r := range rowStates {
				r.refreshFn()
			}
			refreshOtherMods()
			if w.doneView != nil {
				w.doneView.RefreshModsSummary()
			}
			statusBar.SetText(fmt.Sprintf("Added %s to mods folder.", filepath.Base(installedName)))
		}, w.window)
		fd.Show()
	})

	catalogRowsBox := container.NewVBox()
	for _, entry := range catalog {
		r, rowObj := w.buildCatalogModRow(outDir, entry, func() {
			refreshUpdateAllButton()
			refreshOtherMods()
			if w.doneView != nil {
				w.doneView.RefreshModsSummary()
			}
		})
		rowStates = append(rowStates, r)
		catalogRowsBox.Add(rowObj)
		catalogRowsBox.Add(widget.NewSeparator())
	}

	toolbar := container.NewHBox(checkUpdatesBtn, updateAllBtn, addFileBtn, openModsBtn)
	topArea := container.NewVBox(
		container.NewBorder(nil, nil, nil, toolbar, headerPath),
		statusBar,
		widget.NewSeparator(),
	)

	scrollContent := container.NewVBox(
		widget.NewLabelWithStyle("Downloadable Mods", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
		catalogRowsBox,
		widget.NewLabelWithStyle("Other Installed Mods (mods/)", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
		otherModsBox,
	)

	note := widget.NewLabelWithStyle(
		"Enable, disable, or delete mods in-game from ESC -> Mods or right here.",
		fyne.TextAlignLeading,
		fyne.TextStyle{Italic: true},
	)
	note.Wrapping = fyne.TextWrapWord

	content := container.NewBorder(
		topArea,
		container.NewVBox(widget.NewSeparator(), note),
		nil,
		nil,
		container.NewVScroll(scrollContent),
	)

	refreshOtherMods()

	d := dialog.NewCustom("Mods", "Close", content, w.window)
	d.Resize(fyne.NewSize(780, 540))
	d.Show()

	runCheckUpdates()
}

func (w *Wizard) buildCatalogModRow(outDir string, entry mods.ModEntry, onChanged func()) (*catalogRowState, fyne.CanvasObject) {
	r := &catalogRowState{
		entry:        entry,
		statusLabel:  widget.NewLabel(""),
		progressBar:  widget.NewProgressBar(),
		enabledCheck: widget.NewCheck("Enabled", nil),
	}
	r.statusLabel.TextStyle = fyne.TextStyle{Italic: true}
	r.progressBar.Hide()

	r.enabledCheck.OnChanged = func(b bool) {
		if err := mods.SetModDisabled(outDir, entry.ID, !b); err != nil {
			dialog.ShowError(err, w.window)
		}
		if onChanged != nil {
			onChanged()
		}
	}

	r.refreshFn = func() {
		installed := mods.IsInstalled(outDir, entry)
		disabled := mods.LoadDisabledMods(outDir)[entry.ID]

		if installed {
			ver := mods.InstalledVersion(outDir, entry)
			switch {
			case r.updateInfo.UpdateAvailable:
				r.statusLabel.SetText(fmt.Sprintf("Update available: %s -> %s", ver, r.updateInfo.LatestVersion))
				r.actionBtn.SetText(fmt.Sprintf("Update (%s)", r.updateInfo.LatestVersion))
			case ver != "":
				r.statusLabel.SetText("Installed (" + ver + ")")
				r.actionBtn.SetText("Reinstall")
			default:
				r.statusLabel.SetText("Installed")
				r.actionBtn.SetText("Update / Reinstall")
			}
			r.enabledCheck.SetChecked(!disabled)
			r.enabledCheck.Show()
			r.deleteBtn.Show()
		} else {
			if r.hasUpdateErr {
				r.statusLabel.SetText("Not installed")
			} else if r.updateInfo.LatestVersion != "" {
				r.statusLabel.SetText("Available (" + r.updateInfo.LatestVersion + ")")
			} else {
				r.statusLabel.SetText("Not installed")
			}
			r.actionBtn.SetText("Install")
			r.enabledCheck.Hide()
			r.deleteBtn.Hide()
		}

		if r.busy {
			r.actionBtn.Disable()
			r.deleteBtn.Disable()
			r.enabledCheck.Disable()
		} else {
			r.actionBtn.Enable()
			r.deleteBtn.Enable()
			r.enabledCheck.Enable()
		}
	}

	r.actionBtn = widget.NewButtonWithIcon("Install", theme.DownloadIcon(), func() {
		r.busy = true
		r.progressBar.SetValue(0)
		r.progressBar.Show()
		r.statusLabel.SetText("Downloading...")
		r.refreshFn()

		info := r.updateInfo
		if info.AssetURL == "" && info.LatestVersion == "" {
			info = mods.UpdateInfo{
				ModID:         entry.ID,
				LatestVersion: entry.Version,
				ReleaseURL:    entry.HomepageURL,
			}
		}

		go func() {
			err := mods.InstallUpdate(context.Background(), outDir, entry, info, func(downloaded, total int64) {
				fyne.Do(func() {
					if total > 0 {
						r.progressBar.SetValue(float64(downloaded) / float64(total))
						r.statusLabel.SetText(fmt.Sprintf("Downloading (%s / %s)...",
							engine.FormatBytes(downloaded), engine.FormatBytes(total)))
					} else {
						r.statusLabel.SetText(fmt.Sprintf("Downloading (%s)...",
							engine.FormatBytes(downloaded)))
					}
				})
			})
			fyne.Do(func() {
				r.busy = false
				r.progressBar.Hide()
				if err != nil {
					r.statusLabel.SetText("Download failed")
					dialog.ShowError(err, w.window)
				} else {
					if info.LatestVersion != "" {
						r.updateInfo.InstalledVersion = info.LatestVersion
					} else {
						r.updateInfo.InstalledVersion = entry.Version
					}
					r.updateInfo.UpdateAvailable = false
				}
				r.refreshFn()
				if onChanged != nil {
					onChanged()
				}
			})
		}()
	})
	r.actionBtn.Importance = widget.HighImportance

	r.deleteBtn = widget.NewButtonWithIcon("Delete", theme.DeleteIcon(), func() {
		msg := fmt.Sprintf("Delete %s from %s?", entry.Name, mods.ModsDir(outDir))
		dialog.ShowConfirm("Delete Mod", msg, func(confirm bool) {
			if !confirm {
				return
			}
			if err := mods.Uninstall(outDir, entry); err != nil {
				dialog.ShowError(err, w.window)
				return
			}
			_ = mods.SetModDisabled(outDir, entry.ID, false)
			r.updateInfo.UpdateAvailable = false
			r.updateInfo.InstalledVersion = ""
			r.refreshFn()
			if onChanged != nil {
				onChanged()
			}
		}, w.window)
	})

	var releaseLink fyne.CanvasObject = widget.NewLabel("")
	if u, err := url.Parse(entry.HomepageURL); err == nil && entry.HomepageURL != "" {
		releaseLink = widget.NewHyperlink("Release Page", u)
	}

	r.refreshFn()

	title := widget.NewLabelWithStyle(
		fmt.Sprintf("%s  %s  (by %s)", entry.Name, entry.Version, entry.Author),
		fyne.TextAlignLeading,
		fyne.TextStyle{Bold: true},
	)
	desc := wrappedLabel(entry.Description)
	actions := container.NewHBox(r.statusLabel, r.enabledCheck, releaseLink, r.actionBtn, r.deleteBtn)
	header := container.NewBorder(nil, nil, nil, actions, title)
	return r, container.NewPadded(container.NewVBox(header, desc, r.progressBar))
}

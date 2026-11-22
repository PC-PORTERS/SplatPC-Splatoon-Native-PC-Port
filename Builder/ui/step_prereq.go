package ui

import (
	"context"
	"fmt"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/engine"
	"splatpc-builder/prereq"
)

type StepPrereqView struct {
	wizard *Wizard
	report prereq.CheckReport

	container      *fyne.Container
	statusCard     *widget.Card
	itemsList      *fyne.Container
	userspaceCard  *widget.Card
	userspaceInfo  *widget.Label
	downloadBtn    *widget.Button
	cancelDlBtn    *widget.Button
	downloadBar    *widget.ProgressBarInfinite
	downloadStatus *widget.Label
	removeToolsBtn *widget.Button
	installCard    *widget.Card
	installEntry   *widget.Entry
	installBtn     *widget.Button
	nextBtn        *widget.Button
	refreshBtn     *widget.Button
	copyBtn        *widget.Button

	dlCancel context.CancelFunc
}

func NewStepPrereqView(w *Wizard) *StepPrereqView {
	v := &StepPrereqView{
		wizard:    w,
		itemsList: container.NewVBox(),
	}
	v.initUI()
	return v
}

func (v *StepPrereqView) initUI() {
	title := widget.NewLabelWithStyle("Step -1: System Prerequisites Check", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	desc := widget.NewLabel("Verify required compilers, tools, and libraries. Missing dependencies can be downloaded 100% in userspace from official sources.")

	v.refreshBtn = widget.NewButtonWithIcon("Re-check system", theme.ViewRefreshIcon(), func() {
		v.Refresh()
	})

	v.nextBtn = widget.NewButtonWithIcon("Proceed to ROM Definition", theme.NavigateNextIcon(), func() {
		v.wizard.GoToStep(StepRoms)
	})
	v.nextBtn.Importance = widget.HighImportance

	// Userspace downloader card
	v.userspaceInfo = wrappedLabel("")
	v.downloadBtn = widget.NewButtonWithIcon("Download & Install Dependencies (100% Userspace)", theme.DownloadIcon(), v.downloadUserspaceDeps)
	v.downloadBtn.Importance = widget.HighImportance

	v.cancelDlBtn = widget.NewButtonWithIcon("Cancel", theme.CancelIcon(), func() {
		if v.dlCancel != nil {
			v.dlCancel()
		}
	})
	v.cancelDlBtn.Hide()

	v.removeToolsBtn = widget.NewButtonWithIcon("Delete Userspace Tools", theme.DeleteIcon(), v.confirmRemoveUserspaceTools)
	v.removeToolsBtn.Hide()

	v.downloadBar = widget.NewProgressBarInfinite()
	v.downloadBar.Hide()

	v.downloadStatus = wrappedLabel("")
	v.downloadStatus.Hide()

	v.userspaceCard = widget.NewCard(
		"Userspace Dependencies (Official Sources, No Root/Admin)",
		"Downloads directly into "+prereq.UserToolsDir()+" and can be deleted after the build.",
		container.NewVBox(
			v.userspaceInfo,
			v.downloadBar,
			v.downloadStatus,
			container.NewHBox(v.downloadBtn, v.cancelDlBtn, v.removeToolsBtn),
		),
	)

	// Optional fallback system package manager card
	v.installEntry = widget.NewMultiLineEntry()
	v.installEntry.TextStyle = fyne.TextStyle{Monospace: true}
	v.installEntry.Wrapping = fyne.TextWrapWord

	v.copyBtn = widget.NewButtonWithIcon("Copy", theme.ContentCopyIcon(), func() {
		v.wizard.window.Clipboard().SetContent(v.installEntry.Text)
		dialog.ShowInformation("Copied", "Command copied to clipboard!", v.wizard.window)
	})

	v.installBtn = widget.NewButtonWithIcon("Run in Terminal (System)", theme.ComputerIcon(), v.installMissing)

	v.installCard = widget.NewCard("System Package Manager (Alternative)", "Optional: install system-wide packages instead of userspace downloads.",
		container.NewVBox(
			v.installEntry,
			container.NewHBox(v.installBtn, v.copyBtn),
		),
	)

	v.statusCard = widget.NewCard("Dependency Status", "", v.itemsList)

	topBar := container.NewVBox(
		title,
		desc,
		widget.NewSeparator(),
	)

	bottomBar := container.NewHBox(
		v.refreshBtn,
		layout.NewSpacer(),
		v.nextBtn,
	)

	v.container = container.NewBorder(
		topBar,
		bottomBar,
		nil,
		nil,
		container.NewVScroll(
			container.NewVBox(
				v.statusCard,
				v.userspaceCard,
				v.installCard,
			),
		),
	)
}

// Refresh runs the checks again in the background; the buttons wait meanwhile.
func (v *StepPrereqView) Refresh() {
	v.refreshBtn.Disable()
	v.nextBtn.Disable()
	v.refreshBtn.SetText("Checking...")
	go func() {
		report := prereq.RunChecks()
		fyne.Do(func() {
			v.refreshBtn.SetText("Re-check system")
			v.refreshBtn.Enable()
			v.ShowReport(report)
		})
	}()
}

func (v *StepPrereqView) ShowReport(report prereq.CheckReport) {
	v.report = report
	v.itemsList.Objects = nil

	for _, item := range v.report.Items {
		var icon fyne.Resource
		statusText := ""

		if item.Found {
			icon = theme.ConfirmIcon()
			if item.Version != "" {
				statusText = fmt.Sprintf("OK (%s)", item.Version)
			} else {
				statusText = "OK"
			}
		} else {
			if item.Required {
				icon = theme.CancelIcon()
				statusText = fmt.Sprintf("MISSING: %s", item.ErrorMsg)
			} else {
				icon = theme.WarningIcon()
				statusText = fmt.Sprintf("Optional: %s", item.ErrorMsg)
			}
		}

		nameLabel := widget.NewLabelWithStyle(item.Name, fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
		badgeLabel := widget.NewLabel(statusText)
		catBadge := widget.NewLabel(fmt.Sprintf("[%s]", item.Category))

		row := container.NewBorder(
			nil, nil,
			container.NewHBox(widget.NewIcon(icon), nameLabel, catBadge),
			nil,
			badgeLabel,
		)
		v.itemsList.Add(row)
		v.itemsList.Add(widget.NewSeparator())
	}

	v.updateUserspaceCard()

	if v.report.InstallScript != "" && !v.report.AllRequiredMet {
		v.installEntry.SetText(v.report.InstallScript)
		v.installCard.Show()
	} else {
		v.installCard.Hide()
	}
	if len(v.report.InstallSteps) > 0 {
		v.installBtn.Show()
	} else {
		v.installBtn.Hide()
	}

	if v.report.AllRequiredMet {
		v.nextBtn.Enable()
		v.nextBtn.SetText("All tools ready - Proceed to ROMs")
	} else {
		v.nextBtn.Disable()
		v.nextBtn.SetText("Required tools are missing")
	}

	v.container.Refresh()
}

func (v *StepPrereqView) updateUserspaceCard() {
	missingIDs := v.report.MissingDownloadable
	toolsSize := prereq.UserspaceToolsSize()

	if len(missingIDs) == 0 && toolsSize == 0 {
		v.userspaceCard.Hide()
		return
	}

	v.userspaceCard.Show()
	if len(missingIDs) > 0 {
		v.userspaceInfo.SetText("Ready to download from official sources: " + strings.Join(missingIDs, ", "))
		v.downloadBtn.Show()
		v.downloadBtn.Enable()
	} else {
		v.userspaceInfo.SetText(fmt.Sprintf("Userspace tools installed (%s).", engine.FormatBytes(toolsSize)))
		v.downloadBtn.Hide()
	}

	if toolsSize > 0 {
		v.removeToolsBtn.SetText(fmt.Sprintf("Delete Userspace Tools (%s)", engine.FormatBytes(toolsSize)))
		v.removeToolsBtn.Show()
		v.removeToolsBtn.Enable()
	} else {
		v.removeToolsBtn.Hide()
	}
}

func (v *StepPrereqView) downloadUserspaceDeps() {
	ids := v.report.MissingDownloadable
	if len(ids) == 0 {
		return
	}

	ctx, cancel := context.WithCancel(context.Background())
	v.dlCancel = cancel

	v.downloadBtn.Disable()
	v.removeToolsBtn.Disable()
	v.refreshBtn.Disable()
	v.nextBtn.Disable()
	v.cancelDlBtn.Show()
	v.downloadBar.Show()
	v.downloadBar.Start()
	v.downloadStatus.SetText("Starting download...")
	v.downloadStatus.Show()

	go func() {
		err := prereq.InstallUserspaceTools(ctx, ids, func(format string, args ...interface{}) {
			msg := fmt.Sprintf(format, args...)
			fyne.Do(func() {
				v.downloadStatus.SetText(msg)
			})
		})
		fyne.Do(func() {
			v.dlCancel = nil
			v.downloadBar.Stop()
			v.downloadBar.Hide()
			v.cancelDlBtn.Hide()
			v.refreshBtn.Enable()
			v.downloadBtn.Enable()
			v.removeToolsBtn.Enable()
			if err != nil {
				if ctx.Err() == context.Canceled {
					v.downloadStatus.SetText("Download cancelled.")
				} else {
					v.downloadStatus.SetText("Error: " + err.Error())
					dialog.ShowError(err, v.wizard.window)
				}
			} else {
				v.downloadStatus.SetText("All userspace dependencies installed.")
			}
			v.Refresh()
		})
	}()
}

func (v *StepPrereqView) confirmRemoveUserspaceTools() {
	sz := prereq.UserspaceToolsSize()
	msg := fmt.Sprintf("Delete all userspace tools in:\n%s\n\nThis frees %s. You can re-download them anytime.", prereq.UserToolsDir(), engine.FormatBytes(sz))
	dialog.ShowConfirm("Delete Userspace Tools", msg, func(ok bool) {
		if !ok {
			return
		}
		if err := prereq.RemoveUserspaceTools(); err != nil {
			dialog.ShowError(err, v.wizard.window)
			return
		}
		v.downloadStatus.Hide()
		v.Refresh()
	}, v.wizard.window)
}

// installMissing runs the install commands in a terminal window of their own.
func (v *StepPrereqView) installMissing() {
	if err := prereq.OpenInstallTerminal(v.report.InstallSteps); err != nil {
		dialog.ShowError(err, v.wizard.window)
		return
	}
	dialog.ShowInformation("Installing the missing tools",
		"A terminal window opened to install the system tools.\n\nWhen it is done, click \"Re-check system\".", v.wizard.window)
}

func (v *StepPrereqView) Container() fyne.CanvasObject {
	return v.container
}

package ui

import (
	"fmt"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/prereq"
)

type StepPrereqView struct {
	wizard *Wizard
	report prereq.CheckReport

	container    *fyne.Container
	statusCard   *widget.Card
	itemsList    *fyne.Container
	installCard  *widget.Card
	installEntry *widget.Entry
	nextBtn      *widget.Button
	refreshBtn   *widget.Button
	copyBtn      *widget.Button
}

func NewStepPrereqView(w *Wizard) *StepPrereqView {
	v := &StepPrereqView{
		wizard:    w,
		itemsList: container.NewVBox(),
	}
	v.initUI()
	v.Refresh()
	return v
}

func (v *StepPrereqView) initUI() {
	title := widget.NewLabelWithStyle("Step -1: System Prerequisites Check", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	desc := widget.NewLabel("Before building the native PC port, let's verify that your system has all required compilers, tools, and libraries.")

	v.refreshBtn = widget.NewButtonWithIcon("Re-check system", theme.ViewRefreshIcon(), func() {
		v.Refresh()
	})

	v.nextBtn = widget.NewButtonWithIcon("Proceed to ROM Definition →", theme.NavigateNextIcon(), func() {
		v.wizard.GoToStep(StepRoms)
	})
	v.nextBtn.Importance = widget.HighImportance

	v.installEntry = widget.NewEntry()
	v.installEntry.TextStyle = fyne.TextStyle{Monospace: true}

	v.copyBtn = widget.NewButtonWithIcon("Copy", theme.ContentCopyIcon(), func() {
		v.wizard.window.Clipboard().SetContent(v.installEntry.Text)
		dialog.ShowInformation("Copied", "Command copied to clipboard!", v.wizard.window)
	})

	v.installCard = widget.NewCard("Missing Tools Detected - Recommended Installation Command", "",
		container.NewBorder(nil, nil, nil, v.copyBtn, v.installEntry),
	)

	v.statusCard = widget.NewCard("Dependency Status", "", v.itemsList)

	topBar := container.NewVBox(
		title,
		desc,
		widget.NewSeparator(),
	)

	bottomBar := container.NewHBox(
		v.refreshBtn,
		container.NewHBox(),
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
				v.installCard,
			),
		),
	)
}

func (v *StepPrereqView) Refresh() {
	v.report = prereq.RunChecks()
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

	if v.report.InstallScript != "" {
		v.installEntry.SetText(v.report.InstallScript)
		v.installCard.Show()
	} else {
		v.installCard.Hide()
	}

	if v.report.AllRequiredMet {
		v.nextBtn.Enable()
		v.nextBtn.SetText("All tools ready - Proceed to ROMs →")
	} else {
		v.nextBtn.Disable()
		v.nextBtn.SetText("Required tools are missing")
	}

	v.container.Refresh()
}

func (v *StepPrereqView) Container() fyne.CanvasObject {
	return v.container
}

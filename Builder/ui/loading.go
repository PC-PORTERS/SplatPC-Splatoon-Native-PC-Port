package ui

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/widget"
)

// loadingView is shown while the Builder checks the system at startup.
func loadingView() fyne.CanvasObject {
	logo := canvas.NewImageFromResource(AppIcon)
	logo.FillMode = canvas.ImageFillContain
	logo.SetMinSize(fyne.NewSize(96, 96))

	title := widget.NewLabelWithStyle("SplatPC Builder", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})
	status := widget.NewLabelWithStyle("Checking your system...", fyne.TextAlignCenter, fyne.TextStyle{})
	bar := widget.NewProgressBarInfinite()
	barBox := container.NewGridWrap(fyne.NewSize(320, bar.MinSize().Height), bar)

	return container.NewCenter(container.NewVBox(logo, title, status, container.NewCenter(barBox)))
}

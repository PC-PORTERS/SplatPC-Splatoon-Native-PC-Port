package ui

import (
	_ "embed"

	"fyne.io/fyne/v2"
)

//go:embed icon.png
var iconPNG []byte
var AppIcon = fyne.NewStaticResource("icon.png", iconPNG)

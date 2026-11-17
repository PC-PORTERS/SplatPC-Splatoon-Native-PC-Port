package ui

import (
	_ "embed"

	"fyne.io/fyne/v2"
)

var iconPNG []byte
var AppIcon = fyne.NewStaticResource("icon.png", iconPNG)

package ui

import (
	"image/color"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/theme"
)

type SplatTheme struct{}

var _ fyne.Theme = (*SplatTheme)(nil)

func (m *SplatTheme) Color(name fyne.ThemeColorName, variant fyne.ThemeVariant) color.Color {
	switch name {
	case theme.ColorNameBackground:
		return color.NRGBA{R: 20, G: 24, B: 33, A: 255} // Dark slate navy
	case theme.ColorNameButton:
		return color.NRGBA{R: 35, G: 43, B: 58, A: 255}
	case theme.ColorNameDisabledButton:
		return color.NRGBA{R: 28, G: 32, B: 42, A: 255}
	case theme.ColorNamePrimary:
		return color.NRGBA{R: 0, G: 212, B: 170, A: 255} // Neon Splatoon Ink Turquoise
	case theme.ColorNameHover:
		return color.NRGBA{R: 50, G: 65, B: 88, A: 255}
	case theme.ColorNameFocus:
		return color.NRGBA{R: 0, G: 212, B: 170, A: 140}
	case theme.ColorNameForeground:
		return color.NRGBA{R: 245, G: 247, B: 250, A: 255}
	case theme.ColorNameForegroundOnError:
		return color.NRGBA{R: 255, G: 255, B: 255, A: 255}
	case theme.ColorNameError:
		return color.NRGBA{R: 255, G: 75, B: 75, A: 255}
	case theme.ColorNameSuccess:
		return color.NRGBA{R: 70, G: 220, B: 120, A: 255}
	case theme.ColorNameWarning:
		return color.NRGBA{R: 255, G: 165, B: 0, A: 255}
	case theme.ColorNameInputBackground:
		return color.NRGBA{R: 28, G: 35, B: 48, A: 255}
	case theme.ColorNameInputBorder:
		return color.NRGBA{R: 55, G: 70, B: 95, A: 255}
	case theme.ColorNameShadow:
		return color.NRGBA{R: 0, G: 0, B: 0, A: 100}
	case theme.ColorNameScrollBar:
		return color.NRGBA{R: 80, G: 95, B: 125, A: 200}
	case theme.ColorNameOverlayBackground:
		return color.NRGBA{R: 15, G: 18, B: 25, A: 240}
	default:
		return theme.DefaultTheme().Color(name, theme.VariantDark)
	}
}

func (m *SplatTheme) Font(style fyne.TextStyle) fyne.Resource {
	return theme.DefaultTheme().Font(style)
}

func (m *SplatTheme) Icon(name fyne.ThemeIconName) fyne.Resource {
	return theme.DefaultTheme().Icon(name)
}

func (m *SplatTheme) Size(name fyne.ThemeSizeName) float32 {
	switch name {
	case theme.SizeNameText:
		return 13.5
	case theme.SizeNameHeadingText:
		return 22.0
	case theme.SizeNameSubHeadingText:
		return 16.0
	case theme.SizeNamePadding:
		return 7.0
	case theme.SizeNameInlineIcon:
		return 20.0
	case theme.SizeNameScrollBar:
		return 12.0
	default:
		return theme.DefaultTheme().Size(name)
	}
}

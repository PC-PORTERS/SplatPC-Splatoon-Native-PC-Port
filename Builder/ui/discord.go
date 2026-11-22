package ui

import (
	"fmt"
	"net/url"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

const discordInviteURL = "https://discord.gg/ACacguZEfg"

// discordIcon is a speech bubble, tinted by the theme like the built-in icons
var discordIcon = theme.NewThemedResource(fyne.NewStaticResource("discord.svg", []byte(
	`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">`+
		`<path d="M4 3h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H9l-5 4v-4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/>`+
		`</svg>`)))

// openDiscord opens the community Discord server in the default browser
func (w *Wizard) openDiscord() {
	u, err := url.Parse(discordInviteURL)
	if err == nil {
		err = w.app.OpenURL(u)
	}
	if err != nil {
		dialog.ShowError(fmt.Errorf("could not open %s: %w", discordInviteURL, err), w.window)
	}
}

// discordButton sits next to the troubleshooting button in the top bar
func (w *Wizard) discordButton() *widget.Button {
	return widget.NewButtonWithIcon("Discord", discordIcon, w.openDiscord)
}

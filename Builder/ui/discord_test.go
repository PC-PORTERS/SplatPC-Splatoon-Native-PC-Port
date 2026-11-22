package ui

import (
	"net/url"
	"testing"
)

func TestDiscordInviteURL(t *testing.T) {
	u, err := url.Parse(discordInviteURL)
	if err != nil {
		t.Fatalf("invite URL does not parse: %v", err)
	}
	if u.Scheme != "https" || u.Host != "discord.gg" || u.Path == "" {
		t.Fatalf("unexpected invite URL %q", discordInviteURL)
	}
}

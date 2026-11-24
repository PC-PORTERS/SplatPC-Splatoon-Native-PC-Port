package ui

import (
	"os"
	"path/filepath"
	"testing"

	"fyne.io/fyne/v2/test"

	"splatpc-builder/roms"
	"splatpc-builder/settings"
)

func TestStepSettingsOnlineRequiresV288(t *testing.T) {
	test.NewApp()
	outDir := t.TempDir()
	metaDir := filepath.Join(outDir, "meta")
	if err := os.MkdirAll(metaDir, 0755); err != nil {
		t.Fatal(err)
	}
	// 1. Existing v16 install: online must be refused with error message visible
	if err := os.WriteFile(filepath.Join(metaDir, "meta.xml"), []byte("<menu><title_version>16</title_version></menu>"), 0644); err != nil {
		t.Fatal(err)
	}
	cfg := settings.DefaultConfiguration(t.TempDir())
	cfg.OutputDir = outDir
	cfg.EnableOnline = true
	w := &Wizard{config: cfg}

	view := NewStepSettingsView(w)
	if view.onlineCheck.Checked || w.config.EnableOnline {
		t.Fatalf("expected EnableOnline to be disabled on v16")
	}
	if !view.onlineErrorLabel.Visible() {
		t.Fatalf("expected v288 error label to be visible on v16")
	}
	// Attempting to check Enable Online on v16 must keep it unchecked
	view.onlineCheck.SetChecked(true)
	if view.onlineCheck.Checked || w.config.EnableOnline {
		t.Fatalf("expected onlineCheck to stay unchecked without v288")
	}

	// 2. Adding v288 overlay allows enabling online and hides error
	w.overlays = []roms.RomLayer{{IsV288: true}}
	w.config.EnableOnline = true
	view.SyncUIFromConfig()
	if !view.onlineCheck.Checked || !w.config.EnableOnline {
		t.Fatalf("expected onlineCheck to be checked when v288 is present")
	}
	if view.onlineServerEntry.Disabled() {
		t.Fatalf("expected onlineServerEntry to be enabled when v288 is present and online is checked")
	}
	if view.onlineErrorLabel.Visible() {
		t.Fatalf("expected v288 error label to be hidden when v288 is present")
	}
}

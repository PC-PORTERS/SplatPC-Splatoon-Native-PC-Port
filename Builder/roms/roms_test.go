package roms

import (
	"os"
	"path/filepath"
	"testing"
)

func TestValidateDumpStructure(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "splatpc_test_dump_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	// Incomplete dump
	info := ValidateDump(tempDir)
	if info.IsValidDump {
		t.Errorf("Expected incomplete dump to be invalid")
	}

	// Create valid structure
	_ = os.MkdirAll(filepath.Join(tempDir, "code"), 0755)
	_ = os.MkdirAll(filepath.Join(tempDir, "content"), 0755)
	_ = os.MkdirAll(filepath.Join(tempDir, "meta"), 0755)
	_ = os.WriteFile(filepath.Join(tempDir, "code", "Gambit.rpx"), []byte("FAKE_RPX"), 0644)
	metaXml := `<menu><title_version>16</title_version><product_code>WUP-P-AGMP</product_code></menu>`
	_ = os.WriteFile(filepath.Join(tempDir, "meta", "meta.xml"), []byte(metaXml), 0644)

	info = ValidateDump(tempDir)
	if !info.IsValidDump {
		t.Errorf("Expected dump to be valid: %s", info.ErrorMessage)
	}
	if !info.IsV16 {
		t.Errorf("Expected v16 dump to be identified as v16")
	}
	if info.ProductCode != "WUP-P-AGMP" {
		t.Errorf("Expected product code WUP-P-AGMP, got %s", info.ProductCode)
	}
}

func TestCombineROMsWithOverlays(t *testing.T) {
	baseDir, _ := os.MkdirTemp("", "test_base_*")
	defer os.RemoveAll(baseDir)
	modDir, _ := os.MkdirTemp("", "test_mod_*")
	defer os.RemoveAll(modDir)
	outDir, _ := os.MkdirTemp("", "test_out_*")
	defer os.RemoveAll(outDir)

	// Base ROM
	_ = os.MkdirAll(filepath.Join(baseDir, "code"), 0755)
	_ = os.MkdirAll(filepath.Join(baseDir, "content"), 0755)
	_ = os.MkdirAll(filepath.Join(baseDir, "meta"), 0755)
	_ = os.WriteFile(filepath.Join(baseDir, "code", "Gambit.rpx"), []byte("BASE_RPX"), 0644)
	_ = os.WriteFile(filepath.Join(baseDir, "content", "asset.txt"), []byte("base_asset"), 0644)

	// Mod with new asset and unauthorized RPX
	_ = os.MkdirAll(filepath.Join(modDir, "code"), 0755)
	_ = os.MkdirAll(filepath.Join(modDir, "content"), 0755)
	_ = os.MkdirAll(filepath.Join(modDir, "meta"), 0755)
	_ = os.WriteFile(filepath.Join(modDir, "code", "Gambit.rpx"), []byte("MOD_RPX_SHOULD_BE_IGNORED"), 0644)
	_ = os.WriteFile(filepath.Join(modDir, "content", "mod_asset.txt"), []byte("mod_asset"), 0644)

	modLayer, err := NewLayerFromPath(modDir)
	if err != nil {
		t.Fatalf("Failed to create layer: %v", err)
	}
	if modLayer.CanReplaceRPX {
		t.Errorf("Mod should not be allowed to replace RPX")
	}

	err = CombineROMs(baseDir, []RomLayer{modLayer}, outDir, nil)
	if err != nil {
		t.Fatalf("CombineROMs failed: %v", err)
	}

	// Verify that base RPX is kept
	rpxData, _ := os.ReadFile(filepath.Join(outDir, "code", "Gambit.rpx"))
	if string(rpxData) != "BASE_RPX" {
		t.Errorf("Expected base RPX to be preserved, got %q", string(rpxData))
	}

	// Verify that mod asset was merged
	modAsset, _ := os.ReadFile(filepath.Join(outDir, "content", "mod_asset.txt"))
	if string(modAsset) != "mod_asset" {
		t.Errorf("Expected mod asset to be merged, got %q", string(modAsset))
	}
}

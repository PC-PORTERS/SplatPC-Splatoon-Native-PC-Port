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

// dumpWithLink writes a dump of the given title version and returns the path to hand to the
// builder. The link kind decides what is a symbolic link: the dump root, its code folder, or
// code/Gambit.rpx (a relative link, as a dump kept in a shared store has).
func dumpWithLink(t *testing.T, kind, version, rpx string) string {
	t.Helper()
	parent := t.TempDir()
	dump := filepath.Join(parent, "real")
	codeDir := filepath.Join(dump, "code")
	if kind == "code" {
		codeDir = filepath.Join(parent, "realcode")
	}
	for _, dir := range []string{codeDir, filepath.Join(dump, "content"), filepath.Join(dump, "meta")} {
		if err := os.MkdirAll(dir, 0755); err != nil {
			t.Fatal(err)
		}
	}
	meta := `<menu><title_version>` + version + `</title_version><product_code>WUP-P-AGMP</product_code></menu>`
	writeTestFile(t, filepath.Join(dump, "meta", "meta.xml"), meta)
	writeTestFile(t, filepath.Join(dump, "content", "asset_"+version+".txt"), "asset")

	skipWithoutSymlinks := func(err error) {
		if err != nil {
			t.Skipf("symbolic links are not available here: %v", err)
		}
	}
	switch kind {
	case "none":
		if rpx != "" {
			writeTestFile(t, filepath.Join(codeDir, "Gambit.rpx"), rpx)
		}
		return dump
	case "root":
		if rpx != "" {
			writeTestFile(t, filepath.Join(codeDir, "Gambit.rpx"), rpx)
		}
		link := filepath.Join(parent, "dump")
		skipWithoutSymlinks(os.Symlink("real", link))
		return link
	case "code":
		if rpx != "" {
			writeTestFile(t, filepath.Join(codeDir, "Gambit.rpx"), rpx)
		}
		skipWithoutSymlinks(os.Symlink(filepath.Join("..", "realcode"), filepath.Join(dump, "code")))
		return dump
	case "rpx":
		if rpx != "" {
			writeTestFile(t, filepath.Join(dump, "store.bin"), rpx)
			skipWithoutSymlinks(os.Symlink(filepath.Join("..", "store.bin"), filepath.Join(codeDir, "Gambit.rpx")))
		}
		return dump
	}
	t.Fatalf("unknown link kind %q", kind)
	return ""
}

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
}

// A dump reached through symbolic links must be combined by content: the builder checks the
// staging folder for code/Gambit.rpx, and a link recreated there pointed at nothing.
func TestCombineROMsFollowsSymlinks(t *testing.T) {
	for _, kind := range []string{"root", "code", "rpx"} {
		t.Run("base_"+kind, func(t *testing.T) {
			base := dumpWithLink(t, kind, "16", "BASE_RPX")
			update := dumpWithLink(t, "none", "288", "")
			combineAndCheck(t, base, update, "BASE_RPX")
		})
		t.Run("update_"+kind, func(t *testing.T) {
			base := dumpWithLink(t, "none", "16", "BASE_RPX")
			update := dumpWithLink(t, kind, "288", "V288_RPX")
			combineAndCheck(t, base, update, "V288_RPX")
		})
	}
}

func combineAndCheck(t *testing.T, base, update, wantRPX string) {
	t.Helper()
	layer, err := NewLayerFromPath(update)
	if err != nil {
		t.Fatalf("NewLayerFromPath: %v", err)
	}
	if !layer.IsV288 {
		t.Fatalf("the update dump should be recognised as v288")
	}
	staging := filepath.Join(t.TempDir(), "staging-rom")
	if err := CombineROMs(base, []RomLayer{layer}, staging, nil); err != nil {
		t.Fatalf("CombineROMs: %v", err)
	}
	rpx := filepath.Join(staging, "code", "Gambit.rpx")
	if info, err := os.Lstat(rpx); err != nil || !info.Mode().IsRegular() {
		t.Fatalf("code/Gambit.rpx must be a regular file in the staging folder, got %v (%v)", info, err)
	}
	data, err := os.ReadFile(rpx)
	if err != nil || string(data) != wantRPX {
		t.Fatalf("code/Gambit.rpx = %q (%v), want %q", data, err, wantRPX)
	}
	for _, name := range []string{"asset_16.txt", "asset_288.txt"} {
		if _, err := os.Stat(filepath.Join(staging, "content", name)); err != nil {
			t.Errorf("content/%s is missing from the staging folder: %v", name, err)
		}
	}
}

// A link that points back up the tree must not make the copy run forever.
func TestCombineROMsStopsOnSymlinkLoop(t *testing.T) {
	base := dumpWithLink(t, "none", "16", "BASE_RPX")
	if err := os.Symlink("..", filepath.Join(base, "content", "loop")); err != nil {
		t.Skipf("symbolic links are not available here: %v", err)
	}
	staging := filepath.Join(t.TempDir(), "staging-rom")
	if err := CombineROMs(base, nil, staging, nil); err != nil {
		t.Fatalf("CombineROMs: %v", err)
	}
	data, err := os.ReadFile(filepath.Join(staging, "code", "Gambit.rpx"))
	if err != nil || string(data) != "BASE_RPX" {
		t.Fatalf("code/Gambit.rpx = %q (%v)", data, err)
	}
}

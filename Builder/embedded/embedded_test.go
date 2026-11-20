package embedded

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestHasEmbeddedAssets(t *testing.T) {
	if !HasEmbeddedAssets() {
		t.Skip("assets.zip not generated yet (run scripts/bundle_builder_assets.py)")
	}
	assert.True(t, HasEmbeddedAssets(), "Builder must have embedded assets bundled")
}

func TestExtractEmbeddedAssets(t *testing.T) {
	if !HasEmbeddedAssets() {
		t.Skip("assets.zip not generated yet (run scripts/bundle_builder_assets.py)")
	}
	tmpDir, err := os.MkdirTemp("", "splatpc_test_extract_*")
	require.NoError(t, err)
	defer os.RemoveAll(tmpDir)

	err = ExtractEmbeddedAssets(tmpDir)
	require.NoError(t, err)

	assert.True(t, IsRepoRoot(tmpDir), "Extracted directory must be recognized as valid repo root")
	assert.FileExists(t, filepath.Join(tmpDir, "Manifest", "Gambit_v16.manifest"))
	assert.FileExists(t, filepath.Join(tmpDir, "recompiler", "CMakeLists.txt"))
	assert.FileExists(t, filepath.Join(tmpDir, "scripts", "hd_textures.py"))
	assert.FileExists(t, filepath.Join(tmpDir, ".assets_hash"))

	// Second extraction should succeed without re-writing everything
	err = ExtractEmbeddedAssets(tmpDir)
	require.NoError(t, err)
}

func TestResolveRepoRoot(t *testing.T) {
	root, isStandalone, err := ResolveRepoRoot()
	require.NoError(t, err)
	assert.NotEmpty(t, root)
	assert.True(t, IsRepoRoot(root))
	// In the test runner inside the workspace, it should detect the local repo
	assert.False(t, isStandalone)
}

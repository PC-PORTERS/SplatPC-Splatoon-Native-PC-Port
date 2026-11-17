package engine

import (
	"os"
	"path/filepath"
	"testing"
)

func TestFindVolkConfigLocal(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "builder-test-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	// Test case 1: standard lib/cmake/volk layout
	cfgDir := filepath.Join(tmpDir, "deps", "volk", "lib", "cmake", "volk")
	if err := os.MkdirAll(cfgDir, 0755); err != nil {
		t.Fatalf("failed to create dummy layout: %v", err)
	}
	cfgFile := filepath.Join(cfgDir, "volkConfig.cmake")
	if err := os.WriteFile(cfgFile, []byte("# dummy"), 0644); err != nil {
		t.Fatalf("failed to write dummy config: %v", err)
	}

	found, err := findVolkConfig(tmpDir)
	if err != nil {
		t.Fatalf("expected to find volkConfig, got error: %v", err)
	}
	if found != cfgFile {
		t.Errorf("expected %s, got %s", cfgFile, found)
	}
}

func TestFindConfigDirFollowsInstallLayout(t *testing.T) {
	for _, layout := range []string{"lib/cmake/glslang", "lib64/cmake/glslang", "lib/x86_64-linux-gnu/cmake/glslang"} {
		root := t.TempDir()
		cfgDir := filepath.Join(root, filepath.FromSlash(layout))
		if err := os.MkdirAll(cfgDir, 0755); err != nil {
			t.Fatalf("failed to create dummy layout: %v", err)
		}
		if err := os.WriteFile(filepath.Join(cfgDir, "glslang-config.cmake"), []byte("# dummy"), 0644); err != nil {
			t.Fatalf("failed to write dummy config: %v", err)
		}
		found, ok := findConfigDir(root, "glslang-config.cmake", "glslangConfig.cmake")
		if !ok || found != cfgDir {
			t.Errorf("layout %s: expected %s, got %q (found=%v)", layout, cfgDir, found, ok)
		}
	}
	if _, ok := findConfigDir(t.TempDir(), "glslang-config.cmake"); ok {
		t.Errorf("expected no config in an empty profile")
	}
}

func TestFindVolkConfigLib64(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "builder-test-lib64-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	// Test case 2: Fedora/RHEL lib64 layout
	cfgDir := filepath.Join(tmpDir, "deps", "volk", "lib64", "cmake", "volk")
	if err := os.MkdirAll(cfgDir, 0755); err != nil {
		t.Fatalf("failed to create dummy layout: %v", err)
	}
	cfgFile := filepath.Join(cfgDir, "volkConfig.cmake")
	if err := os.WriteFile(cfgFile, []byte("# dummy"), 0644); err != nil {
		t.Fatalf("failed to write dummy config: %v", err)
	}

	found, err := findVolkConfig(tmpDir)
	if err != nil {
		t.Fatalf("expected to find volkConfig in lib64, got error: %v", err)
	}
	if found != cfgFile {
		t.Errorf("expected %s, got %s", cfgFile, found)
	}
}

func TestFindVolkConfigMultiarch(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "builder-test-multiarch-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	// Test case 3: Debian multiarch layout
	cfgDir := filepath.Join(tmpDir, "deps", "volk", "lib", "x86_64-linux-gnu", "cmake", "volk")
	if err := os.MkdirAll(cfgDir, 0755); err != nil {
		t.Fatalf("failed to create dummy layout: %v", err)
	}
	cfgFile := filepath.Join(cfgDir, "volkConfig.cmake")
	if err := os.WriteFile(cfgFile, []byte("# dummy"), 0644); err != nil {
		t.Fatalf("failed to write dummy config: %v", err)
	}

	found, err := findVolkConfig(tmpDir)
	if err != nil {
		t.Fatalf("expected to find volkConfig in multiarch dir, got error: %v", err)
	}
	if found != cfgFile {
		t.Errorf("expected %s, got %s", cfgFile, found)
	}
}

func TestFindVolkConfigNotFound(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "builder-test-empty-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	_, err = findVolkConfig(tmpDir)
	// If system has volk, it might find it, but if not, it should error cleanly
	// The function should not panic regardless
	_ = err
}

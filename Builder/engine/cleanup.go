package engine

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"splatpc-builder/prereq"
)

// CleanupStats summarizes the disk space used by userspace tools, downloaded
// dependencies, and intermediate build artifacts.
type CleanupStats struct {
	ToolsBytes          int64
	DepsBytes           int64
	BuildArtifactsBytes int64
}

func (s CleanupStats) TotalDepsBytes() int64 {
	return s.ToolsBytes + s.DepsBytes
}

func (s CleanupStats) TotalAllBytes() int64 {
	return s.ToolsBytes + s.DepsBytes + s.BuildArtifactsBytes
}

func dependencyPaths(repoRoot string) []string {
	buildDir := filepath.Join(repoRoot, "build")
	return []string{
		filepath.Join(buildDir, "downloads"),
		filepath.Join(buildDir, "deps"),
		filepath.Join(buildDir, "deps-build"),
		filepath.Join(buildDir, "cache"),
		filepath.Join(buildDir, "imgui-source"),
		filepath.Join(buildDir, "volk-source"),
		filepath.Join(buildDir, "volk-build"),
		filepath.Join(buildDir, "vulkan-headers-source"),
		filepath.Join(buildDir, "vulkan-headers-build"),
		filepath.Join(buildDir, "zlib-source"),
		filepath.Join(buildDir, "zlib-build"),
		filepath.Join(buildDir, "curl-source"),
		filepath.Join(buildDir, "curl-build"),
		filepath.Join(repoRoot, "recompiler", "third_party", "nlohmann"),
		filepath.Join(repoRoot, "recompiler", "third_party", "miniaudio.h"),
		filepath.Join(repoRoot, "recompiler", "third_party", "qrcodegen"),
	}
}

func buildArtifactPaths(repoRoot string) []string {
	buildDir := filepath.Join(repoRoot, "build")
	return []string{
		filepath.Join(buildDir, "rebrewu"),
		filepath.Join(buildDir, "game"),
		filepath.Join(buildDir, "port"),
		filepath.Join(buildDir, "generated"),
		filepath.Join(buildDir, "staging-rom"),
	}
}

func pathSize(p string) int64 {
	info, err := os.Stat(p)
	if err != nil {
		return 0
	}
	if !info.IsDir() {
		return info.Size()
	}
	return prereq.DirSize(p)
}

func pathProtectsOutput(candidate, outputDir string) bool {
	if outputDir == "" {
		return false
	}
	absCand, err1 := filepath.Abs(candidate)
	absOut, err2 := filepath.Abs(outputDir)
	if err1 != nil || err2 != nil {
		return false
	}
	if absCand == absOut {
		return true
	}
	rel, err := filepath.Rel(absCand, absOut)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

// MeasureCleanup computes how much disk space is occupied by userspace tools,
// downloaded/built dependencies, and intermediate build files.
func MeasureCleanup(repoRoot, outputDir string) CleanupStats {
	var stats CleanupStats
	if !pathProtectsOutput(prereq.UserToolsDir(), outputDir) {
		stats.ToolsBytes = prereq.UserspaceToolsSize()
	}
	for _, p := range dependencyPaths(repoRoot) {
		if !pathProtectsOutput(p, outputDir) {
			stats.DepsBytes += pathSize(p)
		}
	}
	for _, p := range buildArtifactPaths(repoRoot) {
		if !pathProtectsOutput(p, outputDir) {
			stats.BuildArtifactsBytes += pathSize(p)
		}
	}
	return stats
}

// CleanupAfterBuild deletes downloaded userspace tools and build dependencies
// (and optionally intermediate C++/recompiler build directories) while keeping
// the built game in outputDir and build/build.log intact.
func CleanupAfterBuild(repoRoot, outputDir string, includeBuildArtifacts bool, log func(string, ...interface{})) (int64, error) {
	if log == nil {
		log = func(string, ...interface{}) {}
	}
	var freed int64
	var firstErr error
	recordErr := func(err error) {
		if err != nil && firstErr == nil {
			firstErr = err
		}
	}

	toolsDir := prereq.UserToolsDir()
	if !pathProtectsOutput(toolsDir, outputDir) {
		if sz := pathSize(toolsDir); sz > 0 {
			log("Removing userspace tools (%s)...", toolsDir)
			if err := prereq.RemoveUserspaceTools(); err != nil {
				recordErr(err)
			} else {
				freed += sz
			}
		}
	}

	removePaths := func(paths []string) {
		for _, p := range paths {
			if pathProtectsOutput(p, outputDir) {
				continue
			}
			sz := pathSize(p)
			if sz == 0 {
				if _, err := os.Stat(p); os.IsNotExist(err) {
					continue
				}
			}
			if err := os.RemoveAll(p); err != nil {
				recordErr(err)
			} else {
				freed += sz
			}
		}
	}

	log("Removing downloaded dependency archives and build prefixes...")
	removePaths(dependencyPaths(repoRoot))

	if includeBuildArtifacts {
		log("Removing intermediate build directories...")
		removePaths(buildArtifactPaths(repoRoot))
	}

	log("Cleanup complete: freed %s.", FormatBytes(freed))
	return freed, firstErr
}

// FormatBytes renders a byte count in human-readable form.
func FormatBytes(bytes int64) string {
	const (
		kb = 1024
		mb = 1024 * kb
		gb = 1024 * mb
	)
	switch {
	case bytes >= gb:
		return fmt.Sprintf("%.2f GB", float64(bytes)/float64(gb))
	case bytes >= mb:
		return fmt.Sprintf("%.1f MB", float64(bytes)/float64(mb))
	case bytes >= kb:
		return fmt.Sprintf("%.0f KB", float64(bytes)/float64(kb))
	default:
		return fmt.Sprintf("%d B", bytes)
	}
}

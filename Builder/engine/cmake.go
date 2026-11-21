package engine

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"splatpc-builder/prereq"
)

// buildConfig is the one configuration every CMake project of the build uses.
// Multi-config generators (Visual Studio) ignore CMAKE_BUILD_TYPE and fall back
// to Debug, so it is also passed to every build with --config: mixing a Debug
// game with Release libraries fails to link (LNK2038 _ITERATOR_DEBUG_LEVEL).
const buildConfig = "Release"

type cmakeProject struct {
	exe       string
	generator string
	jobs      int
}

func newCMakeProject(jobs int) cmakeProject {
	exe := "cmake"
	if p, ok := prereq.LookPath("cmake"); ok {
		exe = p
	}
	return cmakeProject{exe: exe, generator: cmakeGenerator(), jobs: jobs}
}

// cmakeGenerator picks the Visual Studio generator on Windows when Visual
// Studio is installed, otherwise Ninja when using a userspace MinGW toolchain.
func cmakeGenerator() string {
	if runtime.GOOS == "windows" {
		if !prereq.UserspaceOnly() {
			if vs, ok := prereq.FindVisualStudio(); ok {
				return vs.CMakeGenerator()
			}
		}
		if _, ok := prereq.LookPath("ninja"); ok {
			return "Ninja"
		}
		return prereq.DefaultVisualStudioGenerator
	}
	if _, ok := prereq.LookPath("ninja"); ok {
		return "Ninja"
	}
	return "Unix Makefiles"
}

func (c cmakeProject) isWindowsMinGW() bool {
	return runtime.GOOS == "windows" && c.generator == "Ninja"
}

func (c cmakeProject) configureArgs(source, build string, defines ...string) []string {
	args := []string{"-S", source, "-B", build, "-G", c.generator, "-DCMAKE_BUILD_TYPE=" + buildConfig}
	return append(args, defines...)
}

func (c cmakeProject) buildArgs(build, target string) []string {
	args := []string{"--build", build, "--config", buildConfig}
	if target != "" {
		args = append(args, "--target", target)
	}
	return append(args, "--parallel", strconv.Itoa(c.jobs))
}

func exeName(name string) string {
	if runtime.GOOS == "windows" {
		return name + ".exe"
	}
	return name
}

// builtExecutable finds an executable CMake wrote to dir, or to
// dir/<config> with a multi-config generator.
func builtExecutable(dir, name string) (string, error) {
	candidates := []string{
		filepath.Join(dir, buildConfig, exeName(name)),
		filepath.Join(dir, exeName(name)),
	}
	for _, candidate := range candidates {
		if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
			return candidate, nil
		}
	}
	return "", fmt.Errorf("%s was not built (looked for %s)", exeName(name), strings.Join(candidates, " and "))
}

package engine

import (
	"context"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"time"

	"splatpc-builder/prereq"
	"splatpc-builder/proc"
	"splatpc-builder/roms"
	"splatpc-builder/settings"
)

type StepInfo struct {
	Index       int
	Total       int
	Title       string
	Description string
	Progress    float64 // 0.0 to 1.0
}

type BuildCallbacks struct {
	OnProgress func(step StepInfo)
	OnLog      func(line string)
	OnDone     func(success bool, err error, elapsed time.Duration, appImagePath string)
}

func RunBuild(ctx context.Context, repoRoot string, cfg settings.Configuration, overlays []roms.RomLayer, cb BuildCallbacks) {
	startTime := time.Now()
	totalSteps := 8
	if cfg.CreateAppImage && runtime.GOOS == "linux" {
		totalSteps = 9
	}

	buildDir := filepath.Join(repoRoot, "build")
	downloadsDir := filepath.Join(buildDir, "downloads")
	depsDir := filepath.Join(buildDir, "deps")
	manifestDir := filepath.Join(repoRoot, "Manifest")
	recompilerDir := filepath.Join(repoRoot, "recompiler")

	var cleanErr error
	if cfg.CleanBuild {
		cleanErr = cleanBuildDir(buildDir)
	}

	logPath := filepath.Join(buildDir, "build.log")
	buildLog := openBuildLog(logPath, cb.OnLog)
	defer buildLog.close()
	log := buildLog.printf

	if cfg.CleanBuild {
		log("Removed the previous build (--clean); downloaded archives were kept.")
		if cleanErr != nil {
			log("Warning: the previous build could not be fully removed: %v", cleanErr)
		}
	}

	progress := func(stepIdx int, title, desc string, p float64) {
		if cb.OnProgress != nil {
			cb.OnProgress(StepInfo{
				Index:       stepIdx,
				Total:       totalSteps,
				Title:       title,
				Description: desc,
				Progress:    p,
			})
		}
	}

	failDone := func(err error) {
		log("ERROR: %v", err)
		if cb.OnDone != nil {
			cb.OnDone(false, fmt.Errorf("%w\n\nFull log: %s", err, logPath), time.Since(startTime), "")
		}
	}

	_ = os.MkdirAll(downloadsDir, 0755)
	_ = os.MkdirAll(depsDir, 0755)

	cmake := newCMakeProject(cfg.ParallelJobs)
	log("CMake generator: %s, configuration: %s", cmake.generator, buildConfig)
	deps := dependencyBuilder{
		ctx:        ctx,
		cmake:      cmake,
		buildDir:   buildDir,
		downloads:  downloadsDir,
		prefixRoot: depsDir,
		log:        log,
	}
	python, pythonVersion, ok := prereq.FindPython()
	if !ok {
		failDone(fmt.Errorf("Python 3 was not found (python3, python and py -3 were tried)"))
		return
	}
	log("Python: %s (%s)", strings.Join(python, " "), pythonVersion)

	// STEP 1: Process and combine ROMs
	progress(1, "Preparing ROMs", "Validating and merging game dump files", 0.05)
	log("=== Step 1/8: ROM Validation ===")
	gameSourceDump := cfg.BaseDumpPath
	if len(overlays) > 0 {
		stagingROM := filepath.Join(buildDir, "staging-rom")
		log("Overlays/mods detected. Merging into %s...", stagingROM)
		_ = os.RemoveAll(stagingROM)
		if err := roms.CombineROMs(cfg.BaseDumpPath, overlays, stagingROM, log); err != nil {
			failDone(fmt.Errorf("merging ROMs: %w", err))
			return
		}
		gameSourceDump = stagingROM
	}

	rpxPath := filepath.Join(gameSourceDump, "code", "Gambit.rpx")
	metaPath := filepath.Join(gameSourceDump, "meta", "meta.xml")
	if _, err := os.Stat(rpxPath); err != nil {
		failDone(fmt.Errorf("missing RPX file: %s", rpxPath))
		return
	}
	if _, err := os.Stat(metaPath); err != nil {
		failDone(fmt.Errorf("missing meta.xml file: %s", metaPath))
		return
	}

	// Select manifest using plt_resolver.py
	log("Selecting manifest for Gambit.rpx...")
	cmdSelect := python.Command(ctx, filepath.Join(recompilerDir, "tools", "plt_resolver.py"),
		"select", "--rpx", rpxPath, "--manifest-dir", manifestDir)
	selectOut, err := cmdSelect.Output()
	if err != nil {
		failDone(fmt.Errorf("unsupported game version or manifest selection failed: %w", err))
		return
	}
	selectedManifest := strings.TrimSpace(string(selectOut))
	log("Selected manifest: %s", filepath.Base(selectedManifest))

	// Read app_version from manifest
	gameRevision := "16"
	if data, err := os.ReadFile(selectedManifest); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "app_version") {
				parts := strings.Split(line, "=")
				if len(parts) >= 2 {
					val := strings.TrimSpace(parts[1])
					val = strings.Split(val, "#")[0]
					gameRevision = strings.TrimSpace(val)
					break
				}
			}
		}
	}
	log("Detected game version: %s", gameRevision)

	// STEP 2: Wii U Folders & Resources
	progress(2, "Wii U Resources & Fonts", "Initializing Wii U folders and Cemu replacement fonts", 0.15)
	log("=== Step 2/8: Initializing Wii U Resources ===")
	if err := PrepareWiiUResources(cfg.OutputDir, repoRoot, downloadsDir, log); err != nil {
		failDone(fmt.Errorf("preparing resources: %w", err))
		return
	}

	// STEP 3: SDL2 & glslang dependencies
	progress(3, "Graphics Libraries", "Downloading and building SDL2 and glslang", 0.25)
	log("=== Step 3/8: SDL2 & glslang Dependencies ===")
	sdlZip := filepath.Join(downloadsDir, "SDL2-2.32.10.zip")
	glslangTar := filepath.Join(downloadsDir, "glslang-16.4.0.tar.gz")

	if _, err := os.Stat(sdlZip); err != nil {
		log("Downloading SDL2...")
		if err := downloadFile("https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip", sdlZip); err != nil {
			failDone(err)
			return
		}
	}
	if _, err := os.Stat(glslangTar); err != nil {
		log("Downloading glslang...")
		if err := downloadFile("https://github.com/KhronosGroup/glslang/archive/refs/tags/16.4.0.tar.gz", glslangTar); err != nil {
			failDone(err)
			return
		}
	}

	sdlSource := filepath.Join(downloadsDir, "SDL2-2.32.10")
	if _, err := os.Stat(sdlSource); err != nil {
		log("Extracting SDL2...")
		cmdExtract := python.Command(ctx, "-c", "import zipfile, sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])", sdlZip, downloadsDir)
		if err := runCmdWithLog(cmdExtract, log); err != nil {
			failDone(fmt.Errorf("extracting SDL2: %w", err))
			return
		}
	}

	cmakePath, _ := exec.LookPath("cmake")
	cmdBuildDeps := python.Command(ctx, filepath.Join(recompilerDir, "tools", "build_vulkan_dependencies.py"),
		"--sdl-source", sdlSource,
		"--glslang-archive", glslangTar,
		"--build-root", filepath.Join(buildDir, "deps-build"),
		"--prefix", depsDir,
		"--generator", cmake.generator,
		"--cmake", cmakePath,
		"--cache-dir", filepath.Join(buildDir, "cache", "deps"),
	)
	if err := runCmdWithLog(cmdBuildDeps, log); err != nil {
		failDone(fmt.Errorf("building Vulkan dependencies: %w", err))
		return
	}

	// STEP 4: Native libraries. Windows has no system Vulkan-Headers, zlib or
	// curl, so the Builder provides them there.
	progress(4, "Native Libraries", "Setting up volk and the libraries the game links", 0.35)
	log("=== Step 4/8: Native Libraries ===")
	var libs bundledLibraries
	if runtime.GOOS == "windows" {
		if libs, err = installVulkanHeaders(deps); err != nil {
			failDone(err)
			return
		}
	}
	volkDir, volkPrefix, err := locateVolk(deps, volkDependency(libs.vulkanHeaders))
	if err != nil {
		failDone(err)
		return
	}
	log("Volk ready: %s (prefix: %s)", volkDir, volkPrefix)
	if runtime.GOOS == "windows" {
		if err := installZlibAndCurl(deps, &libs); err != nil {
			failDone(err)
			return
		}
	}

	// STEP 5: Build RebrewU recompiler
	progress(5, "RebrewU Recompiler", "Building the RebrewU recompiler executable", 0.45)
	log("=== Step 5/8: RebrewU Recompiler ===")
	rebrewuBuild := filepath.Join(buildDir, "rebrewu")
	rebrewuDefines := append([]string{"-DREBREWU_BUILD_TESTS=OFF"}, libs.zlibDefines...)
	cmdRebrewuCfg := proc.CommandContext(ctx, "cmake", cmake.configureArgs(recompilerDir, rebrewuBuild, rebrewuDefines...)...)
	if err := runCmdWithLog(cmdRebrewuCfg, log); err != nil {
		failDone(fmt.Errorf("CMake configuration of RebrewU failed: %w", err))
		return
	}
	cmdRebrewuBuild := proc.CommandContext(ctx, "cmake", cmake.buildArgs(rebrewuBuild, "rebrewu")...)
	if err := runCmdWithLog(cmdRebrewuBuild, log); err != nil {
		failDone(fmt.Errorf("compiling RebrewU: %w", err))
		return
	}

	rebrewuExe, err := builtExecutable(filepath.Join(rebrewuBuild, "bin"), "rebrewu")
	if err != nil {
		failDone(err)
		return
	}

	// STEP 6: Recompile RPX to C++
	progress(6, "Decompiling Gambit.rpx", "Translating PowerPC code into native C++", 0.55)
	log("=== Step 6/8: RPX to C++ Decompilation ===")
	generatedDir := filepath.Join(buildDir, "generated", "Gambit")
	_ = os.MkdirAll(generatedDir, 0755)

	cmdRecompile := proc.CommandContext(ctx, rebrewuExe, "recompile",
		"--rpx", rpxPath,
		"--output", generatedDir,
		"--manifest", selectedManifest,
	)
	if err := runCmdWithLog(cmdRecompile, log); err != nil {
		failDone(fmt.Errorf("RPX recompilation failed: %w", err))
		return
	}
	log("C++ code successfully generated in %s", generatedDir)

	// STEP 7: Build the game
	progress(7, "Compiling Splatoon", fmt.Sprintf("C++ compilation with %d parallel jobs", cfg.ParallelJobs), 0.70)
	log("=== Step 7/8: Compiling Native Game (%d jobs) ===", cfg.ParallelJobs)
	gameBuild := filepath.Join(buildDir, "game")
	sdl2Dir, ok := findConfigDir(filepath.Join(depsDir, "sdl2-vulkan"), "SDL2Config.cmake")
	if !ok {
		sdl2Dir = filepath.Join(depsDir, "sdl2-vulkan", "lib", "cmake", "SDL2")
	}
	glslangDir, ok := findConfigDir(filepath.Join(depsDir, "glslang-vulkan"), "glslang-config.cmake", "glslangConfig.cmake")
	if !ok {
		glslangDir = filepath.Join(depsDir, "glslang-vulkan", "lib", "cmake", "glslang")
	}

	prefixPath := strings.Join(append([]string{volkPrefix}, libs.prefixes...), ";")
	gameDefines := append([]string{
		"-DRBREW_GRAPHICS_BACKEND=Vulkan",
		fmt.Sprintf("-DRBREW_GAME_REVISION=%s", gameRevision),
		fmt.Sprintf("-DRBREW_RPX=%s", rpxPath),
		fmt.Sprintf("-DGAMBIT_GENERATED_DIR=%s", generatedDir),
		fmt.Sprintf("-DSDL2_DIR=%s", sdl2Dir),
		fmt.Sprintf("-Dglslang_DIR=%s", glslangDir),
		fmt.Sprintf("-Dvolk_DIR=%s", volkDir),
		fmt.Sprintf("-DCMAKE_PREFIX_PATH=%s", prefixPath),
		"-DRBREW_BUILD_GAME=ON",
		"-DRBREW_ENABLE_COVERAGE=OFF",
		"-DRBREW_ENABLE_PPC_FALLBACK=OFF",
		"-DRBREW_DIAGNOSTIC_RUNTIME=OFF",
		"-DRBREW_TEXTURE_DIAGNOSTICS=OFF",
	}, libs.gameDefines...)
	cmdGameCfg := proc.CommandContext(ctx, "cmake", cmake.configureArgs(filepath.Join(recompilerDir, "port"), gameBuild, gameDefines...)...)
	if err := runCmdWithLog(cmdGameCfg, log); err != nil {
		failDone(fmt.Errorf("CMake configuration of the game failed: %w", err))
		return
	}

	cmdGameBuild := proc.CommandContext(ctx, "cmake", cmake.buildArgs(gameBuild, "Gambit")...)
	if err := runCmdWithLog(cmdGameBuild, log); err != nil {
		failDone(fmt.Errorf("compiling Splatoon executable: %w", err))
		return
	}

	// STEP 8: Packaging game folder
	progress(8, "Packaging Game Directory", "Copying executable, assets, and custom settings", 0.90)
	log("=== Step 8/8: Game Packaging ===")
	_ = os.MkdirAll(cfg.OutputDir, 0755)

	exeSource, err := builtExecutable(gameBuild, "Splatoon")
	if err != nil {
		failDone(err)
		return
	}
	exeDest := GameExecutable(cfg.OutputDir)
	if err := copySingleFile(exeSource, exeDest); err != nil {
		failDone(fmt.Errorf("copying executable: %w", err))
		return
	}
	_ = os.Chmod(exeDest, 0755)

	// Copy content and meta
	log("Copying game content (content/ and meta/)...")
	_ = copyDirectoryTreeSimple(filepath.Join(gameSourceDump, "content"), filepath.Join(cfg.OutputDir, "content"))
	_ = copyDirectoryTreeSimple(filepath.Join(gameSourceDump, "meta"), filepath.Join(cfg.OutputDir, "meta"))

	if cfg.UpscaleTextures {
		log("HD Texturing enabled: preparing textures/ and texture-dump/ folders...")
		_ = os.MkdirAll(filepath.Join(cfg.OutputDir, "textures"), 0755)
		_ = os.MkdirAll(filepath.Join(cfg.OutputDir, "texture-dump"), 0755)
	}

	// Write settings.ini and launcher script
	log("Writing custom settings (player nickname: %s)...", cfg.PlayerName)
	if cfg.EnableOnline && strings.TrimSpace(cfg.OnlineServer) != "" {
		log("Online multiplayer configured: server %s (pseudo: '%s')", strings.TrimSpace(cfg.OnlineServer), cfg.PlayerName)
	}
	if err := settings.WriteSettingsIni(cfg, cfg.OutputDir); err != nil {
		log("Warning: Failed to write settings.ini: %v", err)
	}
	if err := settings.WriteLauncherScript(cfg, cfg.OutputDir); err != nil {
		log("Warning: Failed to write launch script: %v", err)
	}

	var appImagePath string
	// Optional STEP 9: AppImage for Linux
	if cfg.CreateAppImage && runtime.GOOS == "linux" {
		progress(9, "Packaging AppImage", "Creating portable standalone Linux AppImage", 0.96)
		log("=== Optional Step: AppImage Generation ===")
		var appErr error
		appImagePath, appErr = CreateAppImage(cfg.OutputDir, repoRoot, log)
		if appErr != nil {
			log("Warning: AppImage generation failed: %v", appErr)
		}
	}

	elapsed := time.Since(startTime)
	progress(totalSteps, "Build Complete!", fmt.Sprintf("Game ready in %.1f seconds", elapsed.Seconds()), 1.0)
	log("=== Success! The game is ready in %s ===", cfg.OutputDir)
	cb.OnDone(true, nil, elapsed, appImagePath)
}

// findConfigDir returns the directory under root holding one of the CMake
// package config files, whatever the lib, lib64 or multiarch install layout.
func findConfigDir(root string, configNames ...string) (string, bool) {
	path, ok := findFile(root, configNames...)
	if !ok {
		return "", false
	}
	return filepath.Dir(path), true
}

// findFile returns the first file under root named one of names.
func findFile(root string, names ...string) (string, bool) {
	var found string
	_ = filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return nil
		}
		if slices.Contains(names, entry.Name()) {
			found = path
			return filepath.SkipAll
		}
		return nil
	})
	return found, found != ""
}

func findVolkConfig(buildDir string) (string, error) {
	depsVolk := filepath.Join(buildDir, "deps", "volk")
	if _, err := os.Stat(depsVolk); err == nil {
		var found string
		_ = filepath.Walk(depsVolk, func(path string, info os.FileInfo, err error) error {
			if err == nil && !info.IsDir() && (info.Name() == "volkConfig.cmake" || info.Name() == "volk-config.cmake") {
				found = path
				return filepath.SkipAll
			}
			return nil
		})
		if found != "" {
			return found, nil
		}
	}

	candidates := []string{
		filepath.Join(buildDir, "deps", "volk", "lib", "cmake", "volk", "volkConfig.cmake"),
		filepath.Join(buildDir, "deps", "volk", "lib64", "cmake", "volk", "volkConfig.cmake"),
		filepath.Join(buildDir, "deps", "volk", "share", "cmake", "volk", "volkConfig.cmake"),
		"/usr/lib/cmake/volk/volkConfig.cmake",
		"/usr/lib64/cmake/volk/volkConfig.cmake",
		"/usr/share/cmake/volk/volkConfig.cmake",
		"/usr/local/lib/cmake/volk/volkConfig.cmake",
		"/usr/local/share/cmake/volk/volkConfig.cmake",
	}

	if matches, err := filepath.Glob("/usr/lib/*-linux-gnu/cmake/volk/volkConfig.cmake"); err == nil {
		candidates = append(candidates, matches...)
	}
	if matches, err := filepath.Glob("/usr/local/lib/*-linux-gnu/cmake/volk/volkConfig.cmake"); err == nil {
		candidates = append(candidates, matches...)
	}

	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c, nil
		}
	}
	return "", fmt.Errorf("volkConfig.cmake not found")
}

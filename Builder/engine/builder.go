package engine

import (
	"bufio"
	"context"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

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

	log := func(format string, args ...interface{}) {
		msg := fmt.Sprintf(format, args...)
		if cb.OnLog != nil {
			cb.OnLog(msg)
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
		if cb.OnDone != nil {
			cb.OnDone(false, err, time.Since(startTime), "")
		}
	}

	buildDir := filepath.Join(repoRoot, "build")
	downloadsDir := filepath.Join(buildDir, "downloads")
	depsDir := filepath.Join(buildDir, "deps")
	manifestDir := filepath.Join(repoRoot, "Manifest")
	recompilerDir := filepath.Join(repoRoot, "recompiler")

	if cfg.CleanBuild {
		log("Removing previous build folder (--clean)...")
		_ = os.RemoveAll(buildDir)
	}

	_ = os.MkdirAll(downloadsDir, 0755)
	_ = os.MkdirAll(depsDir, 0755)

	generator := "Unix Makefiles"
	if _, err := exec.LookPath("ninja"); err == nil {
		generator = "Ninja"
	}
	if runtime.GOOS == "windows" {
		generator = "Visual Studio 17 2022"
	}

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
	cmdSelect := exec.CommandContext(ctx, "python3", filepath.Join(recompilerDir, "tools", "plt_resolver.py"),
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
		cmdExtract := exec.CommandContext(ctx, "python3", "-c", "import zipfile, sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])", sdlZip, downloadsDir)
		if err := runCmdWithLog(cmdExtract, log); err != nil {
			failDone(fmt.Errorf("extracting SDL2: %w", err))
			return
		}
	}

	cmakePath, _ := exec.LookPath("cmake")
	cmdBuildDeps := exec.CommandContext(ctx, "python3", filepath.Join(recompilerDir, "tools", "build_vulkan_dependencies.py"),
		"--sdl-source", sdlSource,
		"--glslang-archive", glslangTar,
		"--build-root", filepath.Join(buildDir, "deps-build"),
		"--prefix", depsDir,
		"--generator", generator,
		"--cmake", cmakePath,
		"--cache-dir", filepath.Join(buildDir, "cache", "deps"),
	)
	if err := runCmdWithLog(cmdBuildDeps, log); err != nil {
		failDone(fmt.Errorf("building Vulkan dependencies: %w", err))
		return
	}

	// STEP 4: Volk meta-loader
	progress(4, "Vulkan Meta-Loader (volk)", "Setting up Volk meta-loader", 0.35)
	log("=== Step 4/8: Volk Meta-loader ===")
	volkConfig, err := findVolkConfig(buildDir)
	if err != nil {
		volkTar := filepath.Join(downloadsDir, "volk-1.4.363.tar.gz")
		if _, err := os.Stat(volkTar); err != nil {
			log("Downloading volk...")
			if err := downloadFile("https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz", volkTar); err != nil {
				failDone(err)
				return
			}
		}
		volkSource := filepath.Join(buildDir, "volk-source")
		if _, err := os.Stat(filepath.Join(volkSource, "CMakeLists.txt")); err != nil {
			_ = os.RemoveAll(volkSource)
			_ = os.MkdirAll(volkSource, 0755)
			cmdTar := exec.CommandContext(ctx, "tar", "-xzf", volkTar, "-C", volkSource, "--strip-components=1")
			if err := runCmdWithLog(cmdTar, log); err != nil {
				failDone(err)
				return
			}
		}
		volkBuild := filepath.Join(buildDir, "volk-build")
		cmdVolkCfg := exec.CommandContext(ctx, "cmake", "-S", volkSource, "-B", volkBuild, "-G", generator,
			"-DCMAKE_BUILD_TYPE=Release",
			"-DVOLK_INSTALL=ON",
			fmt.Sprintf("-DCMAKE_INSTALL_PREFIX=%s", filepath.Join(depsDir, "volk")),
		)
		if err := runCmdWithLog(cmdVolkCfg, log); err != nil {
			failDone(err)
			return
		}
		cmdVolkBuild := exec.CommandContext(ctx, "cmake", "--build", volkBuild, "--target", "install", "--parallel", strconv.Itoa(cfg.ParallelJobs))
		if err := runCmdWithLog(cmdVolkBuild, log); err != nil {
			failDone(err)
			return
		}
		volkConfig, err = findVolkConfig(buildDir)
		if err != nil {
			failDone(fmt.Errorf("volk installation succeeded but volkConfig.cmake was not found: %w", err))
			return
		}
	}
	volkDir := filepath.Dir(volkConfig)
	volkPrefix := filepath.Join(depsDir, "volk")
	if !strings.HasPrefix(volkConfig, filepath.Join(buildDir, "deps", "volk")) {
		p := filepath.Dir(volkDir)
		if filepath.Base(p) == "cmake" {
			p = filepath.Dir(p)
		}
		volkPrefix = filepath.Dir(p)
	}
	log("Volk ready: %s (dir: %s, prefix: %s)", volkConfig, volkDir, volkPrefix)

	// STEP 5: Build RebrewU recompiler
	progress(5, "RebrewU Recompiler", "Building the RebrewU recompiler executable", 0.45)
	log("=== Step 5/8: RebrewU Recompiler ===")
	rebrewuBuild := filepath.Join(buildDir, "rebrewu")
	cmdRebrewuCfg := exec.CommandContext(ctx, "cmake", "-S", recompilerDir, "-B", rebrewuBuild, "-G", generator,
		"-DCMAKE_BUILD_TYPE=Release",
		"-DREBREWU_BUILD_TESTS=OFF",
	)
	if err := runCmdWithLog(cmdRebrewuCfg, log); err != nil {
		failDone(err)
		return
	}
	cmdRebrewuBuild := exec.CommandContext(ctx, "cmake", "--build", rebrewuBuild, "--target", "rebrewu", "--parallel", strconv.Itoa(cfg.ParallelJobs))
	if err := runCmdWithLog(cmdRebrewuBuild, log); err != nil {
		failDone(err)
		return
	}

	rebrewuExe := filepath.Join(rebrewuBuild, "bin", "rebrewu")
	if runtime.GOOS == "windows" {
		rebrewuExe += ".exe"
	}
	if _, err := os.Stat(rebrewuExe); err != nil {
		failDone(fmt.Errorf("rebrewu executable not found: %s", rebrewuExe))
		return
	}

	// STEP 6: Recompile RPX to C++
	progress(6, "Decompiling Gambit.rpx", "Translating PowerPC code into native C++", 0.55)
	log("=== Step 6/8: RPX to C++ Decompilation ===")
	generatedDir := filepath.Join(buildDir, "generated", "Gambit")
	_ = os.MkdirAll(generatedDir, 0755)

	cmdRecompile := exec.CommandContext(ctx, rebrewuExe, "recompile",
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

	cmdGameCfg := exec.CommandContext(ctx, "cmake",
		"-S", filepath.Join(recompilerDir, "port"),
		"-B", gameBuild,
		"-G", generator,
		"-DCMAKE_BUILD_TYPE=Release",
		"-DRBREW_GRAPHICS_BACKEND=Vulkan",
		fmt.Sprintf("-DRBREW_GAME_REVISION=%s", gameRevision),
		fmt.Sprintf("-DRBREW_RPX=%s", rpxPath),
		fmt.Sprintf("-DGAMBIT_GENERATED_DIR=%s", generatedDir),
		fmt.Sprintf("-DSDL2_DIR=%s", sdl2Dir),
		fmt.Sprintf("-Dglslang_DIR=%s", glslangDir),
		fmt.Sprintf("-Dvolk_DIR=%s", volkDir),
		fmt.Sprintf("-DCMAKE_PREFIX_PATH=%s", volkPrefix),
		"-DRBREW_BUILD_GAME=ON",
		"-DRBREW_ENABLE_COVERAGE=OFF",
		"-DRBREW_ENABLE_PPC_FALLBACK=OFF",
		"-DRBREW_DIAGNOSTIC_RUNTIME=OFF",
		"-DRBREW_TEXTURE_DIAGNOSTICS=OFF",
	)
	if err := runCmdWithLog(cmdGameCfg, log); err != nil {
		failDone(fmt.Errorf("CMake configuration of the game failed: %w", err))
		return
	}

	cmdGameBuild := exec.CommandContext(ctx, "cmake", "--build", gameBuild, "--target", "Gambit", "--parallel", strconv.Itoa(cfg.ParallelJobs))
	if err := runCmdWithLog(cmdGameBuild, log); err != nil {
		failDone(fmt.Errorf("compiling Splatoon executable: %w", err))
		return
	}

	// STEP 8: Packaging game folder
	progress(8, "Packaging Game Directory", "Copying executable, assets, and custom settings", 0.90)
	log("=== Step 8/8: Game Packaging ===")
	_ = os.MkdirAll(cfg.OutputDir, 0755)

	exeSource := filepath.Join(gameBuild, "Splatoon")
	exeDest := filepath.Join(cfg.OutputDir, "Splatoon")
	if runtime.GOOS == "windows" {
		exeSource = filepath.Join(gameBuild, "Release", "Splatoon.exe")
		exeDest = filepath.Join(cfg.OutputDir, "Splatoon.exe")
	}

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

func runCmdWithLog(cmd *exec.Cmd, log func(string, ...interface{})) error {
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return err
	}

	if err := cmd.Start(); err != nil {
		return err
	}

	scannerOut := bufio.NewScanner(stdout)
	scannerErr := bufio.NewScanner(stderr)

	go func() {
		for scannerOut.Scan() {
			log("%s", scannerOut.Text())
		}
	}()

	go func() {
		for scannerErr.Scan() {
			log("%s", scannerErr.Text())
		}
	}()

	return cmd.Wait()
}

// findConfigDir returns the directory under root holding one of the CMake
// package config files, whatever the lib, lib64 or multiarch install layout.
func findConfigDir(root string, configNames ...string) (string, bool) {
	var found string
	_ = filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return nil
		}
		for _, name := range configNames {
			if entry.Name() == name {
				found = filepath.Dir(path)
				return filepath.SkipAll
			}
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

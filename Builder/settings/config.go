package settings

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"splatpc-builder/prereq"
)

type LanguageOption struct {
	ID   int
	Code string
	Name string
}

var SupportedLanguages = []LanguageOption{
	{ID: 0, Code: "JA", Name: "Japanese (日本語)"},
	{ID: 1, Code: "EN", Name: "English"},
	{ID: 2, Code: "FR", Name: "French (Français)"},
	{ID: 3, Code: "DE", Name: "German (Deutsch)"},
	{ID: 4, Code: "IT", Name: "Italian (Italiano)"},
	{ID: 5, Code: "ES", Name: "Spanish (Español)"},
	{ID: 6, Code: "ZH-CN", Name: "Simplified Chinese (简体中文)"},
	{ID: 7, Code: "KO", Name: "Korean (한국어)"},
	{ID: 8, Code: "NL", Name: "Dutch (Nederlands)"},
	{ID: 9, Code: "PT", Name: "Portuguese (Português)"},
	{ID: 10, Code: "RU", Name: "Russian (Русский)"},
	{ID: 11, Code: "ZH-TW", Name: "Traditional Chinese (繁體中文)"},
}

type Configuration struct {
	// Step 1: Paths
	BaseDumpPath string
	OutputDir    string

	// Step 2: Settings & Parameters
	PlayerName       string // default "default"
	LanguageID       int    // default 1 (English)
	ConsoleRegion    string // "EUR", "USA", "JPN"
	EnableOnline     bool   // default true (community multiplayer)
	OnlineServer     string // e.g. "173.249.46.125:60000"
	Resolution       string // "720p", "1080p", "1440p", "native"
	FrameRate        int    // 30, 60, 120, 180
	Plaza60FPS       bool
	ShaderQuality    int  // 0: Off, 1: Low, 2: Balanced, 3: High
	Fullscreen       bool // default true
	GSync            bool // default false
	FPSOverlay       bool
	Gyro             bool
	GyroSensitivity  int
	GyroInvertPitch  bool
	MouseSensitivity int
	MouseDirect      bool
	Volume           int
	CameraFOV        int
	DayCycle         bool
	Clouds           bool
	SunRays          bool
	Bloom            int
	CreateAppImage   bool // Linux single AppImage
	UpscaleTextures  bool // HD Texturing (Real-ESRGAN AI Upscaling)
	HasNvidiaGPU     bool
	DetectedGPUName  string

	// Step 3: Build jobs
	ParallelJobs int
	CleanBuild   bool
}

const FallbackOnlineServer = "173.249.46.125:60000"

func DefaultOnlineServer(repoRoot string) string {
	candidates := []string{
		filepath.Join(repoRoot, "scripts", "server.txt"),
		filepath.Join(repoRoot, "scripts", "server.public.txt"),
	}
	for _, path := range candidates {
		if data, err := os.ReadFile(path); err == nil {
			for _, line := range strings.Split(string(data), "\n") {
				line = strings.TrimSpace(line)
				if strings.HasPrefix(line, "server=") {
					val := strings.TrimPrefix(line, "server=")
					val = strings.TrimSpace(val)
					if val != "" {
						return val
					}
				}
			}
		}
	}
	return FallbackOnlineServer
}

func DefaultConfiguration(repoRoot string) Configuration {
	outDir := filepath.Join(repoRoot, "Game")
	gpu := prereq.DetectGPU()
	server := DefaultOnlineServer(repoRoot)

	return Configuration{
		BaseDumpPath:     "",
		OutputDir:        outDir,
		PlayerName:       "default",
		LanguageID:       1, // Default to English
		ConsoleRegion:    "EUR",
		EnableOnline:     true,
		OnlineServer:     server,
		Resolution:       "1080p",
		FrameRate:        60,
		Plaza60FPS:       true,
		ShaderQuality:    0, // Original clean game at 60 fps
		Fullscreen:       true,
		GSync:            false,
		FPSOverlay:       false,
		Gyro:             true,
		GyroSensitivity:  100,
		GyroInvertPitch:  false,
		MouseSensitivity: 100,
		MouseDirect:      true,
		Volume:           100,
		CameraFOV:        55,
		DayCycle:         false,
		Clouds:           true,
		SunRays:          true,
		Bloom:            25,
		CreateAppImage:   runtime.GOOS == "linux",
		UpscaleTextures:  gpu.IsNvidia, // Automatically enabled on compatible NVIDIA GPUs!
		HasNvidiaGPU:     gpu.IsNvidia,
		DetectedGPUName:  gpu.Name,
		ParallelJobs:     CalculateDefaultJobs(),
		CleanBuild:       false,
	}
}

// OutputDirFor returns the folder the built game goes to: next to the program
// in a standalone release (beside the AppImage when one is running), or
// repoRoot/Game inside the repository.
func OutputDirFor(repoRoot string, standalone bool) string {
	if standalone {
		if appImage := os.Getenv("APPIMAGE"); appImage != "" {
			return filepath.Join(filepath.Dir(appImage), "Game")
		}
		if exe, err := os.Executable(); err == nil {
			exeDir := filepath.Dir(exe)
			if !strings.HasPrefix(exeDir, os.TempDir()) {
				return filepath.Join(exeDir, "Game")
			}
		} else if cwd, err := os.Getwd(); err == nil {
			return filepath.Join(cwd, "Game")
		}
	}
	return filepath.Join(repoRoot, "Game")
}

func CalculateDefaultJobs() int {
	cpus := runtime.NumCPU()
	if cpus < 1 {
		cpus = 1
	}

	if runtime.GOOS == "linux" {
		// Parse /proc/meminfo to calculate memory-safe jobs
		if data, err := os.ReadFile("/proc/meminfo"); err == nil {
			lines := strings.Split(string(data), "\n")
			for _, line := range lines {
				if strings.HasPrefix(line, "MemAvailable:") {
					fields := strings.Fields(line)
					if len(fields) >= 2 {
						var kb int
						fmt.Sscanf(fields[1], "%d", &kb)
						memJobs := kb / (3 * 1024 * 1024)
						if memJobs < 1 {
							memJobs = 1
						}
						if cpus > memJobs {
							return memJobs
						}
					}
				}
			}
		}
	}
	return cpus
}

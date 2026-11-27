package settings

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// LoadSettingsIni reads an existing settings.ini file and populates Configuration fields.
func LoadSettingsIni(filePath string, cfg *Configuration) error {
	data, err := os.ReadFile(filePath)
	if err != nil {
		return err
	}

	lines := strings.Split(string(data), "\n")
	for _, rawLine := range lines {
		line := strings.TrimSpace(rawLine)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		eqIdx := strings.Index(line, "=")
		if eqIdx < 0 {
			continue
		}
		key := strings.TrimSpace(line[:eqIdx])
		val := strings.TrimSpace(line[eqIdx+1:])

		switch key {
		case "player_name":
			if val != "" {
				cfg.PlayerName = val
			}
		case "resolution":
			if val != "" {
				cfg.Resolution = val
			}
		case "plaza_60fps":
			cfg.Plaza60FPS = (val != "0")
		case "frame_rate":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.FrameRate = n
			}
		case "gsync":
			cfg.GSync = (val != "0")
		case "fullscreen":
			cfg.Fullscreen = (val != "0")
		case "fps_overlay":
			cfg.FPSOverlay = (val != "0")
		case "gyro":
			cfg.Gyro = (val != "0")
		case "gyro_sensitivity":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.GyroSensitivity = n
			}
		case "gyro_invert_pitch":
			cfg.GyroInvertPitch = (val != "0")
		case "mouse_sensitivity":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.MouseSensitivity = n
			}
		case "mouse_direct":
			cfg.MouseDirect = (val != "0")
		case "disable_auto_pitch":
			cfg.DisableAutoPitch = (val != "0")
		case "volume":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.Volume = n
			}
		case "shader_quality":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.ShaderQuality = n
			}
		case "anisotropic_filtering":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.AnisotropicFiltering = n
			}
		case "depth_of_field":
			cfg.DepthOfField = (val != "0")
		case "antialiasing":
			cfg.AntiAliasing = (val != "0")
			if !cfg.AntiAliasing {
				cfg.AAMode = "off"
			} else if cfg.AAMode == "off" || cfg.AAMode == "" {
				cfg.AAMode = "fxaa"
			}
		case "aa_mode":
			switch strings.ToLower(val) {
			case "0", "off":
				cfg.AAMode = "off"
				cfg.AntiAliasing = false
			case "2", "smaa":
				cfg.AAMode = "smaa"
				cfg.AntiAliasing = true
			case "1", "fxaa":
				cfg.AAMode = "fxaa"
				cfg.AntiAliasing = true
			}
		case "upscaler":
			switch strings.ToLower(val) {
			case "1", "fsr", "fsr1":
				cfg.Upscaler = "fsr"
			case "2", "anime4k":
				cfg.Upscaler = "anime4k"
			default:
				cfg.Upscaler = "off"
			}
		case "upscaler_sharpness":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.UpscalerSharpness = n
			}
		case "motion_blur":
			cfg.MotionBlur = (val != "0")
		case "camera_fov":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.CameraFOV = n
			}
		case "day_cycle":
			cfg.DayCycle = (val != "0")
		case "clouds":
			cfg.Clouds = (val != "0")
		case "sun_rays":
			cfg.SunRays = (val != "0")
		case "bloom":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.Bloom = n
			}
		case "console_language", "language":
			if n, err := strconv.Atoi(val); err == nil {
				cfg.LanguageID = n
			}
		case "console_region", "region":
			if val != "" {
				cfg.ConsoleRegion = val
			}
		case "hd_textures", "upscale_textures":
			cfg.UpscaleTextures = (val != "0")
		case "online_server":
			cfg.OnlineServer = val
			cfg.EnableOnline = (val != "")
		case "online_register":
			cfg.OnlineRegister = val
		case "online_account":
			cfg.OnlineAccount = val
		case "online_boss":
			cfg.OnlineBoss = val
		case "online_nncs1":
			cfg.OnlineNncs1 = val
		case "online_nncs2":
			cfg.OnlineNncs2 = val
		case "pseudo":
			if val != "" {
				cfg.PlayerName = val
			}
		}
	}
	return nil
}

// WriteSettingsIni writes configuration to settings.ini.
// If an existing file is present, it preserves comments, custom keybindings, looks,
// and all other non-default/unmanaged settings while updating only the managed keys.
func WriteSettingsIni(cfg Configuration, outFolder string) error {
	filePath := filepath.Join(outFolder, "settings.ini")

	onlineServerVal := ""
	online := map[string]string{"online_register": "", "online_account": "", "online_boss": "", "online_nncs1": "", "online_nncs2": ""}
	if cfg.EnableOnline {
		onlineServerVal = strings.TrimSpace(cfg.OnlineServer)
		online["online_register"] = strings.TrimSpace(cfg.OnlineRegister)
		online["online_account"] = strings.TrimSpace(cfg.OnlineAccount)
		online["online_boss"] = strings.TrimSpace(cfg.OnlineBoss)
		online["online_nncs1"] = strings.TrimSpace(cfg.OnlineNncs1)
		online["online_nncs2"] = strings.TrimSpace(cfg.OnlineNncs2)
	}

	aaMode := strings.ToLower(strings.TrimSpace(cfg.AAMode))
	if aaMode != "off" && aaMode != "fxaa" && aaMode != "smaa" {
		if cfg.AntiAliasing {
			aaMode = "fxaa"
		} else {
			aaMode = "off"
		}
	}
	upscaler := strings.ToLower(strings.TrimSpace(cfg.Upscaler))
	if upscaler != "fsr" && upscaler != "anime4k" {
		upscaler = "off"
	}

	managedValues := map[string]string{
		"player_name":       cfg.PlayerName,
		"pseudo":            cfg.PlayerName,
		"online_server":     onlineServerVal,
		"resolution":        cfg.Resolution,
		"plaza_60fps":       strconv.Itoa(boolToInt(cfg.Plaza60FPS)),
		"frame_rate":        strconv.Itoa(cfg.FrameRate),
		"gsync":             strconv.Itoa(boolToInt(cfg.GSync)),
		"fullscreen":        strconv.Itoa(boolToInt(cfg.Fullscreen)),
		"fps_overlay":       strconv.Itoa(boolToInt(cfg.FPSOverlay)),
		"gyro":              strconv.Itoa(boolToInt(cfg.Gyro)),
		"gyro_sensitivity":  strconv.Itoa(cfg.GyroSensitivity),
		"gyro_invert_pitch": strconv.Itoa(boolToInt(cfg.GyroInvertPitch)),
		"mouse_sensitivity":     strconv.Itoa(cfg.MouseSensitivity),
		"mouse_direct":          strconv.Itoa(boolToInt(cfg.MouseDirect)),
		"disable_auto_pitch":    strconv.Itoa(boolToInt(cfg.DisableAutoPitch)),
		"hd_textures":           strconv.Itoa(boolToInt(cfg.UpscaleTextures)),
		"volume":                strconv.Itoa(cfg.Volume),
		"shader_quality":        strconv.Itoa(cfg.ShaderQuality),
		"anisotropic_filtering": strconv.Itoa(cfg.AnisotropicFiltering),
		"depth_of_field":        strconv.Itoa(boolToInt(cfg.DepthOfField)),
		"antialiasing":          strconv.Itoa(boolToInt(aaMode != "off")),
		"aa_mode":               aaMode,
		"upscaler":              upscaler,
		"upscaler_sharpness":    strconv.Itoa(cfg.UpscalerSharpness),
		"motion_blur":           strconv.Itoa(boolToInt(cfg.MotionBlur)),
		"day_cycle":         strconv.Itoa(boolToInt(cfg.DayCycle)),
		"clouds":            strconv.Itoa(boolToInt(cfg.Clouds)),
		"sun_rays":          strconv.Itoa(boolToInt(cfg.SunRays)),
		"bloom":             strconv.Itoa(cfg.Bloom),
		"camera_fov":        strconv.Itoa(cfg.CameraFOV),
	}
	for key, val := range online {
		managedValues[key] = val
	}

	data, err := os.ReadFile(filePath)
	if err == nil {
		// Non-destructive update of existing file
		existingLines := strings.Split(string(data), "\n")
		updatedLines := make([]string, 0, len(existingLines)+len(managedValues))
		seenKeys := make(map[string]bool)

		for _, rawLine := range existingLines {
			line := strings.TrimRight(rawLine, "\r")
			trimmed := strings.TrimSpace(line)
			if trimmed == "" || strings.HasPrefix(trimmed, "#") {
				updatedLines = append(updatedLines, line)
				continue
			}

			eqIdx := strings.Index(line, "=")
			if eqIdx < 0 {
				updatedLines = append(updatedLines, line)
				continue
			}

			key := strings.TrimSpace(line[:eqIdx])
			if newVal, ok := managedValues[key]; ok {
				updatedLines = append(updatedLines, fmt.Sprintf("%s=%s", key, newVal))
				seenKeys[key] = true
			} else {
				// Preserve existing non-default and custom entries (keybindings, pad bindings, look presets, etc.)
				updatedLines = append(updatedLines, line)
			}
		}

		// Append any managed keys that were not originally present in the existing file
		for key, val := range managedValues {
			if !seenKeys[key] {
				updatedLines = append(updatedLines, fmt.Sprintf("%s=%s", key, val))
			}
		}

		outputContent := strings.Join(updatedLines, "\n")
		if !strings.HasSuffix(outputContent, "\n") {
			outputContent += "\n"
		}
		return os.WriteFile(filePath, []byte(outputContent), 0644)
	}

	// No existing file: create standard default configuration
	content := fmt.Sprintf(`# Splatoon PC port settings (ESC menu)
# Configured via SplatPC Builder
player_name=%s
pseudo=%s
online_server=%s
resolution=%s
plaza_60fps=%d
frame_rate=%d
gsync=%d
fullscreen=%d
fps_overlay=%d
simple_overlay=0
gyro=%d
gyro_sensitivity=%d
gyro_invert_pitch=%d
mouse_sensitivity=%d
volume=%d
shader_quality=%d
weather=0
time_of_day=780
day_cycle=%d
day_length=24
clouds=%d
sun_rays=%d
bloom=%d
antialiasing=%d
aa_mode=%s
upscaler=%s
upscaler_sharpness=%d
motion_blur=%d
depth_of_field=%d
anisotropic_filtering=%d
camera_fov=%d
focus_blur=0
vignette=0
white_balance=0
sway_strength=100
cloud_speed=100
wind_direction=0
mouse_direct=%d
disable_auto_pitch=%d
hd_textures=%d
camera_distance=100
camera_height=0
realistic_look=0
cloud_amount=-1
big_cloud=1
cumulus_amount=-1
real_time=0
fog=0
saturation=100
contrast=100
exposure=0
gamma=100
shadow_lift=0
highlight_softness=0
sharpness=100
clarity=100
film_look=0
camera_motion=0
cinema_mode=0
bindings=2
`,
		cfg.PlayerName,
		cfg.PlayerName,
		onlineServerVal,
		cfg.Resolution,
		boolToInt(cfg.Plaza60FPS),
		cfg.FrameRate,
		boolToInt(cfg.GSync),
		boolToInt(cfg.Fullscreen),
		boolToInt(cfg.FPSOverlay),
		boolToInt(cfg.Gyro),
		cfg.GyroSensitivity,
		boolToInt(cfg.GyroInvertPitch),
		cfg.MouseSensitivity,
		cfg.Volume,
		cfg.ShaderQuality,
		boolToInt(cfg.DayCycle),
		boolToInt(cfg.Clouds),
		boolToInt(cfg.SunRays),
		cfg.Bloom,
		boolToInt(aaMode != "off"),
		aaMode,
		upscaler,
		cfg.UpscalerSharpness,
		boolToInt(cfg.MotionBlur),
		boolToInt(cfg.DepthOfField),
		cfg.AnisotropicFiltering,
		cfg.CameraFOV,
		boolToInt(cfg.MouseDirect),
		boolToInt(cfg.DisableAutoPitch),
		boolToInt(cfg.UpscaleTextures),
	)

	for _, key := range []string{"online_register", "online_account", "online_boss", "online_nncs1", "online_nncs2"} {
		if online[key] != "" {
			content += key + "=" + online[key] + "\n"
		}
	}
	return os.WriteFile(filePath, []byte(content), 0644)
}

// regionMasks are the Cafe region bits the game reads from RBREW_CONSOLE_REGION
var regionMasks = map[string]string{"JPN": "0x1", "USA": "0x2", "EUR": "0x4"}

// RegionMask turns a region name into the number the game expects. The game
// quits at startup on a name like "EUR" an empty value lets it use the
// region of the games own meta.xml
func RegionMask(region string) string {
	region = strings.ToLower(strings.TrimSpace(region))
	if mask, ok := regionMasks[strings.ToUpper(region)]; ok {
		return mask
	}
	// A number is passed as is: decimal or hex with 0x like the game reads it
	if _, err := strconv.ParseUint(region, 0, 32); err == nil {
		return region
	}
	return ""
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

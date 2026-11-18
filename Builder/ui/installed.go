package ui

import (
	"fmt"
	"os"
	"time"

	"fyne.io/fyne/v2"

	"splatpc-builder/engine"
)

// Preference keys of the last successful build, so a restarted Builder can
// find it even in an output folder the player picked.
const (
	prefLastOutputDir = "lastBuild.outputDir"
	prefLastBuiltAt   = "lastBuild.builtAt"
	prefLastSeconds   = "lastBuild.seconds"
)

type installedBuild struct {
	dir      string
	builtAt  time.Time
	duration time.Duration // zero when unknown
}

func rememberBuild(prefs fyne.Preferences, dir string, builtAt time.Time, duration time.Duration) {
	prefs.SetString(prefLastOutputDir, dir)
	prefs.SetString(prefLastBuiltAt, builtAt.Format(time.RFC3339))
	prefs.SetFloat(prefLastSeconds, duration.Seconds())
}

// findInstalledBuild looks for a built game in the folder of the last build,
// then in the default output folder.
func findInstalledBuild(prefs fyne.Preferences, defaultDir string) (installedBuild, bool) {
	lastDir := prefs.String(prefLastOutputDir)
	for _, dir := range []string{lastDir, defaultDir} {
		if dir == "" {
			continue
		}
		info, err := os.Stat(engine.GameExecutable(dir))
		if err != nil || info.IsDir() {
			continue
		}
		build := installedBuild{dir: dir, builtAt: info.ModTime()}
		if dir == lastDir {
			if builtAt, err := time.Parse(time.RFC3339, prefs.String(prefLastBuiltAt)); err == nil {
				build.builtAt = builtAt
			}
			build.duration = time.Duration(prefs.Float(prefLastSeconds) * float64(time.Second))
		}
		return build, true
	}
	return installedBuild{}, false
}

func (b installedBuild) describe() string {
	when := b.builtAt.Format("2 Jan 2006, 15:04")
	if b.duration > 0 {
		return fmt.Sprintf("%s (took %s)", when, b.duration.Round(time.Second))
	}
	return when
}

package settings

import (
	"bufio"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// ServerPreset is one scripts/server*.txt file: the online settings of a known server, written to settings.ini
// (server -> online_server, register -> online_register, account -> online_account, boss -> online_boss,
// nncs1/nncs2 -> online_nncs1/online_nncs2). An empty value leaves the game's own default.
type ServerPreset struct {
	Name     string
	Server   string
	Register string
	Account  string
	Boss     string
	Nncs1    string
	Nncs2    string
}

// ServerPresets lists the presets of repoRoot/scripts, the default one (server.public.txt) first.
func ServerPresets(repoRoot string) []ServerPreset {
	files, _ := filepath.Glob(filepath.Join(repoRoot, "scripts", "server*.txt"))
	sort.SliceStable(files, func(i, j int) bool {
		return filepath.Base(files[i]) == "server.public.txt" && filepath.Base(files[j]) != "server.public.txt"
	})
	var presets []ServerPreset
	for _, file := range files {
		if p, ok := readPreset(file); ok {
			presets = append(presets, p)
		}
	}
	return presets
}

func readPreset(path string) (ServerPreset, bool) {
	f, err := os.Open(path)
	if err != nil {
		return ServerPreset{}, false
	}
	defer f.Close()
	p := ServerPreset{Name: strings.TrimSuffix(filepath.Base(path), ".txt")}
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		key, val, ok := strings.Cut(line, "=")
		if !ok || strings.HasPrefix(line, "#") {
			continue
		}
		val = strings.TrimSpace(val)
		switch strings.TrimSpace(key) {
		case "name":
			p.Name = val
		case "server":
			p.Server = val
		case "register":
			p.Register = val
		case "account":
			p.Account = val
		case "boss":
			p.Boss = val
		case "nncs1":
			p.Nncs1 = val
		case "nncs2":
			p.Nncs2 = val
		}
	}
	return p, p.Server != ""
}

// ApplyPreset copies a preset's online settings into the configuration.
func (c *Configuration) ApplyPreset(p ServerPreset) {
	c.OnlineServer, c.OnlineRegister, c.OnlineAccount, c.OnlineBoss = p.Server, p.Register, p.Account, p.Boss
	c.OnlineNncs1, c.OnlineNncs2 = p.Nncs1, p.Nncs2
}

// PresetIndex returns the preset whose settings are the configuration's, or -1 (a custom server).
func (c *Configuration) PresetIndex(presets []ServerPreset) int {
	for i, p := range presets {
		if p.Server == c.OnlineServer && p.Register == c.OnlineRegister && p.Account == c.OnlineAccount &&
			p.Boss == c.OnlineBoss && p.Nncs1 == c.OnlineNncs1 && p.Nncs2 == c.OnlineNncs2 {
			return i
		}
	}
	return -1
}

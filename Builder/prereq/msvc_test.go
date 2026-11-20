package prereq

import (
	"strings"
	"testing"
)

func TestParseVswhereFindsBuildToolsInstall(t *testing.T) {
	out := []byte(`[{"installationPath": "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools",
		"installationVersion": "17.14.36310.24", "displayName": "Visual Studio Build Tools 2022"}]`)
	vs, ok := parseVswhere(out)
	if !ok {
		t.Fatal("expected the Build Tools install to be found")
	}
	if vs.Path != `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` {
		t.Errorf("unexpected path %q", vs.Path)
	}
	if got := vs.CMakeGenerator(); got != "Visual Studio 17 2022" {
		t.Errorf("unexpected generator %q", got)
	}
}

func TestParseVswhereWithoutInstall(t *testing.T) {
	for _, out := range []string{"[]", "", "not json"} {
		if _, ok := parseVswhere([]byte(out)); ok {
			t.Errorf("expected no install for %q", out)
		}
	}
}

func TestVisualStudio2026NeedsCMake42(t *testing.T) {
	vs2026 := VisualStudio{Version: "18.10.12210.168"}
	if msg := cmakeSupportsVisualStudio("4.1.2", vs2026); !strings.Contains(msg, "4.2") {
		t.Errorf("expected CMake 4.1.2 to be rejected for VS 2026, got %q", msg)
	}
	if msg := cmakeSupportsVisualStudio("4.4.4", vs2026); msg != "" {
		t.Errorf("expected CMake 4.4.4 to build with VS 2026, got %q", msg)
	}
	if msg := cmakeSupportsVisualStudio("3.20.0", VisualStudio{Version: "17.14"}); msg != "" {
		t.Errorf("expected CMake 3.20 to build with VS 2022, got %q", msg)
	}
}

func TestVisualStudioGenerator(t *testing.T) {
	cases := map[string]string{
		"16.11.5":  "Visual Studio 16 2019",
		"17.14.36": "Visual Studio 17 2022",
		"18.0.1":   "Visual Studio 18 2026",
		"":         DefaultVisualStudioGenerator,
	}
	for version, want := range cases {
		if got := (VisualStudio{Version: version}).CMakeGenerator(); got != want {
			t.Errorf("version %q: expected %q, got %q", version, want, got)
		}
	}
}

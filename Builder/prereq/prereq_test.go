package prereq

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseVersion(t *testing.T) {
	cases := []struct {
		input    string
		expected string
	}{
		{"cmake version 3.25.1", "3.25.1"},
		{"Python 3.12.0", "3.12.0"},
		{"gcc (GCC) 13.2.1 20230801", "13.2.1"},
	}

	for _, c := range cases {
		result := parseVersion(c.input)
		if result != c.expected {
			t.Errorf("parseVersion(%q) = %q, expected %q", c.input, result, c.expected)
		}
	}
}

func TestIsVersionOlder(t *testing.T) {
	if !isVersionOlder("3.19.0", "3.20") {
		t.Errorf("Expected 3.19.0 to be older than 3.20")
	}
	if isVersionOlder("3.20.0", "3.20") {
		t.Errorf("Expected 3.20.0 to not be older than 3.20")
	}
	if isVersionOlder("4.4.4", "3.20") {
		t.Errorf("Expected 4.4.4 to not be older than 3.20")
	}
}

func TestGenerateInstallCommand(t *testing.T) {
	items := []CheckItem{
		{ID: "cmake", Found: false},
	}
	cmdArch := generateInstallCommand("arch", items)
	if cmdArch == "" {
		t.Errorf("Expected install command for arch")
	}
	cmdDeb := generateInstallCommand("debian", items)
	if cmdDeb == "" {
		t.Errorf("Expected install command for debian")
	}
}

func TestGenerateInstallCommandWindows(t *testing.T) {
	// 1. Only Python missing
	itemsOnlyPy := []CheckItem{
		{ID: "cmake", Found: true},
		{ID: "python3", Found: false},
	}
	cmdPy := generateInstallCommand("windows", itemsOnlyPy)
	if cmdPy != "winget install Python.Python.3.12" {
		t.Errorf("Unexpected windows install command for Python: %q", cmdPy)
	}

	// 2. CMake and Ninja missing
	itemsCmakeNinja := []CheckItem{
		{ID: "cmake", Found: false},
		{ID: "ninja", Found: false},
		{ID: "python3", Found: true},
	}
	cmdCmakeNinja := generateInstallCommand("windows", itemsCmakeNinja)
	if !strings.Contains(cmdCmakeNinja, "Kitware.CMake") || !strings.Contains(cmdCmakeNinja, "Ninja-build.Ninja") {
		t.Errorf("Expected CMake and Ninja in command, got %q", cmdCmakeNinja)
	}

	// 3. Compiler missing
	itemsCompiler := []CheckItem{
		{ID: "compiler", Found: false},
	}
	cmdCompiler := generateInstallCommand("windows", itemsCompiler)
	if !strings.Contains(cmdCompiler, "Microsoft.VisualStudio.2022.BuildTools") {
		t.Errorf("Expected Visual Studio build tools in command, got %q", cmdCompiler)
	}

	// 4. None missing
	itemsNone := []CheckItem{
		{ID: "cmake", Found: true},
		{ID: "python3", Found: true},
	}
	cmdNone := generateInstallCommand("windows", itemsNone)
	if cmdNone != "" {
		t.Errorf("Expected empty command when none missing, got %q", cmdNone)
	}
}

func TestPrependToPath(t *testing.T) {
	orig := os.Getenv("PATH")
	defer os.Setenv("PATH", orig)

	dummyDir := filepath.Join(os.TempDir(), "dummy-splatpc-path-test")
	_ = os.MkdirAll(dummyDir, 0755)
	defer os.RemoveAll(dummyDir)

	PrependToPath(dummyDir)
	current := os.Getenv("PATH")
	if !strings.Contains(current, dummyDir) {
		t.Errorf("Expected PATH to contain %s, got %s", dummyDir, current)
	}

	// Ensure no duplicate insertion
	PrependToPath(dummyDir)
	count := strings.Count(os.Getenv("PATH"), dummyDir)
	if count != 1 {
		t.Errorf("Expected exactly 1 occurrence of dummyDir, got %d", count)
	}
}

func TestPythonInfoCommand(t *testing.T) {
	py := PythonInfo{
		Executable: "py",
		Args:       []string{"-3"},
		Version:    "3.12.0",
		Path:       `C:\Windows\py.exe`,
	}

	cmd := py.Command(context.Background(), "-c", "print('hello')")
	if cmd.Path != "py" {
		t.Errorf("Expected executable 'py', got %q", cmd.Path)
	}
	if len(cmd.Args) < 3 || cmd.Args[1] != "-3" || cmd.Args[2] != "-c" {
		t.Errorf("Expected args [-3, -c, ...], got %v", cmd.Args)
	}
}

func TestVSWhereJsonParsing(t *testing.T) {
	sampleJSON := `[
		{
			"instanceId": "1234abcd",
			"installationPath": "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
			"installationVersion": "17.8.34330.188",
			"displayName": "Visual Studio Community 2022"
		}
	]`

	var entries []vsWhereEntry
	err := json.Unmarshal([]byte(sampleJSON), &entries)
	if err != nil {
		t.Fatalf("Failed to parse sample vswhere json: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("Expected 1 entry, got %d", len(entries))
	}
	if entries[0].DisplayName != "Visual Studio Community 2022" {
		t.Errorf("Unexpected display name: %s", entries[0].DisplayName)
	}
	if !strings.Contains(entries[0].InstallationPath, "Community") {
		t.Errorf("Unexpected path: %s", entries[0].InstallationPath)
	}
}

func TestDetectGPU(t *testing.T) {
	gpu := DetectGPU()
	if gpu.Name == "" {
		t.Errorf("Expected GPU name to be non-empty")
	}
	t.Logf("Detected GPU: %s (NVIDIA: %v, CUDA: %v, Status: %s)", gpu.Name, gpu.IsNvidia, gpu.IsCUDACompatible, gpu.StatusMessage)
}

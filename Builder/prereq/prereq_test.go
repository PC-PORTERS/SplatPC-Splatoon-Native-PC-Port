package prereq

import (
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

func TestDetectGPU(t *testing.T) {
	gpu := DetectGPU()
	if gpu.Name == "" {
		t.Errorf("Expected GPU name to be non-empty")
	}
	t.Logf("Detected GPU: %s (NVIDIA: %v, CUDA: %v, Status: %s)", gpu.Name, gpu.IsNvidia, gpu.IsCUDACompatible, gpu.StatusMessage)
}

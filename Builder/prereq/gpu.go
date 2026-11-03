package prereq

import (
	"bufio"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

type GPUInfo struct {
	IsNvidia         bool
	Name             string
	DriverVersion    string
	IsCUDACompatible bool
	StatusMessage    string
}

// DetectGPU scans the system for graphics hardware, prioritizing NVIDIA GPUs
// compatible with CUDA-accelerated Real-ESRGAN HD texturing.
func DetectGPU() GPUInfo {
	info := GPUInfo{
		IsNvidia:         false,
		Name:             "Unknown GPU",
		DriverVersion:    "",
		IsCUDACompatible: false,
		StatusMessage:    "No graphics card detected",
	}

	// 1. Try official nvidia-smi tool (supported on both Windows and Linux)
	if checkNvidiaSmi(&info) {
		return info
	}

	// 2. Try Python PyTorch CUDA check if python is available
	if checkPyTorchCUDA(&info) {
		return info
	}

	// 3. Platform-specific fallbacks
	if runtime.GOOS == "windows" {
		if checkWindowsWMI(&info) {
			return info
		}
	} else if runtime.GOOS == "linux" {
		if checkLinuxProcNvidia(&info) {
			return info
		}
		if checkLinuxPCI(&info) {
			return info
		}
		if checkLinuxLspci(&info) {
			return info
		}
	}

	if info.IsNvidia {
		info.IsCUDACompatible = true
		info.StatusMessage = "Compatible NVIDIA GPU detected (CUDA accelerated)"
	} else if info.Name != "Unknown GPU" {
		info.StatusMessage = "Non-NVIDIA GPU detected (HD texturing requires NVIDIA for fast upscaling)"
	} else {
		info.StatusMessage = "No compatible NVIDIA GPU found"
	}

	return info
}

func checkNvidiaSmi(info *GPUInfo) bool {
	smiBin := "nvidia-smi"
	if runtime.GOOS == "windows" {
		candidates := []string{
			`C:\Windows\System32\nvidia-smi.exe`,
			`C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe`,
		}
		for _, c := range candidates {
			if _, err := os.Stat(c); err == nil {
				smiBin = c
				break
			}
		}
	}

	cmd := exec.Command(smiBin, "--query-gpu=name,driver_version", "--format=csv,noheader")
	out, err := cmd.Output()
	if err == nil {
		line := strings.TrimSpace(string(out))
		if line != "" {
			parts := strings.Split(line, ",")
			gpuName := strings.TrimSpace(parts[0])
			driverVer := ""
			if len(parts) > 1 {
				driverVer = strings.TrimSpace(parts[1])
			}

			if gpuName != "" {
				info.IsNvidia = true
				info.Name = gpuName
				info.DriverVersion = driverVer
				info.IsCUDACompatible = true
				info.StatusMessage = "Compatible NVIDIA GPU detected via nvidia-smi"
				return true
			}
		}
	}
	return false
}

func checkPyTorchCUDA(info *GPUInfo) bool {
	pyBin := "python3"
	if runtime.GOOS == "windows" {
		if _, err := exec.LookPath("python3"); err != nil {
			pyBin = "python"
		}
	}

	cmd := exec.Command(pyBin, "-c", "import torch; print(f'{torch.cuda.is_available()}|{torch.cuda.get_device_name(0) if torch.cuda.is_available() else \"\"}')")
	out, err := cmd.Output()
	if err == nil {
		line := strings.TrimSpace(string(out))
		parts := strings.Split(line, "|")
		if len(parts) >= 2 && strings.EqualFold(parts[0], "true") {
			name := strings.TrimSpace(parts[1])
			if name == "" {
				name = "NVIDIA CUDA GPU"
			}
			info.IsNvidia = true
			info.Name = name
			info.IsCUDACompatible = true
			info.StatusMessage = "CUDA-enabled NVIDIA GPU confirmed via PyTorch"
			return true
		}
	}
	return false
}

func checkLinuxProcNvidia(info *GPUInfo) bool {
	data, err := os.ReadFile("/proc/driver/nvidia/version")
	if err == nil {
		content := string(data)
		lines := strings.Split(content, "\n")
		driver := ""
		if len(lines) > 0 {
			driver = strings.TrimSpace(lines[0])
		}
		info.IsNvidia = true
		info.Name = "NVIDIA Graphics Device"
		info.DriverVersion = driver
		info.IsCUDACompatible = true
		info.StatusMessage = "NVIDIA driver active (/proc/driver/nvidia)"
		return true
	}
	return false
}

func checkLinuxPCI(info *GPUInfo) bool {
	matches, err := filepath.Glob("/sys/bus/pci/devices/*/vendor")
	if err == nil {
		for _, m := range matches {
			data, err := os.ReadFile(m)
			if err == nil {
				vendor := strings.TrimSpace(string(data))
				if vendor == "0x10de" { // NVIDIA PCI Vendor ID
					info.IsNvidia = true
					info.Name = "NVIDIA PCI Graphics Device"
					info.IsCUDACompatible = true
					info.StatusMessage = "NVIDIA PCI hardware detected (vendor 0x10de)"
					return true
				}
			}
		}
	}
	return false
}

func checkLinuxLspci(info *GPUInfo) bool {
	cmd := exec.Command("lspci")
	out, err := cmd.Output()
	if err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(out)))
		for scanner.Scan() {
			line := scanner.Text()
			lower := strings.ToLower(line)
			if strings.Contains(lower, "vga") || strings.Contains(lower, "3d controller") || strings.Contains(lower, "display") {
				if strings.Contains(lower, "nvidia") {
					info.IsNvidia = true
					info.Name = extractGPUNameFromLspci(line)
					info.IsCUDACompatible = true
					info.StatusMessage = "NVIDIA GPU detected via lspci"
					return true
				} else if info.Name == "Unknown GPU" {
					info.Name = extractGPUNameFromLspci(line)
				}
			}
		}
	}
	return false
}

func extractGPUNameFromLspci(line string) string {
	parts := strings.SplitN(line, ": ", 2)
	if len(parts) == 2 {
		return strings.TrimSpace(parts[1])
	}
	return line
}

func checkWindowsWMI(info *GPUInfo) bool {
	// Query Win32_VideoController via PowerShell
	cmd := exec.Command("powershell", "-NoProfile", "-Command", "Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name")
	out, err := cmd.Output()
	if err == nil {
		lines := strings.Split(string(out), "\n")
		for _, rawLine := range lines {
			line := strings.TrimSpace(rawLine)
			if line == "" {
				continue
			}
			lower := strings.ToLower(line)
			if strings.Contains(lower, "nvidia") || strings.Contains(lower, "geforce") || strings.Contains(lower, "rtx") || strings.Contains(lower, "gtx") {
				info.IsNvidia = true
				info.Name = line
				info.IsCUDACompatible = true
				info.StatusMessage = "Compatible NVIDIA GPU detected via Windows WMI"
				return true
			} else if info.Name == "Unknown GPU" {
				info.Name = line
			}
		}
	}
	return false
}

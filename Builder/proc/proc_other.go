//go:build !windows

package proc

import "os/exec"

func hideWindow(*exec.Cmd) {}

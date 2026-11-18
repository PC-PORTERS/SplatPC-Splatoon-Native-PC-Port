package proc

import (
	"os/exec"
	"syscall"
)

// createNoWindow runs a console program without opening a console window.
// Programs it starts (cmake -> cl.exe, link.exe) share that hidden console,
// and their output still reaches the Builder through the pipes.
const createNoWindow = 0x08000000

func hideWindow(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
}

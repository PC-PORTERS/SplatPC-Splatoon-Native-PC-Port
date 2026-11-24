package proc

import (
	"os/exec"
	"strconv"
	"syscall"
	"time"
)

// createNoWindow runs a console program without opening a console window.
// Programs it starts (cmake -> cl.exe, link.exe) share that hidden console,
// and their output still reaches the Builder through the pipes.
const createNoWindow = 0x08000000

func hideWindow(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
}

func configureCancel(cmd *exec.Cmd) {
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return nil
		}
		// Terminate the whole process tree (cmake -> MSBuild/ninja -> cl.exe/clang++/link.exe)
		// so child processes cannot keep stdout/stderr pipes open after cancellation.
		if err := Command("taskkill", "/T", "/F", "/PID", strconv.Itoa(cmd.Process.Pid)).Run(); err != nil {
			return cmd.Process.Kill()
		}
		return nil
	}
	cmd.WaitDelay = 3 * time.Second
}

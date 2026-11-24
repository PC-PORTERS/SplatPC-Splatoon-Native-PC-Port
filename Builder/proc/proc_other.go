//go:build !windows

package proc

import (
	"os/exec"
	"syscall"
	"time"
)

func hideWindow(*exec.Cmd) {}

func configureCancel(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return nil
		}
		// Signal the whole process group (-pgid) so child compilers exit with cmake/ninja.
		if err := syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL); err != nil {
			return cmd.Process.Kill()
		}
		return nil
	}
	cmd.WaitDelay = 3 * time.Second
}

// Package proc starts the helper programs the Builder runs (compilers, cmake,
// version checks) without a console window of their own.
package proc

import (
	"context"
	"os/exec"
)

// Command is exec.Command for a background program.
func Command(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	hideWindow(cmd)
	return cmd
}

// CommandContext is exec.CommandContext for a background program.
func CommandContext(ctx context.Context, name string, args ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, name, args...)
	hideWindow(cmd)
	configureCancel(cmd)
	return cmd
}

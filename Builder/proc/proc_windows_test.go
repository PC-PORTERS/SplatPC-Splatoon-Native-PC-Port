package proc

import (
	"context"
	"os/exec"
	"testing"
)

func TestCommandsOpenNoConsoleWindow(t *testing.T) {
	for _, cmd := range []*exec.Cmd{
		Command("cmake", "--version"),
		CommandContext(context.Background(), "cmake", "--version"),
	} {
		if cmd.SysProcAttr == nil || !cmd.SysProcAttr.HideWindow || cmd.SysProcAttr.CreationFlags&createNoWindow == 0 {
			t.Errorf("%s would open a console window", cmd)
		}
	}
}

func TestHiddenCommandStillReturnsOutput(t *testing.T) {
	out, err := Command("cmd", "/c", "echo hello").Output()
	if err != nil {
		t.Fatalf("running a hidden command: %v", err)
	}
	if string(out) != "hello\r\n" {
		t.Errorf("output = %q, expected %q", out, "hello\r\n")
	}
}

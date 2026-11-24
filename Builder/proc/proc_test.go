package proc

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"os/exec"
	"testing"
	"time"
)

func TestHelperChildTree(t *testing.T) {
	role := os.Getenv("SPLATPC_PROC_TEST_ROLE")
	switch role {
	case "":
		return
	case "child":
		time.Sleep(15 * time.Second)
		os.Exit(0)
	case "parent":
		child := exec.Command(os.Args[0], "-test.run=^TestHelperChildTree$")
		child.Env = append(os.Environ(), "SPLATPC_PROC_TEST_ROLE=child")
		child.Stdout = os.Stdout
		child.Stderr = os.Stderr
		if err := child.Start(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		fmt.Println("ready")
		_ = child.Wait()
		os.Exit(0)
	}
}

func TestCommandContextCancelStopsChildTree(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cmd := CommandContext(ctx, os.Args[0], "-test.run=^TestHelperChildTree$")
	cmd.Env = append(os.Environ(), "SPLATPC_PROC_TEST_ROLE=parent")
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("StdoutPipe: %v", err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	scanner := bufio.NewScanner(stdout)
	ready := false
	for scanner.Scan() {
		if scanner.Text() == "ready" {
			ready = true
			break
		}
	}
	if !ready {
		cancel()
		_ = cmd.Wait()
		t.Fatalf("parent helper never signalled ready (scan err=%v)", scanner.Err())
	}

	start := time.Now()
	cancel()
	for scanner.Scan() {
	}
	_ = cmd.Wait()

	if elapsed := time.Since(start); elapsed >= 5*time.Second {
		t.Fatalf("cancelling CommandContext took %s; child process kept pipe open", elapsed)
	}
}

package engine

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
)

type logFunc func(format string, args ...interface{})

// statusDLLNotFound is the exit code Windows gives a program that could not
// start because a DLL it links is missing.
const statusDLLNotFound = 0xC0000135

// errorLinePattern matches the first line worth showing from a failed tool:
// CMake errors, MSVC (English or German) and GCC/Clang diagnostics, Ninja.
var errorLinePattern = regexp.MustCompile(`CMake Error|\b(?:error|Fehler) [A-Z]+[0-9]+|error:|FAILED:`)

// errorExcerpt keeps the first error line of an output stream and the
// indented lines right after it, where CMake puts the actual message.
type errorExcerpt struct {
	lines []string
	open  bool
}

func (e *errorExcerpt) offer(line string) {
	if len(e.lines) == 0 {
		if errorLinePattern.MatchString(line) {
			e.lines = []string{strings.TrimSpace(line)}
			e.open = true
		}
		return
	}
	if e.open && len(e.lines) < 4 && strings.HasPrefix(line, "  ") && strings.TrimSpace(line) != "" {
		e.lines = append(e.lines, strings.TrimSpace(line))
		return
	}
	e.open = false
}

func (e *errorExcerpt) String() string {
	return strings.Join(e.lines, "\n")
}

type commandError struct {
	program    string
	err        error
	firstError string
}

func (e *commandError) Error() string {
	msg := e.program + " failed: " + e.err.Error()
	var exitErr *exec.ExitError
	if errors.As(e.err, &exitErr) {
		if code := uint32(exitErr.ExitCode()); code == statusDLLNotFound {
			msg = fmt.Sprintf("%s could not start (exit code 0x%08X): a DLL it needs is missing", e.program, code)
		} else {
			msg = fmt.Sprintf("%s failed with exit code %d", e.program, exitErr.ExitCode())
		}
	}
	if e.firstError != "" {
		msg += "\nFirst error:\n" + e.firstError
	}
	return msg
}

func (e *commandError) Unwrap() error {
	return e.err
}

// runCmdWithLog runs cmd, sends every output line to log and returns an error
// naming the program and its first error line when it fails.
func runCmdWithLog(cmd *exec.Cmd, log logFunc) error {
	program := strings.TrimSuffix(filepath.Base(cmd.Path), ".exe")
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return &commandError{program: program, err: err}
	}

	// Read both pipes to the end before Wait closes them, or the last lines
	// (usually the error) are lost.
	excerpts := make([]errorExcerpt, 2)
	var wg sync.WaitGroup
	for i, pipe := range []io.Reader{stderr, stdout} {
		wg.Add(1)
		go func(r io.Reader, excerpt *errorExcerpt) {
			defer wg.Done()
			scanner := bufio.NewScanner(r)
			scanner.Buffer(make([]byte, 64*1024), 1024*1024)
			for scanner.Scan() {
				excerpt.offer(scanner.Text())
				log("%s", scanner.Text())
			}
		}(pipe, &excerpts[i])
	}
	wg.Wait()

	if err := cmd.Wait(); err != nil {
		cmdErr := &commandError{program: program, err: err}
		for _, excerpt := range excerpts {
			if cmdErr.firstError == "" {
				cmdErr.firstError = excerpt.String()
			}
		}
		return cmdErr
	}
	return nil
}

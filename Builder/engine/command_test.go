package engine

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"sync"
	"testing"
)

// TestHelperProcess stands in for a failing CMake run when started by
// TestRunCmdWithLogReportsTheFirstError.
func TestHelperProcess(t *testing.T) {
	if os.Getenv("SPLATPC_HELPER_PROCESS") != "1" {
		return
	}
	for i := 0; i < 2000; i++ {
		fmt.Printf("line %d\n", i)
	}
	fmt.Fprintln(os.Stderr, "CMake Error at CMakeLists.txt:33 (find_package):")
	fmt.Fprintln(os.Stderr, "  Could NOT find ZLIB (missing: ZLIB_LIBRARY ZLIB_INCLUDE_DIR)")
	fmt.Fprintln(os.Stderr, "Call Stack (most recent call first):")
	os.Exit(1)
}

func TestRunCmdWithLogReportsTheFirstError(t *testing.T) {
	cmd := exec.Command(os.Args[0], "-test.run=^TestHelperProcess$")
	cmd.Env = append(os.Environ(), "SPLATPC_HELPER_PROCESS=1")

	var mu sync.Mutex
	var lines []string
	err := runCmdWithLog(cmd, func(format string, args ...interface{}) {
		mu.Lock()
		defer mu.Unlock()
		lines = append(lines, fmt.Sprintf(format, args...))
	})

	var cmdErr *commandError
	if !errors.As(err, &cmdErr) {
		t.Fatalf("expected a commandError, got %v", err)
	}
	for _, want := range []string{"exit code 1", "CMake Error at CMakeLists.txt:33", "Could NOT find ZLIB"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("expected %q in the error, got:\n%s", want, err)
		}
	}
	if strings.Contains(err.Error(), "Call Stack") {
		t.Errorf("expected the excerpt to stop before the call stack, got:\n%s", err)
	}
	// Every line must be logged, the last ones included.
	for _, want := range []string{"line 1999", "Call Stack (most recent call first):"} {
		found := false
		for _, line := range lines {
			found = found || line == want
		}
		if !found {
			t.Errorf("expected %q in the log", want)
		}
	}
}

func TestRunCmdWithLogNamesAMissingProgram(t *testing.T) {
	err := runCmdWithLog(exec.Command("splatpc-no-such-program"), func(string, ...interface{}) {})
	if err == nil || !strings.Contains(err.Error(), "splatpc-no-such-program") {
		t.Errorf("expected an error naming the program, got %v", err)
	}
}

func TestErrorExcerptMatchesCompilerAndLinkerErrors(t *testing.T) {
	cases := map[string]string{
		"msvc":    `  game.cpp(12,5): error C2065: "foo": nichtdeklarierter Bezeichner`,
		"german":  `Gambit.lib(part0001.obj) : Fehler LNK2038: Konflikt ermittelt für "_ITERATOR_DEBUG_LEVEL"`,
		"linker":  `LINK : fatal error LNK1319: 80 Konflikte erkannt`,
		"gcc":     `game.cpp:12:5: error: 'foo' was not declared in this scope`,
		"ninja":   `FAILED: CMakeFiles/Gambit.dir/game.cpp.o`,
		"cmake":   `CMake Error at gx2_graphics_backend.cmake:7 (find_package):`,
		"missing": `fatal error: zlib.h: No such file or directory`,
	}
	for name, line := range cases {
		var excerpt errorExcerpt
		excerpt.offer("-- Configuring done")
		excerpt.offer(line)
		if excerpt.String() != strings.TrimSpace(line) {
			t.Errorf("%s: expected %q, got %q", name, strings.TrimSpace(line), excerpt.String())
		}
	}
}

func TestErrorExcerptIgnoresSummaries(t *testing.T) {
	var excerpt errorExcerpt
	for _, line := range []string{"    0 Warning(s)", "    0 Error(s)", "-- Looking for strerror_r", "Build succeeded."} {
		excerpt.offer(line)
	}
	if excerpt.String() != "" {
		t.Errorf("expected no excerpt, got %q", excerpt.String())
	}
}

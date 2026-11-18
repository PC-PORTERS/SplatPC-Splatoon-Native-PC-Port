package ui

import (
	"fmt"
	"strings"
	"testing"
	"time"

	"fyne.io/fyne/v2/test"
)

func TestBuildLogKeepsUpWithAFastCompiler(t *testing.T) {
	test.NewApp()
	v := NewStepBuildingView(&Wizard{})

	// A compiler that prints much faster than any window can redraw.
	start := time.Now()
	for i := 0; i < 50000; i++ {
		v.AppendLog(fmt.Sprintf("Gambit_part%04d.cpp", i))
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Errorf("storing 50000 log lines took %s; the build must never wait for the window", elapsed)
	}

	if got := len(v.logLines); got != maxLogLines {
		t.Errorf("Copy Logs keeps %d lines, expected %d", got, maxLogLines)
	}

	v.showNewLines()
	shown := strings.Split(v.logBox.Text, "\n")
	if len(shown) != shownLogLines {
		t.Fatalf("window shows %d lines, expected the newest %d", len(shown), shownLogLines)
	}
	if last := shown[len(shown)-1]; last != "Gambit_part49999.cpp" {
		t.Errorf("last shown line = %q, expected the newest one", last)
	}
}

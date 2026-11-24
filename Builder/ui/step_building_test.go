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

func TestStepBuildingViewCancelAndRetryStates(t *testing.T) {
	test.NewApp()
	v := NewStepBuildingView(&Wizard{})
	defer v.StopTimer()

	v.Reset()
	if !v.cancelBtn.Visible() || v.cancelBtn.Disabled() || v.cancelBtn.Text != "Cancel Build" {
		t.Errorf("expected enabled 'Cancel Build' button after Reset")
	}
	if v.retryBtn.Visible() || v.backBtn.Visible() {
		t.Errorf("expected Retry/Back buttons hidden during build")
	}

	v.SetCancelling()
	if !v.cancelBtn.Disabled() || v.cancelBtn.Text != "Cancelling..." {
		t.Errorf("expected disabled 'Cancelling...' button after SetCancelling")
	}

	v.SetFailed(fmt.Errorf("cmake failed\nfirst error line"))
	if v.cancelBtn.Visible() {
		t.Errorf("expected Cancel button hidden after failure")
	}
	if !v.retryBtn.Visible() || !v.backBtn.Visible() {
		t.Errorf("expected Retry Build and Back to Config buttons visible after failure")
	}
	if v.stepTitle.Text != "Build Failed" || v.stepDesc.Text != "cmake failed" {
		t.Errorf("unexpected failed header: title=%q desc=%q", v.stepTitle.Text, v.stepDesc.Text)
	}

	v.Reset()
	if !v.cancelBtn.Visible() || v.cancelBtn.Disabled() || v.retryBtn.Visible() || v.backBtn.Visible() {
		t.Errorf("expected Reset on retry to restore Cancel Build and hide Retry/Back")
	}
}

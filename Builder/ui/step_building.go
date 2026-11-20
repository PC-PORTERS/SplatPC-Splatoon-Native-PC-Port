package ui

import (
	"fmt"
	"strings"
	"sync"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	"splatpc-builder/engine"
)

type StepBuildingView struct {
	wizard *Wizard

	container    *fyne.Container
	stepTitle    *widget.Label
	stepDesc     *widget.Label
	progressBar  *widget.ProgressBar
	percentLabel *widget.Label

	logBox     *widget.Entry
	cancelBtn  *widget.Button
	copyLogBtn *widget.Button
	timerLabel *widget.Label

	// The build goroutine appends to logLines the window picks up the newest
	// lines a few times a second.
	mu         sync.Mutex
	logLines   []string
	logChanged bool

	startTime time.Time
	stop      chan struct{}
}

// maxLogLines is how much of the log Copy Logs keeps build.log has all of it.
const maxLogLines = 5000

// shownLogLines is how much of the log the text box shows, wrapping the text
// gets slow quickly (about 0.06 s for 500 lines, 0.9 s for 2000).
const shownLogLines = 300
const logRefresh = 250 * time.Millisecond

func NewStepBuildingView(w *Wizard) *StepBuildingView {
	v := &StepBuildingView{wizard: w}
	v.initUI()
	return v
}

func (v *StepBuildingView) initUI() {
	v.stepTitle = widget.NewLabelWithStyle("Step 5: Compiling Splatoon...", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	v.stepDesc = widget.NewLabel("Initializing compilation processes...")

	v.progressBar = widget.NewProgressBar()
	v.progressBar.SetValue(0.0)

	v.percentLabel = widget.NewLabelWithStyle("0%", fyne.TextAlignTrailing, fyne.TextStyle{Bold: true})

	v.timerLabel = widget.NewLabelWithStyle("Elapsed Time: 00:00", fyne.TextAlignLeading, fyne.TextStyle{Monospace: true})

	v.logBox = widget.NewMultiLineEntry()
	v.logBox.TextStyle = fyne.TextStyle{Monospace: true}
	v.logBox.Wrapping = fyne.TextWrapWord

	v.copyLogBtn = widget.NewButtonWithIcon("Copy Logs", theme.ContentCopyIcon(), func() {
		v.mu.Lock()
		text := strings.Join(v.logLines, "\n")
		v.mu.Unlock()
		v.wizard.window.Clipboard().SetContent(text)
		dialog.ShowInformation("Logs Copied", "The complete log output was copied to the clipboard.", v.wizard.window)
	})

	v.cancelBtn = widget.NewButtonWithIcon("Cancel Build", theme.CancelIcon(), func() {
		dialog.ShowConfirm("Confirmation", "Are you sure you want to cancel the build in progress?", func(confirm bool) {
			if confirm {
				v.wizard.CancelBuild()
			}
		}, v.wizard.window)
	})
	v.cancelBtn.Importance = widget.DangerImportance

	progressRow := container.NewBorder(
		nil, nil,
		nil, v.percentLabel,
		v.progressBar,
	)

	topSection := container.NewVBox(
		v.stepTitle,
		v.stepDesc,
		progressRow,
		container.NewBorder(nil, nil, v.timerLabel, nil),
		widget.NewSeparator(),
	)

	bottomBar := container.NewBorder(
		nil, nil,
		v.cancelBtn,
		v.copyLogBtn,
	)

	v.container = container.NewBorder(
		topSection,
		bottomBar,
		nil,
		nil,
		v.logBox,
	)
}

// Reset starts a new build: empty log, timer from zero. Runs on the UI thread.
func (v *StepBuildingView) Reset() {
	v.StopTimer()
	v.mu.Lock()
	v.logLines = nil
	v.logChanged = false
	v.startTime = time.Now()
	v.mu.Unlock()
	v.logBox.SetText("")
	v.progressBar.SetValue(0.0)
	v.percentLabel.SetText("0%")
	v.stepTitle.SetText("Step 5: Compiling Splatoon")
	v.stepDesc.SetText("Starting build pipeline...")

	stop := make(chan struct{})
	v.stop = stop
	go func() {
		logTick := time.NewTicker(logRefresh)
		clockTick := time.NewTicker(time.Second)
		defer logTick.Stop()
		defer clockTick.Stop()
		for {
			select {
			case <-logTick.C:
				fyne.Do(v.showNewLines)
			case <-clockTick.C:
				elapsed := time.Since(v.startTime)
				fyne.Do(func() {
					v.timerLabel.SetText(fmt.Sprintf("Elapsed Time: %02d:%02d", int(elapsed.Minutes()), int(elapsed.Seconds())%60))
				})
			case <-stop:
				return
			}
		}
	}()
}

// StopTimer ends the build page updates and shows the last log lines. Runs on
// the UI thread.
func (v *StepBuildingView) StopTimer() {
	if v.stop != nil {
		close(v.stop)
		v.stop = nil
	}
	v.showNewLines()
}

// AppendLog is called by the build for every line it prints; it only stores
// the line, the window picks new lines up a few times a second.
func (v *StepBuildingView) AppendLog(line string) {
	v.mu.Lock()
	v.logLines = append(v.logLines, line)
	if len(v.logLines) > maxLogLines {
		v.logLines = v.logLines[len(v.logLines)-maxLogLines:]
	}
	v.logChanged = true
	v.mu.Unlock()
}

// showNewLines puts the lines that arrived since the last call on screen.
func (v *StepBuildingView) showNewLines() {
	v.mu.Lock()
	if !v.logChanged {
		v.mu.Unlock()
		return
	}
	newest := v.logLines[max(0, len(v.logLines)-shownLogLines):]
	text := strings.Join(newest, "\n")
	v.logChanged = false
	v.mu.Unlock()
	v.logBox.SetText(text)
	v.logBox.CursorRow = len(newest)
}

func (v *StepBuildingView) UpdateProgress(step engine.StepInfo) {
	fyne.Do(func() {
		v.stepTitle.SetText(fmt.Sprintf("[%d/%d] %s", step.Index, step.Total, step.Title))
		v.stepDesc.SetText(step.Description)
		v.progressBar.SetValue(step.Progress)
		v.percentLabel.SetText(fmt.Sprintf("%d%%", int(step.Progress*100)))
	})
}

func (v *StepBuildingView) Container() fyne.CanvasObject {
	return v.container
}

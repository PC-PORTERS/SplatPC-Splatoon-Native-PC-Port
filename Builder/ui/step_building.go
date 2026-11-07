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

	mu        sync.Mutex
	logLines  []string
	startTime time.Time
	ticker    *time.Ticker
	stopTimer chan struct{}
}

func NewStepBuildingView(w *Wizard) *StepBuildingView {
	v := &StepBuildingView{
		wizard:    w,
		logLines:  make([]string, 0),
		stopTimer: make(chan struct{}),
	}
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

func (v *StepBuildingView) Reset() {
	v.mu.Lock()
	v.logLines = nil
	v.logBox.SetText("")
	v.progressBar.SetValue(0.0)
	v.percentLabel.SetText("0%")
	v.stepTitle.SetText("Step 5: Compiling Splatoon")
	v.stepDesc.SetText("Starting build pipeline...")
	v.startTime = time.Now()
	v.mu.Unlock()

	if v.ticker != nil {
		v.ticker.Stop()
	}
	v.ticker = time.NewTicker(time.Second)
	go func() {
		for {
			select {
			case <-v.ticker.C:
				elapsed := time.Since(v.startTime)
				mins := int(elapsed.Minutes())
				secs := int(elapsed.Seconds()) % 60
				fyne.Do(func() {
					v.timerLabel.SetText(fmt.Sprintf("Elapsed Time: %02d:%02d", mins, secs))
				})
			case <-v.stopTimer:
				return
			}
		}
	}()
}

func (v *StepBuildingView) StopTimer() {
	if v.ticker != nil {
		v.ticker.Stop()
	}
}

func (v *StepBuildingView) AppendLog(line string) {
	v.mu.Lock()
	v.logLines = append(v.logLines, line)
	// Retain up to 2000 lines for high performance rendering
	if len(v.logLines) > 2000 {
		v.logLines = v.logLines[len(v.logLines)-2000:]
	}
	fullText := strings.Join(v.logLines, "\n")
	row := len(v.logLines)
	v.mu.Unlock()

	fyne.Do(func() {
		v.logBox.SetText(fullText)
		v.logBox.CursorRow = row
	})
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

package engine

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sync"
)

// buildLog sends each line to the UI and to build/build.log, so a failed
// build can be shared without copying the console by hand.
type buildLog struct {
	mu   sync.Mutex
	file *os.File
	sink func(string)
}

func openBuildLog(path string, sink func(string)) *buildLog {
	l := &buildLog{sink: sink}
	if err := os.MkdirAll(filepath.Dir(path), 0755); err == nil {
		l.file, _ = os.Create(path)
	}
	return l
}

func (l *buildLog) printf(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.file != nil {
		fmt.Fprintln(l.file, msg)
	}
	if l.sink != nil {
		l.sink(msg)
	}
}

func (l *buildLog) close() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.file != nil {
		_ = l.file.Close()
		l.file = nil
	}
}

// cleanBuildDir empties buildDir but keeps the downloaded archives, which
// never change, so --clean does not download everything again.
func cleanBuildDir(buildDir string) error {
	entries, err := os.ReadDir(buildDir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	keep := []string{"downloads"}
	for _, entry := range entries {
		if slices.Contains(keep, entry.Name()) {
			continue
		}
		if err := os.RemoveAll(filepath.Join(buildDir, entry.Name())); err != nil {
			return err
		}
	}
	return nil
}

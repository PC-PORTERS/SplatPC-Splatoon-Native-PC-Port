package settings

import (
	"os"
	"path/filepath"
	"testing"
)

func TestDefaultOnlineServerReadsServerFiles(t *testing.T) {
	root := t.TempDir()
	scripts := filepath.Join(root, "scripts")
	if err := os.MkdirAll(scripts, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(scripts, "server.public.txt"),
		[]byte("server=192.0.2.10:60000\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := DefaultOnlineServer(root); got != "192.0.2.10:60000" {
		t.Fatalf("expected 192.0.2.10:60000, got %q", got)
	}

	// Local scripts/server.txt overrides server.public.txt
	if err := os.WriteFile(filepath.Join(scripts, "server.txt"),
		[]byte("server=203.0.113.5:60000\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := DefaultOnlineServer(root); got != "203.0.113.5:60000" {
		t.Fatalf("expected server.txt override 203.0.113.5:60000, got %q", got)
	}
}

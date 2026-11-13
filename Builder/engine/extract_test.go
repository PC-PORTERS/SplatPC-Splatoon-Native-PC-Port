package engine

import (
	"archive/tar"
	"compress/gzip"
	"os"
	"path/filepath"
	"testing"
)

func TestExtractTarGz(t *testing.T) {
	tempDir := t.TempDir()
	tarGzPath := filepath.Join(tempDir, "test.tar.gz")
	destDir := filepath.Join(tempDir, "extracted")

	// Create test .tar.gz with a top-level dir to be stripped
	f, err := os.Create(tarGzPath)
	if err != nil {
		t.Fatalf("Failed to create tar.gz: %v", err)
	}
	gw := gzip.NewWriter(f)
	tw := tar.NewWriter(gw)

	files := []struct {
		Name string
		Body string
	}{
		{"volk-1.4/CMakeLists.txt", "cmake_minimum_required(VERSION 3.10)\n"},
		{"volk-1.4/include/volk.h", "#pragma once\n"},
	}

	for _, file := range files {
		hdr := &tar.Header{
			Name: file.Name,
			Mode: 0644,
			Size: int64(len(file.Body)),
		}
		if err := tw.WriteHeader(hdr); err != nil {
			t.Fatalf("Failed to write tar header: %v", err)
		}
		if _, err := tw.Write([]byte(file.Body)); err != nil {
			t.Fatalf("Failed to write tar body: %v", err)
		}
	}

	if err := tw.Close(); err != nil {
		t.Fatalf("Failed to close tar: %v", err)
	}
	if err := gw.Close(); err != nil {
		t.Fatalf("Failed to close gzip: %v", err)
	}
	if err := f.Close(); err != nil {
		t.Fatalf("Failed to close file: %v", err)
	}

	// Extract with stripComponents = 1
	if err := ExtractTarGz(tarGzPath, destDir, 1); err != nil {
		t.Fatalf("ExtractTarGz failed: %v", err)
	}

	// Verify top-level CMakeLists.txt and include/volk.h exist in destDir
	cmakeFile := filepath.Join(destDir, "CMakeLists.txt")
	if !fileExists(cmakeFile) {
		t.Errorf("Expected %s to exist", cmakeFile)
	}

	volkHeader := filepath.Join(destDir, "include", "volk.h")
	if !fileExists(volkHeader) {
		t.Errorf("Expected %s to exist", volkHeader)
	}
}

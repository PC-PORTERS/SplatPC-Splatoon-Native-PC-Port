package engine

import (
	"archive/tar"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// ExtractTarGz extracts a .tar.gz archive into destDir in pure Go without relying
// on system `tar` utilities, ensuring seamless operation across Windows and Linux.
func ExtractTarGz(tarGzPath, destDir string, stripComponents int) error {
	f, err := os.Open(tarGzPath)
	if err != nil {
		return fmt.Errorf("opening tar archive: %w", err)
	}
	defer f.Close()

	gzr, err := gzip.NewReader(f)
	if err != nil {
		return fmt.Errorf("initializing gzip reader: %w", err)
	}
	defer gzr.Close()

	tr := tar.NewReader(gzr)
	for {
		header, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return fmt.Errorf("reading tar entry: %w", err)
		}

		parts := strings.Split(filepath.ToSlash(header.Name), "/")
		if len(parts) <= stripComponents {
			continue
		}
		relPath := strings.Join(parts[stripComponents:], "/")
		if relPath == "" || relPath == "." {
			continue
		}

		target := filepath.Join(destDir, filepath.FromSlash(relPath))
		switch header.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0755); err != nil {
				return fmt.Errorf("creating directory %s: %w", target, err)
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return fmt.Errorf("creating parent dir for %s: %w", target, err)
			}
			outFile, err := os.OpenFile(target, os.O_CREATE|os.O_RDWR|os.O_TRUNC, header.FileInfo().Mode())
			if err != nil {
				return fmt.Errorf("creating file %s: %w", target, err)
			}
			if _, err := io.Copy(outFile, tr); err != nil {
				outFile.Close()
				return fmt.Errorf("writing file %s: %w", target, err)
			}
			outFile.Close()
		}
	}
	return nil
}

func fileExists(p string) bool {
	s, err := os.Stat(p)
	return err == nil && (s.IsDir() || s.Mode().IsRegular())
}


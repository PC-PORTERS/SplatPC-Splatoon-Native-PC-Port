package engine

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
)

var fontNames = []string{"CafeCn.ttf", "CafeKr.ttf", "CafeStd.ttf", "CafeTw.ttf"}
var fontSha256 = []string{
	"6afd751a9bb76bc788efb50a138c6f3765624ee84d8d8203be04d871ade40a6a",
	"c3c326f6ce1899e391dc4996b10a0c30558c5b8f36dfbaa418976862b3e2dffb",
	"59e2494727b8e7a7bbf48c8749f2dbc07d438c9fabeaa9847a08ec690f9888a1",
	"c8f9e16345dbc3b6e165fe12435486a748e730ebe6002b5d713efe73d171d51c",
}

const fontBaseURL = "https://raw.githubusercontent.com/cemu-project/Cemu/cc6fea125a5d4f559c662d13c5f4af0b6dd0876f/bin/resources/sharedFonts"

func InitializeWiiUFolders(gameFolder string) error {
	subfolders := []string{
		"save/common",
		"mlc01/sys",
		"mlc01/usr/title/00050000",
		"mlc01/usr/title/0005000c",
		"mlc01/usr/title/0005000e",
		"mlc01/usr/save/00050000",
		"mlc01/usr/boss",
		"mlc01/usr/save/00050010/1004a000/user/common/db",
		"mlc01/usr/save/00050010/1004a100/user/common/db",
		"mlc01/usr/save/00050010/1004a200/user/common/db",
		"mlc01/sys/title/0005001b/10042400/content",
		"mlc01/sys/title/0005001b/10056000/content",
		"mlc01/sys/title/0005001b/1005c000/content",
	}

	for _, sub := range subfolders {
		if err := os.MkdirAll(filepath.Join(gameFolder, sub), 0755); err != nil {
			return err
		}
	}
	return nil
}

func PrepareWiiUResources(gameFolder, repoRoot, cacheFolder string, log func(string, ...interface{})) error {
	if err := InitializeWiiUFolders(gameFolder); err != nil {
		return fmt.Errorf("initializing Wii U folders: %w", err)
	}

	fontDest := filepath.Join(gameFolder, "mlc01/sys/title/0005001b/10042400/content")
	if err := os.MkdirAll(fontDest, 0755); err != nil {
		return err
	}

	// Download fonts to cache folder if not present
	fontCache := filepath.Join(cacheFolder, "shared-fonts")
	if err := os.MkdirAll(fontCache, 0755); err != nil {
		return err
	}

	for i, name := range fontNames {
		destFile := filepath.Join(fontCache, name)
		valid := false
		if _, err := os.Stat(destFile); err == nil {
			if h, err := fileHash(destFile); err == nil && h == fontSha256[i] {
				valid = true
			}
		}

		if !valid {
			log("Downloading Cemu replacement font: %s", name)
			url := fmt.Sprintf("%s/%s", fontBaseURL, name)
			if err := downloadFile(url, destFile); err != nil {
				return fmt.Errorf("error downloading %s: %w", name, err)
			}
			h, err := fileHash(destFile)
			if err != nil || h != fontSha256[i] {
				_ = os.Remove(destFile)
				return fmt.Errorf("integrity check failed for font %s", name)
			}
		}

		// Copy to game folder
		if err := copySingleFile(destFile, filepath.Join(fontDest, name)); err != nil {
			return err
		}
	}

	// Copy OFL license
	licenseSrc := filepath.Join(repoRoot, "LICENSES/cemu-shared-fonts-OFL-1.1.txt")
	licenseDestDir := filepath.Join(gameFolder, "licenses")
	_ = os.MkdirAll(licenseDestDir, 0755)
	if _, err := os.Stat(licenseSrc); err == nil {
		_ = copySingleFile(licenseSrc, filepath.Join(licenseDestDir, "cemu-shared-fonts-OFL-1.1.txt"))
	}

	log("Replacement fonts and Wii U folder structure ready.")
	return nil
}

func downloadFile(url, dest string) error {
	resp, err := http.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP error %s", resp.Status)
	}

	tmpFile := dest + ".part"
	out, err := os.Create(tmpFile)
	if err != nil {
		return err
	}

	if _, err := io.Copy(out, resp.Body); err != nil {
		out.Close()
		_ = os.Remove(tmpFile)
		return err
	}
	out.Close()
	return os.Rename(tmpFile, dest)
}

func fileHash(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func copySingleFile(src, dst string) error {
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}

	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()

	_, err = io.Copy(out, in)
	return err
}

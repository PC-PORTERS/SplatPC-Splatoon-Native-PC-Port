package roms

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type DumpInfo struct {
	Path         string
	TitleVersion string
	RPXHash      string
	ProductCode  string
	LongTitle    string
	IsValidDump  bool
	HasRPX       bool
	IsV16        bool
	IsV288       bool
	ErrorMessage string
}

type metaXml struct {
	XMLName      xml.Name `xml:"menu"`
	TitleVersion string   `xml:"title_version"`
	ProductCode  string   `xml:"product_code"`
	LongNameEn   string   `xml:"longname_en"`
	LongNameFr   string   `xml:"longname_fr"`
}

const (
	V288_SHA256 = "06684edefe90fea2de20430eb0e68500995980fef154caa88e4dcb59cdb7cd65"
)

func ValidateDump(dumpPath string) DumpInfo {
	info := DumpInfo{
		Path: dumpPath,
	}

	stat, err := os.Stat(dumpPath)
	if err != nil || !stat.IsDir() {
		info.ErrorMessage = fmt.Sprintf("Folder does not exist: %s", dumpPath)
		return info
	}

	// Check required directories
	for _, folder := range []string{"code", "content", "meta"} {
		sub := filepath.Join(dumpPath, folder)
		if s, err := os.Stat(sub); err != nil || !s.IsDir() {
			info.ErrorMessage = fmt.Sprintf("Missing '%s' folder in %s", folder, dumpPath)
			return info
		}
	}

	rpxFile := filepath.Join(dumpPath, "code", "Gambit.rpx")
	if s, err := os.Stat(rpxFile); err == nil && !s.IsDir() {
		info.HasRPX = true
		hash, err := computeSHA256(rpxFile)
		if err == nil {
			info.RPXHash = hash
		}
	}

	metaFile := filepath.Join(dumpPath, "meta", "meta.xml")
	if data, err := os.ReadFile(metaFile); err == nil {
		var meta metaXml
		if err := xml.Unmarshal(data, &meta); err == nil {
			info.TitleVersion = strings.TrimSpace(meta.TitleVersion)
			info.ProductCode = strings.TrimSpace(meta.ProductCode)
			if meta.LongNameEn != "" {
				info.LongTitle = meta.LongNameEn
			} else {
				info.LongTitle = meta.LongNameFr
			}
		}
	}

	if info.TitleVersion == "16" || info.TitleVersion == "v16" {
		info.IsV16 = true
	}
	if info.TitleVersion == "288" || info.TitleVersion == "v288" || strings.EqualFold(info.RPXHash, V288_SHA256) {
		info.IsV288 = true
	}

	info.IsValidDump = true
	return info
}

func computeSHA256(filePath string) (string, error) {
	file, err := os.Open(filePath)
	if err != nil {
		return "", err
	}
	defer file.Close()

	hasher := sha256.New()
	if _, err := io.Copy(hasher, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(hasher.Sum(nil)), nil
}

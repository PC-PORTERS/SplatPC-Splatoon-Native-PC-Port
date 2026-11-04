package roms

import (
	"fmt"
	"path/filepath"
)

type RomLayer struct {
	ID             string
	Path           string
	Name           string
	TitleVersion   string
	RPXHash        string
	IsBase         bool
	IsV288         bool
	CanReplaceRPX  bool
	HasRPX         bool
	ModifiesRPX    bool
	WarningMessage string
}

func NewLayerFromPath(path string) (RomLayer, error) {
	info := ValidateDump(path)
	if !info.IsValidDump {
		return RomLayer{}, fmt.Errorf("invalid folder: %s (%s)", path, info.ErrorMessage)
	}

	name := filepath.Base(path)
	layer := RomLayer{
		ID:           path,
		Path:         path,
		Name:         name,
		TitleVersion: info.TitleVersion,
		RPXHash:      info.RPXHash,
		HasRPX:       info.HasRPX,
		IsV288:       info.IsV288,
	}

	if info.IsV288 {
		layer.Name = fmt.Sprintf("Update v288 (%s)", name)
		layer.CanReplaceRPX = true
		layer.ModifiesRPX = true
	} else if info.HasRPX && !info.IsV16 {
		layer.CanReplaceRPX = false
		layer.ModifiesRPX = false
		layer.WarningMessage = "Only official update v288 can replace the built .rpx executable. Mod content/meta assets will be applied, but this .rpx will be skipped."
	}

	return layer, nil
}

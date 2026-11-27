package settings

import (
	"bufio"
	"errors"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// Up to v1.0.11 an unpacked AppImage put the game beside its program, in
// APPDIR/usr/bin/Game. The first start that uses APPDIR/Game copies what the
// player made there (saves, settings, online identity, screenshots, mods,
// textures) and leaves the originals where they are. The built game is not
// copied: the setup step builds it again.

const (
	copiedMarker = ".copied-from-program-folder"
	copyJournal  = ".copying-from-program-folder"
)

// builtItems are what the setup step writes from the dump and the build.
var builtItems = map[string]bool{
	"Splatoon":                 true,
	"content":                  true,
	"meta":                     true,
	"vulkan_cache":             true,
	"Splatoon-x86_64.AppImage": true,
}

// beforeItem lets tests stop a copy partway.
var beforeItem = func(name string) error { return nil }

// unpackedGameDir returns dir once everything from old is copied into it, and
// old for this start while the copy has not finished.
func unpackedGameDir(dir, old string) string {
	if filepath.Clean(dir) == filepath.Clean(old) {
		return dir
	}
	if copyFromProgramFolder(old, dir, stateDir(dir)) {
		return dir
	}
	return old
}

func stateDir(dir string) string {
	if state := os.Getenv("RECOMPLAUNCHER_DATA"); state != "" {
		return state
	}
	return dir
}

// RemoveOldBuild deletes the built game left in the old folder once the
// player's items were copied and a build into dir has finished. The player's
// items stay there as they are.
func RemoveOldBuild(repoRoot, dir string, log func(string, ...interface{})) {
	appDir := os.Getenv("APPDIR")
	if runtime.GOOS != "linux" || os.Getenv("APPIMAGE") != "" || appDir == "" {
		return
	}
	if filepath.Clean(dir) != filepath.Clean(filepath.Join(appDir, "Game")) {
		return
	}
	removeOldBuild(besideProgram(repoRoot), dir, stateDir(dir), log)
}

func removeOldBuild(old, dir, state string, log func(string, ...interface{})) {
	if filepath.Clean(dir) == filepath.Clean(old) {
		return
	}
	if _, err := os.Stat(filepath.Join(state, copiedMarker)); err != nil {
		return
	}
	if _, err := os.Stat(filepath.Join(dir, "Splatoon")); err != nil {
		return
	}
	// The build also left its AppImage beside the old folder.
	paths := []string{filepath.Join(filepath.Dir(old), "Splatoon-x86_64.AppImage")}
	for name := range builtItems {
		paths = append(paths, filepath.Join(old, name))
	}
	for _, p := range paths {
		if _, err := os.Lstat(p); err != nil {
			continue
		}
		if err := os.RemoveAll(p); err != nil {
			log("Could not remove the old build's %s: %v", p, err)
			continue
		}
		log("Removed the old build's %s", p)
	}
}

// copyFromProgramFolder copies old into dir once and reports whether the
// marker in state says it is done. Each item goes to <name>.part and is
// renamed into place, never over an existing one (settings.ini excepted: the
// player's own replaces the launcher's on this first start). Every name is
// journaled just before its rename, so a copy that fails, or is cut off, has
// exactly those taken back before the next try.
func copyFromProgramFolder(old, dir, state string) bool {
	marker := filepath.Join(state, copiedMarker)
	if _, err := os.Stat(marker); err == nil {
		return true
	}
	journal := filepath.Join(state, copyJournal)
	if err := os.MkdirAll(state, 0o755); err != nil {
		return false
	}
	if err := takeBack(dir, journal); err != nil {
		return false
	}
	entries, err := os.ReadDir(old)
	if err != nil && !errors.Is(err, fs.ErrNotExist) {
		return false
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return false
	}
	for _, e := range entries {
		if err := os.RemoveAll(filepath.Join(dir, e.Name()+".part")); err != nil {
			return false
		}
	}
	for _, e := range entries {
		name := e.Name()
		if builtItems[name] || name == copiedMarker || name == copyJournal {
			continue
		}
		dst := filepath.Join(dir, name)
		if name != "settings.ini" {
			if _, err := os.Lstat(dst); err == nil {
				continue
			}
		}
		if err := copyItem(filepath.Join(old, name), dst, journal); err != nil {
			_ = os.RemoveAll(dst + ".part")
			_ = takeBack(dir, journal)
			return false
		}
	}
	if err := writeSynced(marker, []byte(old+"\n"), false); err != nil {
		_ = os.Remove(marker)
		_ = takeBack(dir, journal)
		return false
	}
	_ = os.Remove(journal)
	return true
}

func copyItem(src, dst, journal string) error {
	name := filepath.Base(dst)
	if err := beforeItem(name); err != nil {
		return err
	}
	part := dst + ".part"
	if err := copyTree(src, part); err != nil {
		return err
	}
	if err := writeSynced(journal, []byte(name+"\n"), true); err != nil {
		return err
	}
	return os.Rename(part, dst)
}

// takeBack removes the items the journal names from dir, then the journal.
func takeBack(dir, journal string) error {
	f, err := os.Open(journal)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var names []string
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		names = append(names, scanner.Text())
	}
	readErr := scanner.Err()
	f.Close()
	if readErr != nil {
		return readErr
	}
	for _, name := range names {
		if name == "" || name == "." || name == ".." || strings.ContainsAny(name, `/\`) {
			continue
		}
		if err := os.RemoveAll(filepath.Join(dir, name)); err != nil {
			return err
		}
	}
	return os.Remove(journal)
}

func writeSynced(path string, data []byte, appendTo bool) error {
	flags := os.O_WRONLY | os.O_CREATE | os.O_TRUNC
	if appendTo {
		flags = os.O_WRONLY | os.O_CREATE | os.O_APPEND
	}
	f, err := os.OpenFile(path, flags, 0o644)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// copyTree copies a file, a link or a whole folder, keeping modes and times.
func copyTree(src, dst string) error {
	info, err := os.Lstat(src)
	if err != nil {
		return err
	}
	switch {
	case info.Mode()&fs.ModeSymlink != 0:
		target, err := os.Readlink(src)
		if err != nil {
			return err
		}
		return os.Symlink(target, dst)
	case info.IsDir():
		if err := os.Mkdir(dst, info.Mode().Perm()|0o700); err != nil {
			return err
		}
		entries, err := os.ReadDir(src)
		if err != nil {
			return err
		}
		for _, e := range entries {
			if err := copyTree(filepath.Join(src, e.Name()), filepath.Join(dst, e.Name())); err != nil {
				return err
			}
		}
	case info.Mode().IsRegular():
		if err := copyFile(src, dst, info.Mode().Perm()); err != nil {
			return err
		}
	default:
		return nil
	}
	return os.Chtimes(dst, info.ModTime(), info.ModTime())
}

func copyFile(src, dst string, perm fs.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, perm)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	if err := out.Sync(); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

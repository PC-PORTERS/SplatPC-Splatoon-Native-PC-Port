package settings

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

type unpacked struct {
	old, dir, state string
}

// newUnpacked lays out an unpacked AppImage whose old build folder holds a
// built game and what the player made.
func newUnpacked(t *testing.T) unpacked {
	t.Helper()
	root := t.TempDir()
	u := unpacked{
		old:   filepath.Join(root, "usr", "bin", "Game"),
		dir:   filepath.Join(root, "Game"),
		state: filepath.Join(root, "recompdata"),
	}
	write(t, filepath.Join(u.old, "Splatoon"), "game")
	write(t, filepath.Join(u.old, "content", "Common", "a.szs"), "content")
	write(t, filepath.Join(u.old, "meta", "meta.xml"), "meta")
	write(t, filepath.Join(u.old, "vulkan_cache", "c.bin"), "cache")
	write(t, filepath.Join(u.old, "save", "common", "save.dat"), "old save")
	write(t, filepath.Join(u.old, "settings.ini"), "player settings")
	write(t, filepath.Join(u.old, "friends", "list.txt"), "friends")
	write(t, filepath.Join(u.old, "online_session.ini"), "session")
	return u
}

func write(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

func read(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("reading %s: %v", path, err)
	}
	return string(data)
}

func exists(path string) bool {
	_, err := os.Lstat(path)
	return err == nil
}

func failOn(t *testing.T, item string) {
	t.Helper()
	beforeItem = func(name string) error {
		if name == item {
			return errors.New("cut off")
		}
		return nil
	}
	t.Cleanup(func() { beforeItem = func(string) error { return nil } })
}

func TestFreshStartUsesTheNewFolder(t *testing.T) {
	root := t.TempDir()
	dir, state := filepath.Join(root, "Game"), filepath.Join(root, "recompdata")
	if !copyFromProgramFolder(filepath.Join(root, "usr", "bin", "Game"), dir, state) {
		t.Fatal("a fresh start should finish at once")
	}
	if !exists(filepath.Join(state, copiedMarker)) || !exists(dir) {
		t.Fatal("expected the marker and the game folder")
	}
}

func TestOldDataIsCopiedButNotTheBuild(t *testing.T) {
	u := newUnpacked(t)
	if !copyFromProgramFolder(u.old, u.dir, u.state) {
		t.Fatal("copy failed")
	}
	if got := read(t, filepath.Join(u.dir, "save", "common", "save.dat")); got != "old save" {
		t.Errorf("save = %q", got)
	}
	for _, name := range []string{"settings.ini", "friends/list.txt", "online_session.ini"} {
		if !exists(filepath.Join(u.dir, name)) {
			t.Errorf("%s was not copied", name)
		}
	}
	for name := range builtItems {
		if exists(filepath.Join(u.dir, name)) {
			t.Errorf("built item %s was copied", name)
		}
	}
	if !exists(filepath.Join(u.old, "save", "common", "save.dat")) || !exists(filepath.Join(u.old, "Splatoon")) {
		t.Error("the originals must stay")
	}
	if exists(filepath.Join(u.state, copyJournal)) {
		t.Error("the journal should be gone once the marker is written")
	}
}

func TestOldSettingsWinOnlyOnTheFirstStart(t *testing.T) {
	u := newUnpacked(t)
	write(t, filepath.Join(u.dir, "settings.ini"), "launcher settings")
	write(t, filepath.Join(u.dir, "save", "common", "save.dat"), "already here")
	if !copyFromProgramFolder(u.old, u.dir, u.state) {
		t.Fatal("copy failed")
	}
	if got := read(t, filepath.Join(u.dir, "settings.ini")); got != "player settings" {
		t.Errorf("settings.ini = %q, expected the player's own", got)
	}
	if got := read(t, filepath.Join(u.dir, "save", "common", "save.dat")); got != "already here" {
		t.Errorf("an existing save was overwritten: %q", got)
	}
	write(t, filepath.Join(u.dir, "settings.ini"), "launcher settings")
	copyFromProgramFolder(u.old, u.dir, u.state)
	if got := read(t, filepath.Join(u.dir, "settings.ini")); got != "launcher settings" {
		t.Errorf("settings.ini replaced again after the copy: %q", got)
	}
}

func TestFinishedCopyIsNotRepeated(t *testing.T) {
	u := newUnpacked(t)
	if !copyFromProgramFolder(u.old, u.dir, u.state) {
		t.Fatal("copy failed")
	}
	write(t, filepath.Join(u.old, "save", "common", "save.dat"), "played in the old folder")
	if err := os.RemoveAll(filepath.Join(u.dir, "friends")); err != nil {
		t.Fatal(err)
	}
	if !copyFromProgramFolder(u.old, u.dir, u.state) {
		t.Fatal("a finished copy should stay finished")
	}
	if got := read(t, filepath.Join(u.dir, "save", "common", "save.dat")); got != "old save" {
		t.Errorf("save copied again: %q", got)
	}
	if exists(filepath.Join(u.dir, "friends")) {
		t.Error("items copied again after the marker")
	}
}

func TestFailedCopyTakesBackAndTheNextStartBringsTheNewerSave(t *testing.T) {
	u := newUnpacked(t)
	failOn(t, "settings.ini")
	if got := u.start(t); got != u.old {
		t.Fatalf("after a failed copy the game should stay in %s, got %s", u.old, got)
	}
	for _, name := range []string{"save", "friends", "online_session.ini", "save.part"} {
		if exists(filepath.Join(u.dir, name)) {
			t.Errorf("%s was not taken back", name)
		}
	}
	if exists(filepath.Join(u.state, copiedMarker)) || exists(filepath.Join(u.state, copyJournal)) {
		t.Error("no marker or journal expected after a failed copy")
	}

	beforeItem = func(string) error { return nil }
	write(t, filepath.Join(u.old, "save", "common", "save.dat"), "newer save")
	if got := u.start(t); got != u.dir {
		t.Fatalf("expected %s, got %s", u.dir, got)
	}
	if got := read(t, filepath.Join(u.dir, "save", "common", "save.dat")); got != "newer save" {
		t.Errorf("save = %q, expected the newer one", got)
	}
}

func TestInterruptedCopyFinishesOnTheNextStart(t *testing.T) {
	u := newUnpacked(t)
	// What a copy killed partway leaves: one item renamed and journaled, the
	// next one half copied, no marker.
	write(t, filepath.Join(u.dir, "friends", "list.txt"), "friends")
	write(t, filepath.Join(u.dir, "save.part", "common", "save.dat"), "half")
	write(t, filepath.Join(u.dir, "settings.ini"), "launcher settings")
	write(t, filepath.Join(u.state, copyJournal), "friends\n")
	write(t, filepath.Join(u.old, "friends", "list.txt"), "friends, played on")

	failOn(t, "save")
	if got := u.start(t); got != u.old {
		t.Fatalf("while the copy is unfinished the game should stay in %s, got %s", u.old, got)
	}
	if got := read(t, filepath.Join(u.dir, "settings.ini")); got != "launcher settings" {
		t.Errorf("an item the journal doesn't name was touched: %q", got)
	}

	beforeItem = func(string) error { return nil }
	if got := u.start(t); got != u.dir {
		t.Fatalf("expected %s, got %s", u.dir, got)
	}
	if got := read(t, filepath.Join(u.dir, "friends", "list.txt")); got != "friends, played on" {
		t.Errorf("friends = %q, expected the one from the old folder", got)
	}
	if got := read(t, filepath.Join(u.dir, "save", "common", "save.dat")); got != "old save" {
		t.Errorf("save = %q", got)
	}
	if exists(filepath.Join(u.dir, "save.part")) {
		t.Error("leftover .part not cleaned up")
	}
}

func (u unpacked) start(t *testing.T) string {
	t.Setenv("RECOMPLAUNCHER_DATA", u.state)
	return unpackedGameDir(u.dir, u.old)
}

func TestOutputDirForUnpackedAppImage(t *testing.T) {
	root := t.TempDir()
	t.Setenv("APPIMAGE", "")
	t.Setenv("APPDIR", root)
	t.Setenv("RECOMPLAUNCHER_DATA", filepath.Join(root, "recompdata"))
	want := besideProgram("/tmp/splatpc")
	if runtime.GOOS == "linux" {
		want = filepath.Join(root, "Game")
	}
	if got := OutputDirFor("/tmp/splatpc", true); got != want {
		t.Errorf("OutputDirFor = %s, expected %s", got, want)
	}
}

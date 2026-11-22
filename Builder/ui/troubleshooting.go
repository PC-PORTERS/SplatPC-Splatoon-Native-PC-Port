package ui

import (
	"path/filepath"
	"runtime"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type troubleshootingTopic struct {
	title string
	body  string // Markdown
}

// troubleshootingTopics are the problems players run into most, with their fixes
func troubleshootingTopics(logPath string) []troubleshootingTopic {
	topics := []troubleshootingTopic{
		{"The build failed", "Everything the build printed is in this file:\n\n`" + logPath + "`\n\n" +
			"**Copy Logs** on the build page copies the same output. Attach it when you open an issue on GitHib."},
		{"Tools are missing", "The **Prerequisites** page lists what is missing.\n\n" +
			"- Click **Download & Install Dependencies (100% Userspace)** to download CMake, Python, Ninja, the C++ toolchain, and AppImageTool directly from official sources into your user cache — no root or administrator password needed.\n" +
			"- After the build finishes, you can delete all downloaded dependencies and build files from the final **Play** page (or automatically via Step 3) to reclaim disk space.\n" +
			"- Alternatively, you can still use **Run in Terminal (System)** or your distribution's package manager."},
		{"\"Visual Studio is not installed\" although it is", "The Builder needs the **C++** part of Visual Studio. " +
			"Open the **Visual Studio Installer**, click **Modify** and tick **Desktop development with C++**. " +
			"**Install missing tools** does this for you when it finds a Visual Studio without C++."},
		{"CMake cannot build with Visual Studio 2026", "Visual Studio 2026 needs CMake 4.2 or newer:\n\n" +
			"`winget install --id Kitware.CMake -e`"},
		{"Python is not found", "Install Python 3.12 or newer, for example `winget install --id Python.Python.3.13 -e`.\n\n" +
			"- Windows: if typing `python` opens the Microsoft Store, that is only a shortcut. The Builder also tries `py -3`. " +
			"Turning the shortcuts off under *Settings > Apps > Advanced app settings > App execution aliases* helps other programs too.\n" +
			"- Linux: Debian 12 and Ubuntu 22.04 ship an older Python. Install a newer one or use a newer release."},
		{"\"Unsupported game version\"", "Your `code/Gambit.rpx` matches none of the supported versions. " +
			"Use a clean dump from your own console (folders `code`, `content`, `meta`). " +
			"Select the **base game (v16)** as the base ROM and add the v288 update with **Add update or mod...**, not the other way round."},
		{"The build fails with an odd path error", "Brackets, accents or other special characters in a folder name " +
			"(for example `...-main(1)` after downloading the ZIP twice) can break the C++ tools. " +
			"Move the files to a short, plain folder such as `C:\\SplatPC` and build again."},
		{"The game opens and closes right away", "- Start it from the Builder" +
			"- The game needs Vulkan 1.3: update your graphics driver (NVIDIA, AMD or Intel).\n" +
			"- Windows: a message about `MSVCP140.dll` or `VCRUNTIME140.dll` means the Visual C++ runtime is missing: " +
			"`winget install --id Microsoft.VCRedist.2015+.x64 -e`"},
		{"\"No dumped textures found\" (HD textures)", "The upscaler works on textures the game recorded while you played. " +
			"Turn on HD texturing, play for a while (plaza, Octo Valley, a few stages), close the game, then click **Upscale Dumped Textures**."},
		{"My installed game is not detected", "The Builder looks in the folder of its last successful build and in its default `Game` folder. " +
			"A game built into another folder (or from an older setup) is found once the Builder has built into that folder."},
	}
	if runtime.GOOS != "windows" {
		// The Visual Studio, winget and .bat topics only apply to Windows
		topics = append(topics[:2], topics[4:]...)
	}
	return topics
}

// showTroubleshooting opens the troubleshooting guide in a window of the Builder
func (w *Wizard) showTroubleshooting() {
	accordion := widget.NewAccordion()
	for _, t := range troubleshootingTopics(filepath.Join(w.repoRoot, "build", "build.log")) {
		body := widget.NewRichTextFromMarkdown(t.body)
		body.Wrapping = fyne.TextWrapWord
		accordion.Append(widget.NewAccordionItem(t.title, body))
	}
	intro := wrappedLabel("Click a problem to see how to fix it. If nothing helps, open an issue on GitHub and attach the build log.")
	content := container.NewBorder(intro, nil, nil, nil, container.NewVScroll(accordion))

	d := dialog.NewCustom("Troubleshooting", "Close", content, w.window)
	d.Resize(fyne.NewSize(720, 540))
	d.Show()
}

// troubleshootingButton opens the guidek it sits in the top bar on every page
func (w *Wizard) troubleshootingButton() *widget.Button {
	return widget.NewButtonWithIcon("Troubleshooting", theme.HelpIcon(), w.showTroubleshooting)
}

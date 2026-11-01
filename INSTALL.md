# Install Splatoon PC (online edition)

This version is the Splatoon 1 PC port with the **revision 288 update** and **online play** on a
community server. It contains no Nintendo files: the installer builds the game on your PC from
**your own** Wii U dumps.

## 1. What you need

| | |
| --- | --- |
| PC | Windows 10 or 11 (64-bit), NVIDIA graphics card (Vulkan 1.3), 16 GB RAM (32 GB better) |
| Disk | about **50 GB free** during the installation (about 8 GB once finished) |
| Your game | **Splatoon EUR** dumped from your own Wii U: the **game** and its **update** (v288). Each dump is a folder with `code`, `content` and `meta` inside. |
| Time | about **1 to 2 hours** the first time (the PC builds the game itself) |

Missing tools (Visual Studio Build Tools, Python 3.12, Git) are offered by the installer.

## 2. Install

1. Download this project (**Code → Download ZIP**) and extract it to a **short path**, for
   example `C:\SplatoonPC`.
2. Double-click **`Setup.bat`**.
   - If it installs tools for you, close the window when it says so and double-click
     `Setup.bat` again.
3. When asked, choose your **game** dump folder (the one that contains `code`, `content`, `meta`).
4. Wait for the base game to build (30–90 minutes).
5. When asked, choose your **update** dump folder (revision 288). Wait again (20–60 minutes).
6. Done: a **Splatoon PC Port** shortcut is on your desktop (or in this folder if you said no). No shortcut? Double-click **Play-Online.bat** in this folder: it does the same thing.

## 3. Play

- Double-click the **Splatoon PC Port** shortcut, type a **pseudo** (2 to 10 letters or digits) and click
  **PLAY**. **Play offline** starts the game without Internet.
- **Esc** opens the settings (graphics, controls, mouse, volume).
- Keep the file `Game\online_identity.ini`: it is your online account on this PC.

## 4. Update

When a new version is out, the launcher tells you at start-up and offers to update. Your save
(`Game\save`), your settings and your account are kept.

## 5. If something goes wrong

- **The installer stops with an error:** take a screenshot of the window and open an issue on
  this project, with your graphics card. Never attach game files, dumps or saves.
- **"A communication error occurred" in a match:** the game disconnects a player who does not
  move for a while, like on the Wii U. Otherwise, report the time it happened.
- **Not enough space:** free at least 50 GB on the drive of the project folder.

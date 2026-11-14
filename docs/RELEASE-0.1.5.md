# Version 0.1.5

This update is **required** to play online: the public server only accepts 0.1.5. A 0.1.4 game is stopped by its own
sign-in screen with "This server needs version 0.1.5 or newer". Offline play is not affected.

## What changes for players

- **Friend requests.** Adding a friend in the F4 page now sends a request; the other player accepts or declines it
  in their own F4 page. You see each other in the game's friend list once the request is accepted.
- **Online play on Windows no longer needs Python.** The game itself talks to the friends server, proves that its
  files are the official ones and writes the rotation files (maps, modes, Splatfest). Python is still used by
  `Setup.bat`, by the HD textures tool and by the local lab server (`Launch-Online.ps1` without a public server).
- **Clearer refusal.** When the server does not recognise the game files, the F4 page says
  "Game files not recognised by the server. Update the game." and the launcher log
  (`build\diagnostics\online-*-launcher.log`) keeps the server's exact words for bug reports.
- Online, the graphics overhaul applies to the plaza and solo scenes only; matches keep the original look.
- The ESC menu no longer shows a second Friends button; GamePad audio is mixed to the PC output when
  "GamePad on phone" is off.

## What changes for people who build or host

- The launchers read the game version from `VERSION` only.
- Setup checks the SHA-256 of what it downloads and pins vcpkg to a tested commit.
- Linux keeps `scripts/friends_relay.py` and `tools/boss`; both give the same result as the game's own code on
  Windows (`tools/tests/test_online_native.py` compares them).
- Server owners: declare the new hash and raise `min_client_version` together, see `docs/SERVEUR.md`.

## Installing

Online play needs a Git checkout of the `SplatPc-ONLINE` branch: the game proves its files to the server from the
files of that checkout.

1. `git clone -b SplatPc-ONLINE https://github.com/PC-PORTERS/SplatPC-Splatoon-Native-PC-Port.git`
2. Run `Setup.bat` (Linux: `./setup.sh`) and follow it.
3. Start the game with `Play-Online.bat` (Linux: `./Play-Online.sh`) or the desktop shortcut.

## Updating

1. `git pull` on the `SplatPc-ONLINE` branch.
2. Run `Setup.bat` again (Linux: `./setup.sh`).
3. Start the game from the usual shortcut.

## Known issues

- This release has no Builder download. A game installed by the standalone Builder (the `.exe` or AppImage used
  outside a checkout) cannot play online: it has no files to prove to the server. Use the steps above instead.
  The Builder started from inside a checkout is fine.
- When the server refuses the game, the lobby only sends you back to the plaza. The reason is in the F4 page.

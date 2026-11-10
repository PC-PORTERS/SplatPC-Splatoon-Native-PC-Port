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

## Updating

1. `git pull` on the `SplatPc-ONLINE` branch, or run the Builder again.
2. Run `Setup.bat` again (Linux: `./setup.sh`).
3. Start the game from the usual shortcut.

## Known issue

When the server refuses the game, the lobby only sends you back to the plaza. The reason is in the F4 page.

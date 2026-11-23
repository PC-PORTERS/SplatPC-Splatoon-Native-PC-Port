# Online play

Community servers replace Nintendo's: sign-in, matchmaking, friends and the NAT test. Matches themselves are peer-to-peer. The server code lives in [SplatPc-Server](https://github.com/lgicbd/SplatPc-Server) ([run your own](SERVER.md)); it contains and reads no Nintendo content.

## Playing

- Build the game with **SplatPC Builder** ([Builder](BUILDER.md)) with a `v288` dump and enable **Online** (on by default).
- First launch: pick a pseudo in the in-game login screen (or configure it in the Builder). The game signs you up natively (an account number plus a key derived from your password; the password is never stored), checks your version against the server, claims your pseudo and connects.
- You can switch back to offline mode via the in-game **Play offline** option, `RBREW_OFFLINE=1`, or by turning off Online in `settings.ini` / Builder. Your save (`Game/save`) is never touched.
- Your identity is stored in `Game/online_identity.ini`. **Never share it**: it is your account.
- The server is chosen by `settings.ini` (`online_server`), `scripts/server.txt` (yours, wins) or `scripts/server.public.txt`.

## Friends

Pseudos are unique. The game natively relays presence (who is online, for 30 seconds) and SpotPass rotations directly on both Windows and Linux so the in-game friends list (`F4`) and private matches work across PCs.

## Updates and version check

The game reads `/health` from the server: `integrity_mode`, the minimum client version and the latest released version. Below the minimum (or below the latest when the server enforces integrity), it stops with a message telling you to update from the Releases page. The client version comes from the release tag used to build the release.

## Anti-cheat: what it is, and what it is not

Cheating cannot be fully prevented in a peer-to-peer game that runs on your own PC. The project does three things, in order of strength:

1. **Version and code proof** (`scripts/integrity.py`). Each release generates `integrity-manifest.txt` listing its code files with hashes. At sign-in your PC sends the hash of its code; the server compares it with the releases it accepts. Modes in `server.json`:
   - `off` – nothing checked;
   - `log` – unknown or missing proofs are accepted and written to the log;
   - `enforce` – unknown, old or missing proofs are refused (**what the public server runs**).
   This makes casual edits and old versions visible. A determined cheater who copies the right hash is not stopped: it is a speed bump, not a lock.
2. **Moderation.** Bans by account or IP (permanent or timed), an immediate kick, and a moderation log via [SplatPc-Server](https://github.com/lgicbd/SplatPc-Server).
3. **Server-side hygiene.** Rate limits, validated pseudos, no admin route exposed, a hardened sign-up path.

Report a cheater with their pseudo and the time of the match; the operator can ban them within minutes.

## Privacy

The server keeps an account number, a derived key, your pseudo and creation / last-seen dates. IP addresses appear in the server logs (rotating) and briefly for rate limiting. Nothing else about you or your game is collected.

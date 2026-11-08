# Online play

Community servers replace Nintendo's: sign-in, matchmaking, friends and the NAT test. Matches themselves are peer-to-peer. The server code is in `tools/nexlab/` ([run your own](SERVEUR.md)); it contains and reads no Nintendo content.

## Playing

- **Windows:** `Play-Online.bat` (or the *Splatoon PC Port* shortcut). **Linux:** `./Play-Online.sh` ([Linux](LINUX.md)). **Builder:** the *Play* button ([Builder](BUILDER.md)).
- First launch: pick a pseudo and a language. The launcher signs you up (an account number plus a key derived from your password; the password is never stored), checks your version against the server, claims your pseudo and starts the game.
- `--offline` (Linux: `./Play-Online.sh --offline`) starts without any server. Your save (`Game/save`) is never touched by the launcher.
- Your identity is stored in `Game/online_identity.ini`. **Never share it**: it is your account.
- The server is chosen by `scripts/server.txt` (yours, wins) or `scripts/server.public.txt`. The launcher asks before sending your identity to a server other than the public one.

## Friends

Pseudos are unique. The launcher relays presence (who is online, for 30 seconds) so the in-game friends list works across PCs; private matches use it.

## Updates and version check

The launcher reads `/health` from the server: `integrity_mode`, the minimum client version and the latest released version. Below the minimum (or below the latest when the server enforces integrity), it stops with a message telling you to update. Get updates from the Releases page / the repository. The version shown in the launcher comes from `build/installed-version.txt` if it exists, else `VERSION`: after a manual update, delete or fix a stale `build/installed-version.txt`.

## Anti-cheat: what it is, and what it is not

Cheating cannot be fully prevented in a peer-to-peer game that runs on your own PC. The project does three things, in order of strength:

1. **Version and code proof** (`scripts/integrity.py`). Each release lists its code files with hashes in `scripts/integrity-manifest.txt`. At sign-in your PC sends the hash of its code; the server compares it with the releases it accepts. Modes in `server.json`:
   - `off` – nothing checked;
   - `log` – unknown or missing proofs are accepted and written to the log;
   - `enforce` – unknown, old or missing proofs are refused (**what the public server runs**, since 0.1.4).
   This makes casual edits and old versions visible. A determined cheater who copies the right hash is not stopped: it is a speed bump, not a lock.
2. **Moderation.** Bans by account or IP (permanent or timed), an immediate kick, and a moderation log, from `ops.py` or the local admin panel (`tools/adminpanel`, reached over SSH only; the `/admin/*` routes are closed from outside).
3. **Server-side hygiene.** Rate limits, validated pseudos, no admin route exposed, a hardened sign-up path.

Report a cheater with their pseudo and the time of the match; the operator can ban them within minutes.

## Privacy

The server keeps an account number, a derived key, your pseudo and creation / last-seen dates. IP addresses appear in the server logs (rotating) and briefly for rate limiting. Nothing else about you or your game is collected.

## En français

Les serveurs communautaires remplacent ceux de Nintendo ; les parties sont en pair-à-pair. Lance `Play-Online.bat` (Windows) ou `./Play-Online.sh` (Linux) : pseudo, inscription automatique, vérification de version, puis jeu. `--offline` joue sans serveur. Ne partage jamais `Game/online_identity.ini`. L'anti-triche complet est impossible en pair-à-pair : le serveur vérifie la version et l'empreinte du code (modes `off`, `log`, `enforce` ; le serveur public est en `enforce`), et la modération peut bannir un compte ou une IP en quelques minutes.

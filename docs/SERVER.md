# Run your own server

The community server lives in its own repository: [SplatPc-Server](https://github.com/lgicbd/SplatPc-Server). It replaces Nintendo's servers for online play (sign-in, matchmaking, friends and the NAT test) and contains or reads no Nintendo content. Use it to host your own server or to test locally.

## Steps

1. **Get the server:** clone [https://github.com/lgicbd/SplatPc-Server.git](https://github.com/lgicbd/SplatPc-Server.git) on a Windows or Linux machine with Python 3.12 and a public IP (the NAT test needs **two different public IPs**; otherwise set `"natcheck": false`, though the game may leave the session).
2. **Install & configure:** follow the server repository's setup instructions (`requirements.txt`, `server.example.json` copied to `server.json`):
   ```json
   {
     "mode": "public",
     "advertise": "203.0.113.10",
     "natcheck": true,
     "natcheck_addresses": ["203.0.113.10", "203.0.113.11"],
     "natcheck_same_socket": true,
     "accounts_db": "/var/lib/splatoon/accounts.sqlite3",
     "registration": "open",
     "registration_proxy": true,
     "registration_port": 60002,
     "min_client_version": "0.1.0"
   }
   ```
3. **Open the ports:** UDP 60000, 60001, 10025, 10125 (10025 and 10125 on **both** IPs), and TCP 80 and 443 for HTTPS sign-up below. Never expose TCP 60002.
4. **HTTPS sign-up and friends (Caddy):** point a domain to the server and use this `Caddyfile`; Caddy gets the certificate automatically:
   ```
   your-domain.example {
       @ok path /register /health /social/*
       reverse_proxy @ok 127.0.0.1:60002
       respond 404
   }
   ```
   `/social/*` serves the friends feature (unique pseudos, who is online): small JSON requests, nothing else to configure.
5. **Start the server:** follow the start command in [SplatPc-Server](https://github.com/lgicbd/SplatPc-Server) (for example as a systemd service). The log should show `mode=public`, `NAT check on ...` and `accounts: ...`.
6. **Point the players to it:** in `scripts/server.public.txt` (or a local `scripts/server.txt`, which takes precedence) or in **SplatPC Builder**:
   ```
   server=203.0.113.10:60000
   nncs1=203.0.113.10
   nncs2=203.0.113.11
   register=https://your-domain.example/register
   ```
7. **Check:** `https://your-domain.example/health` must answer `{"ok": true, ...}`.

## Running it

See [SplatPc-Server](https://github.com/lgicbd/SplatPc-Server) for the account, moderation (`ops.py` / admin panel) and backup commands.

- **Old game versions:** `min_client_version` is published in `/health`; the game refuses to play online below it. Raise it after an update that breaks compatibility, then restart the server.
- **Game integrity:** each GitHub release ships `integrity-manifest.txt` (generated from the release tag by `scripts/integrity.py --make`, which prints the release's `release_hash`). The game sends its version (from the release tag) and the code hash when it signs in (`/social/session`). In `server.json`: `"integrity_mode": "log"` only logs unknown hashes; `"enforce"` refuses the sign-in (HTTP 426) **and** the NEX login of a PC that has not proved a listed release in the last 12 hours; `"integrity_releases": {"0.1.5": "<release_hash>"}` lists the accepted releases (a version may list several hashes). Start with `log`, check the log (`integrity: pid ... refused proof`), then switch to `enforce` once players have updated. This makes casual changes and old versions visible and refused; it cannot stop someone who copies the right hash, so moderation stays the real answer.
- **Moderation:** use `ops.py` or the server admin panel from [SplatPc-Server](https://github.com/lgicbd/SplatPc-Server) to ban/unban by PID or IP (temporary or permanent), kick active players and inspect moderation logs.
- **Logs:** rotating, 8 MB x 3. Read them first when the game stops at a step.
- **Common problems:** `Address already in use` (port taken); the player quits after matchmaking (NAT test unreachable: check ports 10025/10125 and the two IPs); `RendezVous::InvalidUsername` (account not signed up yet, or banned).

## Privacy (what you store)

An account number, a derived key (never the password), the player's pseudo, creation and last-seen dates (who is online lives in memory only, for 30 seconds); IP addresses in the logs and, briefly, for sign-up limits and bans. Say so to your players (see the README).

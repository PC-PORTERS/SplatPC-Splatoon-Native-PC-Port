# Run your own server

The server in `tools/nexlab/` (Python) replaces Nintendo's servers for online play: sign-in, matchmaking and the NAT test.
It contains and reads no Nintendo content.

## Steps

1. **Machine:** Windows or Linux, Python 3.12, a public IP. The NAT test needs **two different public IPs** (otherwise set `"natcheck": false`, and the game may leave the session).
2. **Install:** `py -3.12 -m pip install -r tools\nexlab\requirements.txt` (Linux: use a venv and `.venv/bin/pip`).
3. **Configure:** copy `tools/nexlab/server.example.json` to `server.json` and edit it:
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
4. **Open the ports:** UDP 60000, 60001, 10025, 10125 (10025 and 10125 on **both** IPs), and TCP 80 and 443 for the HTTPS sign-up below. Never expose TCP 60002.
5. **HTTPS sign-up and friends (Caddy):** point a domain (a free DuckDNS one works) to the server and use this `Caddyfile`; Caddy gets the certificate by itself:
   ```
   your-domain.example {
       @ok path /register /health /social/*
       reverse_proxy @ok 127.0.0.1:60002
       respond 404
   }
   ```
   `/social/*` serves the friends feature (unique pseudos, who is online): small JSON requests, nothing else to configure.
6. **Start:** `py -3.12 tools\nexlab\nexserver.py --config server.json` (Linux: `.venv/bin/python tools/nexlab/nexserver.py --config server.json`; run it as a systemd service to keep it up). The log must show `mode=public`, `NAT check on ...` and `accounts: ...`.
7. **Point the players to it:** in `scripts/server.public.txt` (or a local `scripts/server.txt`, which wins):
   ```
   server=203.0.113.10:60000
   nncs1=203.0.113.10
   nncs2=203.0.113.11
   register=https://your-domain.example/register
   ```
8. **Check:** `https://your-domain.example/health` must answer `{"ok": true, ...}`.

## Running it

```
py -3.12 -m tools.nexlab.accounts ban --pid 5eea2491 --reason "cheating"   # or --ip 203.0.113.7
py -3.12 -m tools.nexlab.accounts unban --pid 5eea2491
py -3.12 -m tools.nexlab.accounts backup backup.sqlite3                     # stop the server before restore
```

- **Old game versions:** `min_client_version` is published in `/health`; the launcher refuses to play online below it. Raise it after an update that breaks compatibility, then restart the server.
- **Logs:** rotating, 8 MB x 3. Read them first when the game stops at a step.
- **Common problems:** `Address already in use` (port taken); the player quits after matchmaking (NAT test unreachable: check ports 10025/10125 and the two IPs); `RendezVous::InvalidUsername` (account not signed up yet, or banned).

## Privacy (what you store)

An account number, a derived key (never the password), the player's pseudo, creation and last-seen dates (who is online lives in memory only, for 30 seconds); IP addresses in the logs and, briefly, for sign-up limits and bans. Say so to your players (see the README).

---

# Héberger son propre serveur

Le serveur de `tools/nexlab/` (Python) remplace les serveurs de Nintendo pour le jeu en ligne : connexion, matchmaking et test NAT.
Il ne contient ni ne lit aucun contenu de Nintendo.

## Étapes

1. **Machine :** Windows ou Linux, Python 3.12, une IP publique. Le test NAT demande **deux IP publiques différentes** (sinon `"natcheck": false`, et le jeu peut quitter la session).
2. **Installer :** `py -3.12 -m pip install -r tools\nexlab\requirements.txt` (Linux : un venv et `.venv/bin/pip`).
3. **Configurer :** copie `tools/nexlab/server.example.json` en `server.json` et adapte-le (même exemple que ci-dessus : `mode`, `advertise`, `natcheck_addresses`, `accounts_db`, `registration`, `registration_proxy`, `min_client_version`).
4. **Ouvrir les ports :** UDP 60000, 60001, 10025, 10125 (10025 et 10125 sur **les deux** IP), et TCP 80 et 443 pour l'inscription HTTPS. N'expose jamais le TCP 60002.
5. **Inscription et amis en HTTPS (Caddy) :** pointe un nom de domaine (un DuckDNS gratuit suffit) vers le serveur et utilise le `Caddyfile` ci-dessus (la ligne `/social/*` sert la liste d'amis : pseudos uniques, joueurs en ligne) ; Caddy obtient seul le certificat.
6. **Démarrer :** `py -3.12 tools\nexlab\nexserver.py --config server.json` (Linux : `.venv/bin/python tools/nexlab/nexserver.py --config server.json` ; un service systemd le garde en marche). Le journal doit afficher `mode=public`, `NAT check on ...` et `accounts: ...`.
7. **Indiquer le serveur aux joueurs :** dans `scripts/server.public.txt` (ou un `scripts/server.txt` local, prioritaire), avec `server=`, `nncs1=`, `nncs2=` et `register=` (voir l'exemple ci-dessus).
8. **Vérifier :** `https://ton-domaine/health` doit répondre `{"ok": true, ...}`.

## Exploitation

- **Bannir / débannir / sauvegarder :** les commandes `accounts ban`, `unban` et `backup` ci-dessus (arrête le serveur avant une restauration).
- **Anciennes versions du jeu :** `min_client_version` est publié dans `/health` ; le lanceur refuse de jouer en ligne en dessous. Augmente-le après une mise à jour qui casse la compatibilité, puis redémarre le serveur.
- **Journaux :** tournants, 8 Mo x 3. À lire en premier quand le jeu s'arrête à une étape.
- **Problèmes courants :** `Address already in use` (port pris) ; le joueur quitte après le matchmaking (test NAT injoignable : ports 10025/10125 et deux IP) ; `RendezVous::InvalidUsername` (compte pas encore inscrit, ou banni).

## Vie privée (ce que tu stockes)

Un numéro de compte, une clé dérivée (jamais le mot de passe), le pseudo du joueur, les dates de création et de dernière connexion (qui est en ligne reste en mémoire, 30 secondes) ; des adresses IP dans les journaux et, brièvement, pour les limites d'inscription et les bannissements. Dis-le à tes joueurs (voir le README).

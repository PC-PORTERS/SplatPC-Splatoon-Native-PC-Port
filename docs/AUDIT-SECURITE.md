# Audit de sécurité — serveur NEX (`tools\nexlab\`)

**Date** : 28/10/2026. **Méthode** : lecture seule du code (aucune modification, aucune
attaque exécutée), en complément des 52 tests de l'époque (73 après les corrections, section
« État des corrections »). **Périmètre** : `nexserver.py`, `matchmake.py`,
`natcheck_server.py`, `accounts.py`, `register_server.py`, `server_config.py`, `register.py`.
**Hors périmètre** : bibliothèque NintendoClients/anyio/anynet (supposée correcte), pile réseau et
OS, client du jeu (`recompiler\port`), scripts PowerShell.

## Résumé

Aucune faille **critique** : le mot de passe NEX n'est jamais stocké en clair côté serveur, toutes
les requêtes SQL sont paramétrées, et aucun secret n'est écrit dans les journaux. En revanche,
quatre risques **élevés** subsistent pour un serveur exposé à Internet : l'inscription envoie le
mot de passe en clair (HTTP), le test NAT peut être empoisonné pour rediriger les URL de session,
les méthodes de matchmaking ne vérifient pas qui a le droit d'agir, et rien ne limite les
connexions/login (déni de service). Cinq risques **moyens** concernent la journalisation, la base
SQLite, l'inscription ouverte et le serveur HTTP d'inscription.

| ID | Gravité | Risque | Fichier:ligne |
| --- | --- | --- | --- |
| S-01 | Élevé | Mot de passe NEX envoyé en clair (HTTP) à l'inscription | `register.py:44-60`, `register_server.py:87-125`, `nexserver.py:297-298` |
| S-02 | Élevé | Empoisonnement de `PEER` via le dernier paquet NAT check reçu | `natcheck_server.py:44-45`, `matchmake.py:362-368`, `matchmake.py:344` |
| S-03 | Élevé | Aucune autorisation sur les méthodes de matchmaking | `matchmake.py:315-333`, `348-360` |
| S-04 | Élevé | Pas de limite de connexions ni de logins (DoS) | `nexserver.py:109-128`, `241-289` |
| S-05 | Moyen | Sessions sans plafond ni expiration après fermeture | `matchmake.py:175-196` |
| S-06 | Moyen | Journalisation non bornée, données client en hexadécimal | `natcheck_server.py:41,56`, `nexserver.py:193,223` |
| S-07 | Moyen | Inscription ouverte par défaut, pas de limite des succès, oracle 409 | `server_config.py:142`, `register_server.py:87-125`, `accounts.py:148-155` |
| S-08 | Moyen | Clés Kerberos en clair dans SQLite, permissions par défaut | `accounts.py:80-98` |
| S-09 | Moyen | Serveur HTTP sans délai ni plafond de threads (slowloris) | `register_server.py:42-49` |
| S-10 | Faible | Jeton d'inscription non ASCII → exception 500 | `register_server.py:74-79` |
| S-11 | Faible | Nom d'utilisateur géant → exception non gérée et trace | `nexserver.py:70-77` |
| S-12 | Faible | `num_participants` fourni par le client, critères non vérifiés | `matchmake.py:217-221,269`, `162-174` |
| S-13 | Faible | `join_message` retransmis tel quel aux autres joueurs | `matchmake.py:75-88` |
| S-14 | Faible | Outil joueur : URL arbitraire, pas d'épinglage TLS, jeton en argument | `register.py:40-65` |
| S-15 | Faible | CLI comptes : mot de passe visible en ligne de commande | `accounts.py:171-190` |
| S-16 | Faible | Config : chemin de fichier arbitraire, liens symboliques suivis | `server_config.py:98-110` |
| S-17 | Faible | Connexion d'authentification PRUDP en clair (PID visible) | `nexserver.py:241-289` |
| S-18 | Faible | `/health` révèle le nombre de comptes | `register_server.py:81-85` |

## État des corrections (28/10/2026, 12h10)

Chaque correction est couverte par un test de `tools\tests\test_security.py` (20 tests, tous en
échec avant la correction). Suite complète : **73/73**.

| ID | État | Correctif |
| --- | --- | --- |
| S-01 | **Corrigé** (documentation + garde-fou) | `docs\SERVEUR.md` impose TLS pour `/register` ; `register.py` refuse `http://` non local (`check_server_url`, `--allow-http` explicite). Le serveur ne fait toujours pas de TLS : c'est au proxy de le terminer. |
| S-02 | **Corrigé (public) / partiel assumé (labo)** | Plus de `LAST_SENDER` global : `natcheck_server.OBSERVATIONS` par IP d'origine, `consume(ip)` (péremption 120 s) ; `report_nat_properties` associe l'observation à l'IP de la connexion. En public, `PEER` n'est plus jamais posé ni utilisé. En labo, `PEER` ne sert plus que de repli quand `ReplaceURL` n'a pas annoncé le vrai endpoint PIA. Reste : deux joueurs derrière la **même** IP restent indistinguables côté serveur (cas du duo sur 127.0.0.1) ; c'est sans effet en public. |
| S-03 | **Corrigé** | `CloseParticipation`, `OpenParticipation`, `ModifyCurrentGameAttribute` : hôte uniquement ; `UpdateProgressScore` : membres seulement ; `GetSessionURLs` : membres seulement (sinon `SessionVoid`). Refus journalisé, pas d'erreur protocolaire nouvelle. |
| S-04 | **Corrigé (partiel assumé)** | Plafonds de connexions (32/IP, 1024 au total) via `_serve_rmc` ; limiteurs de débit 60 logins/min/IP et 20 tickets/min/PID, appliqués à **tous** les logins (pas seulement les PID inconnus). Reste : le handshake PRUDP de la bibliothèque aboutit avant le refus, donc un flood SYN consomme encore un handshake par paquet. |
| S-05 | **Corrigé** | `MAX_SESSIONS = 512` (erreur `SessionFull`), expiration des sessions fermées après `SESSION_TTL = 600 s`, purge à la création et à la recherche. |
| S-06 | **Corrigé** | NAT check en DEBUG ; requêtes RMC en DEBUG en mode public (INFO conservé en labo) ; `SendReport` tronqué à 32 octets + taille ; journal rotatif 8 Mo × 3. |
| S-07 | **Corrigé (partiel assumé)** | `registration = "closed"` par défaut en public ; toutes les tentatives (réussies incluses) comptent dans la fenêtre par IP. Reste : l'oracle 409 (un PID pris est distinguable) et pas de plafond global d'inscriptions. |
| S-08 | **Corrigé (POSIX)** | `chmod 0600` sur la base et ses fichiers WAL/SHM à la création et après chaque inscription. Sous Windows, les ACL héritées du profil s'appliquent ; le fichier reste un secret à sauvegarder avec précaution. |
| S-09 | **Corrigé** | `BoundedHTTPServer` : 8 requêtes simultanées au maximum, délai de 10 s par connexion, refus immédiat au-delà. |
| S-10 | **Corrigé** | Comparaison du jeton en octets (`register_server.token_ok`), plus de `TypeError` sur un jeton non ASCII. |
| S-11 | **Corrigé** | `parse_username` refuse plus de 10 chiffres avant `int()`. |
| S-12 | **Partiel** | `num_participants` borné à la capacité (8) ; les critères de recherche ne sont toujours pas appliqués (choix de matchmaking public, à revoir avec l'étape 5/6). |
| S-13 | **Corrigé** | `join_message` tronqué à 200 caractères avant notification. |
| S-14 | **Partiel** | Garde-fou d'URL et jeton par variable d'environnement ; pas d'épinglage de certificat (le HTTPS système est utilisé). |
| S-15 | **Corrigé** | `--password` supprimé ; `--password-stdin` ou invite sans écho. |
| S-16 | **Reste** | Chemin de configuration fourni par l'opérateur ; risque local, non prioritaire. |
| S-17 | **Reste** | PRUDP d'authentification sans TLS (bibliothèque) ; atténué par la limitation de débit et l'absence de mot de passe sur le fil. |
| S-18 | **Corrigé** | `/health` ne renvoie plus le nombre de comptes. |

## Ajouts du 28/10 soir (chantiers 2 et 4)

- **Bans opérateur** (`accounts.py` table `bans`, CLI `ban/unban/bans`) : PID, IP ou couple ;
  refus au login (même chemin que S-03, journalisé) et à l'inscription (403). Aucun secret stocké.
- **`/metrics`** : compteurs seulement (`players`, `sessions`, `connections`, `errors`, `uptime`) ;
  aucune identité, aucun jeton. **`/admin/sessions`** et **`/admin/kick`** exigent le jeton
  d'inscription (401 sinon) ; le kick ferme la connexion et efface l'état (pas d'injection).
- **Gardien PRUDP** (`_idle_janitor`) : ferme les connexions muettes depuis 60 s (S-04/B-17) ;
  les journaux restent bornés (rotation 8 Mo × 3) et l'arrêt propre efface sessions et base.
- **S-04/S-05 inchangés** ; la limite de 16 connexions PRUDP par flux (B-16) reste une limite de
  bibliothèque, hors correctif local.

## Détails
### S-01 — Mot de passe NEX en clair à l'inscription (Élevé)
`register.py:44-60` envoie `{"pid": …, "password": …}` en HTTP vers l'URL passée en `--server` ;
`register_server.py:87-125` l'accepte sans TLS, et `nexserver.py:297-298` expose l'endpoint sur
`0.0.0.0` en mode public. Le mot de passe est la matière première de la clé Kerberos : quiconque
écoute le réseau (Wi-Fi public, proxy, opérateur) peut usurper l'identité du joueur ensuite.
*Recommandation* : TLS obligatoire devant l'endpoint (reverse proxy), refuser une URL `http://` en
dehors du loopback, mode `token` ou `closed` par défaut en public, et documenter l'exigence (déjà
amorcée dans `docs\SERVEUR.md`).

### S-02 — Empoisonnement de `PEER` par le dernier NAT check (Élevé)
`natcheck_server.py:44-45` met à jour un global `LAST_SENDER` pour **tout** datagramme reçu, sans
lien avec le joueur qui l'a envoyé ; `matchmake.py:362-368` l'affecte à `PEER[pid]` quand ce joueur
appelle `ReportNATProperties`, et `matchmake.py:344` remplace l'URL de l'hôte par ce couple dans
`GetSessionURLs`. Un client malveillant (ou un simple paquet UDP forgé vers 10025) peut donc faire
pointer les arrivants vers l'adresse de son choix : panne de session ciblée, ou relais passif.
*Recommandation* : associer l'observation au couple (IP source, port PIA déclaré) et non au dernier
paquet global ; accepter `ReportNATProperties` seulement si l'IP source de la connexion sécurisée
correspond à l'IP observée ; sinon ignorer et journaliser.

### S-03 — Aucune autorisation sur le matchmaking (Élevé)
`matchmake.py:315-333` : `CloseParticipation`, `OpenParticipation`,
`ModifyCurrentGameAttribute` et `UpdateProgressScore` acceptent n'importe quel `gid` de n'importe
quel client connecté, sans vérifier qu'il est l'hôte ou un membre. `matchmake.py:348-360` :
`UnregisterGathering`/`EndParticipation` ne retirent que le PID appelant (correct), mais
`GetSessionURLs`/`FindMatchmakeSession…` exposent les URL (IP, port, RVCID) de n'importe quelle
session. Un joueur peut donc fermer la salle d'autrui ou la faire modifier.
*Recommandation* : vérifier l'appartenance (`pid in SESSIONS.members[gid]`) et réserver la
fermeture/modification à l'hôte ; limiter `GetSessionURLs` aux membres.

### S-04 — Déni de service par connexions et logins non limités (Élevé)
`nexserver.py:109-128` ne limite (10/5 min par IP) que les PID **inconnus** ; un PID connu, ou une
rafale de SYN PRUDP (aucun plafond par IP dans `serve`, `241-289`), fait allouer une connexion et,
à chaque `Login`/`RequestTicket`, un ticket Kerberos (génération de 32 octets aléatoires +
chiffrement). Rien ne borne le nombre de connexions simultanées, ni le débit de tickets.
*Recommandation* : plafonner les connexions et les tentatives par IP (y compris réussies), limiter
les tickets par PID et par minute, et prévoir un délai de garde sur les connexions inactives.

### S-05 — Croissance illimitée des sessions (Moyen)
`matchmake.py:175-196` crée une session à chaque `AutoMatchmake…` quand aucune n'est ouverte, et
une session fermée ou pleine reste en mémoire jusqu'au départ de tous ses membres. Un client peut
créer des milliers de sessions à un joueur (`max_participants=1`, `game_mode` libre) : mémoire
croissante, jamais expirée.
*Recommandation* : plafonner le nombre de sessions, supprimer les sessions fermées après un délai,
et limiter la création par PID.

### S-06 — Journalisation non bornée et données client (Moyen)
`natcheck_server.py:56` journalise **chaque** datagramme NAT (non authentifié) ; `nexserver.py:223`
journalise chaque requête RMC, y compris les protocoles inconnus ; `nexserver.py:193` écrit en hexa
tout le contenu d'un `SendReport`. Un flot UDP suffit à remplir le disque du serveur ; le
`SendReport` peut contenir des données du joueur.
*Recommandation* : niveau par défaut WARN pour les paquets NAT, échantillonnage/limite de débit des
lignes RMC, troncature du `SendReport` (premiers octets + taille), rotation des journaux.

### S-07 — Inscription ouverte, succès non limités, oracle (Moyen)
`server_config.py:142` met `registration = "open"` par défaut en public ; `accounts.py:148-155`
compte les échecs mais **pas** les inscriptions réussies, donc un attaquant peut créer des comptes
sans limite (croissance SQLite) ; `register_server.py:117-124` répond 409 si le PID existe avec un
autre mot de passe, ce qui révèle qu'un PID est déjà pris (et permet de « squatter » le PID d'un
joueur vu en session, le privant de son compte).
*Recommandation* : limite globale d'inscriptions et par IP (succès compris), `registration="token"`
par défaut en public, réponse 409 neutre, purge des comptes jamais utilisés.

### S-08 — Clés Kerberos en clair dans SQLite (Moyen)
`accounts.py:80-98` crée `nexlab-accounts.sqlite3` (mode WAL) avec les clés dérivées en clair ; les
permissions suivent l'umask (souvent 0644 sur Linux) et les copies de sauvegarde contiennent les
identifiants. Toute lecture du fichier = usurpation de tous les comptes.
*Recommandation* : `chmod 600` (et dossier 700), utilisateur de service dédié, sauvegardes
chiffrées ou exclues, et documenter que ce fichier est un secret.

### S-09 — HTTP sans délai ni plafond (Moyen)
`register_server.py:42-49` utilise `ThreadingHTTPServer` (un thread par connexion) sans `timeout`
sur les connexions ni plafond de threads : un attaquant peut ouvrir des connexions et ne jamais
envoyer de corps (slowloris), épuisant les threads.
*Recommandation* : `timeout` court sur le handler, plafond de threads (ou reverse proxy avec
limites), et fermeture explicite des connexions sans `Content-Length`.

### S-10 à S-18 — Faibles
- **S-10** `register_server.py:74-79` : `hmac.compare_digest` sur des `str` exige de l'ASCII ; un
  jeton non ASCII lève `TypeError` (500 + trace). Comparer des octets.
- **S-11** `nexserver.py:70-77` : `int(username)` sur des milliers de chiffres lève `ValueError`
  (limite Python) non convertie en `RMCError` → trace générique et bruit de journal.
- **S-12** `matchmake.py:217-221,269` : `num_participants` vient du client et est renvoyé aux
  autres ; `find_open` ne compare que `game_mode`, pas les critères de recherche → affichage
  trompeur, sessions rejoignables sans correspondre aux critères.
- **S-13** `matchmake.py:75-88` : `join_message` est recopié dans la notification 3001 vers tous
  les membres ; si le client l'affiche, un joueur peut injecter du texte dans l'interface des
  autres.
- **S-14** `register.py:40-65` : le client envoie le mot de passe à l'URL qu'on lui donne, sans
  épinglage ni contrôle de schéma ; `--token` apparaît dans l'historique PowerShell.
- **S-15** `accounts.py:171-190` : `--password` en argument est visible dans la liste des
  processus ; `getpass` n'est utilisé que si l'option est absente.
- **S-16** `server_config.py:98-110` : le chemin de config est lu tel quel (liens symboliques
  suivis) ; risque local, opérateur.
- **S-17** `nexserver.py:241-289` : la connexion d'authentification PRUDP est anonyme (pas de TLS)
  ; le PID et le jeton `rbrew1:…` circulent en clair. Pas de mot de passe, mais le PID permet de
  cibler un joueur (voir S-02, S-07).
- **S-18** `register_server.py:81-85` : `/health` donne le nombre de comptes (information
  d'exploitation, faible).

## Ce qui est déjà sain (vérifié)

- Aucun mot de passe ni clé n'est écrit dans les journaux ; `accounts.py` ne journalise rien.
- Requêtes SQL toutes paramétrées ; aucune concaténation, aucun `exec`/`eval`, aucun appel shell.
- Limite de corps HTTP (4 Ko), validation stricte du PID et du mot de passe (16 alphanumériques
  ASCII), refus de collision, comparaison à temps constant de la clé et du jeton.
- Limite de tentatives par IP sur les PID inconnus (10 / 5 min) ; table `failures` élaguée.
- Ticket Kerberos à durée de vie courte (120 s, bibliothèque) ; clé de session aléatoire par ticket.
- Séparation nette des ports (auth / sécurisé / inscription) et des modes labo/public.
- Les tests ne couvrent pas la sécurité : aucune attaque, aucun test de débit ni d'autorisation.

## Priorités suggérées

1. S-01 (TLS à l'inscription) — bloquant pour un serveur public.
2. S-02 (empoisonnement PEER) — corrige aussi un bug de concurrence latent.
3. S-04 (limites de connexion/login) — sans cela, une seule machine peut saturer le serveur.
4. S-03 (autorisations matchmaking) — griefing trivial sinon.
5. S-06/S-08 (journaux et fichier SQLite) — exploitation locale et saturation disque.

## Limites de cet audit

Lecture de code seulement : pas de test d'intrusion, pas de validation dynamique des hypothèses
d'exploitation, pas d'analyse de la bibliothèque NintendoClients (PRUDP/RMC) ni du client du jeu.
À refaire après les corrections, et à compléter par un test réel entre deux réseaux quand le
client ne sera plus confiné au loopback.

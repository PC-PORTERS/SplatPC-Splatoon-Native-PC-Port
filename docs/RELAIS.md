# Relais PIA — étude (28/10/2026)

**Question posée (mission, étape 7)** : PIA sait-il relayer d'origine (champs `natf`/`natm`,
`RVCID`, type 3 dans les URL de station) ? Comment fait Pretendo ? Faut-il un relais UDP côté
serveur avec une redirection dans `nsysnet` ? Quelle est la solution la plus simple, et ses
limites ?

**Statut : étude seulement.** Rien n'est implémenté ici. Toute mise en œuvre attend l'accord
de l'utilisateur.

## 1. Ce que fait PIA

PIA est la bibliothèque réseau pair-à-pair de Nintendo. D'après la documentation publique
([vue d'ensemble](https://github.com/kinnay/NintendoClients/wiki/Pia-Overview),
[types](https://github.com/kinnay/NintendoClients/wiki/Pia-Types)) :

- Tous les paquets de jeu passent en **UDP**, chiffrés par PIA lui-même.
- Un groupe connecté est un **mesh** avec **un seul hôte** ; l'hôte traite les demandes
  d'adhésion, puis envoie un *mesh update* qui donne aux autres joueurs la station du nouveau.
  Ensuite **chaque joueur se connecte directement à chaque autre** : ce n'est pas une étoile.
- Adhésion : traversée NAT (sondes `InitiateProbe` / `probe request` / `probe reply` via le
  serveur NEX, méthode 3/2), puis `connection request` directe entre consoles.
- **Aucune étape de relais** dans ce flux documenté. Le relais n'est pas un mode de PIA : c'est
  au serveur de matchmaking de fournir une adresse que les pairs peuvent atteindre.

### Les champs cités par la mission

| Champ | Sens réel (doc PIA) |
| --- | --- |
| `natm` | NAT mapping : 1 = indépendant du destinataire, 2 = dépendant du destinataire |
| `natf` | NAT filtering : 1 = indépendant du port, 2 = dépendant du port |
| `type` (station) | *NAT location* : 1 = derrière un NAT, 2 = publique. Dans une URL de station NEX, `type` est un masque (2 = publique) ; `3` = publique **et** derrière un NAT. Ce n'est **pas** un marqueur de relais. |
| `RVCID` | Identifiant de connexion Rendez-Vous, reçu à `Register` (notre `connection_id`) |
| `CID` | Identifiant de connexion PIA (variable id), fourni par l'hôte à `ReplaceURL` |

La structure `StationLocation` de PIA 5.2–5.11 contient bien un champ **Relay address**, mais
le flux d'adhésion documenté ne s'en sert pas : rien n'indique que Splatoon (Wii U, 2015)
l'utilise. Nos journaux labo le confirment de notre côté : l'URL enregistrée est
`prudp:/address=…;port=…;Pl=2;natf=0;natm=0;pmp=0;sid=15;upnp=0`, sans adresse de relais.

## 2. Ce que fait Pretendo

- **Deux IP** pour `nncs1`/`nncs2` (obligatoire, cf. leur README nncs), qui mesurent les NAT
  mapping/filtering avec les types 1–5 et 101–103.
- Le serveur NEX renvoie aux joueurs l'**endpoint UDP réel** observé par le nncs, pas le port
  TCP « WebSocket » que PIA enregistre à `ReplaceURL` — c'est le rôle du *natbridge* de
  [nextendo-nex](https://github.com/NextendoNetwork/nextendo-nex/blob/main/natbridge.go) :
  l'URL LAN de l'hôte porte l'adresse privée, le port UDP observé, `RVCID` et le `CID` du
  `ReplaceURL`, et l'URL publique porte `type=11` et `Pa=<adresse privée>`.
- **Pretendo ne relaie pas le trafic de jeu** dans son cœur de service : il fait du NAT
  traversal classique (sondes) avec le bon endpoint. nextendo-nex a ajouté un **relais de
  station optionnel, désactivé par défaut** (`SetStationRelay`) pour un jeu où les deux pairs
  sont derrière un NAT strict (Luigi's Mansion 3). C'est un contournement ciblé, pas un mode
  général.

Pour notre port, l'équivalent du natbridge existe déjà en labo : `matchmake.PEER`, alimenté par
`natcheck_server.LAST_SENDER`, remplace l'URL de l'hôte par son vrai socket UDP dans
`GetSessionURLs` (mesuré le 27/10 : le jeu envoyait ses paquets PIA au port NEX de l'hôte).

## 3. Options pour Splatoon PC

### A. Réparer le chemin direct (le plus simple, déjà à moitié fait)
- Endpoint UDP réel dans `GetSessionURLs` (fait en labo ; à confirmer en public).
- Ajouter **UPnP / NAT-PMP** côté client (`nsysnet`, réglage `upnp=1` dans l'URL de station) :
  beaucoup de box ouvrent le port automatiquement. C'est la vraie réponse au NAT strict pour
  la majorité des joueurs, sans relais ni latence ajoutée.
- IPv6 quand le jeu le permet : plus de NAT du tout (PIA 5.11+ gère les adresses 16 octets).

### B. Relais « hôte » par le serveur (partiel)
Le serveur annonce, dans `GetSessionURLs`, l'endpoint du relais à la place de celui de l'hôte.
Le relais retransmet les datagrams entre l'hôte et chaque arrivant.
- **Limite majeure** : PIA est un mesh, et les autres joueurs apprennent les stations par le
  *mesh update* de l'hôte, pas par le serveur. Un relais annoncé pour l'hôte ne corrige donc
  que les liaisons vers l'hôte ; les liaisons pair-à-pair restent directes et échouent encore
  si les deux NAT sont stricts.
- Coût : un aller-retour de plus (latence), bande passante agrégée faible devant un FPS mais
  non nulle (~20–25 paquets/s par joueur mesurés en duo), et le relais devient un point de
  panne unique.

### C. Relais complet (nécessite une modification côté client)
Rediriger **tout** le trafic PIA d'un joueur vers le relais, qui multiplexe par `RVCID`/`CID`
et réécrit les adresses de station vues par le jeu.
- Côté serveur : simple redirecteur UDP (aucun déchiffrement : PIA chiffre de bout en bout),
  mais il doit tenir une table `RVCID → socket client` et prévenir les pairs.
- Côté client : une redirection dans `nsysnet` (`RBREW_PIA_RELAY=hôte:port`, envoi de tout
  l'UDP PIA au relais) **et** la réécriture des adresses de station renvoyées par
  `GetSessionURLs`/`mesh update`, sinon PIA envoie quand même aux adresses directes.
- C'est un chantier réel (protocole PIA non documenté pour la réécriture, handshakes,
  migration d'hôte), pour un gain qui ne concerne que les deux joueurs « NAT strict ».

## 4. Recommandation

1. **UPnP/NAT-PMP** d'abord (option A) : ça règle la majorité des cas sans relais.
2. Vérifier en conditions réelles que l'endpoint UDP annoncé en public est bien celui observé
   par le nncs (le labo le fait déjà).
3. Ne construire un relais (option B) que si un test réel entre deux réseaux stricts échoue, et
   le documenter comme « dépannage » : il ne couvre pas le pair-à-pair du mesh.
4. L'option C (relais complet + redirection `nsysnet`) seulement si l'utilisateur accepte le chantier et
   si le besoin est confirmé par des joueurs réels.

## 5. Décision demandée à l'utilisateur

- Étape 7 validée comme étude : **oui / non**.
- Faut-il prévoir l'option A (UPnP) dans la prochaine passe côté `nsysnet` ? (hors étape 5, qui
  n'est pas demandée).
- L'option B (relais hôte) est-elle utile à court terme, ou attend-on un vrai test à deux sur
  Internet ?

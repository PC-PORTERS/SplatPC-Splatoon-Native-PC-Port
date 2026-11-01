# Splatoon 1 PC Port - Online Edition

An unofficial native PC port of **Splatoon (Wii U)** for Windows, made by statically recompiling the game's code
into a native program with a Vulkan renderer. It continues [RebrewU](https://github.com/ApfelTeeSaft/RebrewU) by ApfelTeeSaft.
This branch (`SplatPc-ONLINE`) adds the **revision 288 update** and **online play on a community server**.

> **No Nintendo code, assets or game files are in this repository.** You need your own copy of the game, dumped from
> your own Wii U (game + update 288, EUR). The port is built on your PC from your files.
> *Splatoon is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.*

> **Work in progress.** Expect glitches, stutter and crashes. A recent NVIDIA GPU (Vulkan 1.3), 16 GB of RAM
> (32 GB better) and about 40 GB of free disk during setup are recommended.

## Install

Follow **[INSTALL.md](INSTALL.md)** (en francais : [INSTALL-FR.md](INSTALL-FR.md)). In short:

1. Download this branch as a ZIP and extract it to a short path (for example `C:\SplatoonPort`).
2. Run `Setup.bat`, pick your **game** dump, then your **update 288** dump. The first build takes 30-90 minutes.
3. Start the game with the **Splatoon PC Port** shortcut: choose a pseudo and a language, then **PLAY**.

The launcher checks for updates when it starts and rebuilds only what changed.

## Playing

| Input | Action |
|---|---|
| W A S D / Mouse | Move / aim |
| Left click / Right click | Shoot (ZR) / Squid form (ZL) |
| Space, Q, E | Jump, sub weapon, special |
| F, G | Map (+), Options (-) |
| Esc | Settings menu (graphics, controls, volume) |
| F4 | Friends window (add a friend by pseudo) |
| F11 / F12 | Window / photo |

Controllers work too (buttons mapped like the Wii U GamePad). Every key can be rebound in the Esc menu.

## Online play

- PLAY connects to a **community server**. It is run by volunteers, has **no guarantee** and may stop at any time.
- Your account is created automatically at the first PLAY and stays linked to your PC (`Game\online_identity.ini`: keep it,
  never share it). There is no password to remember.
- **Friends:** press **F4**, type a friend's pseudo, Enter. They must add you too (your pseudos are unique). You then see each other
  in the game's friend list and can join each other's rooms. Your friend list stays on your PC.
- **Privacy:** the server stores an account number, a derived key (never your password), your pseudo, and the creation and
  last-seen dates; while you play, whether you are online and your game mode are shown only to friends who added you back. Your IP address appears in its logs (kept in 3 rotating files) and is used only for sign-up limits and bans.
  During a match, other players' game connects to yours directly (peer to peer), as on the Wii U, so they can see your IP address.
- To run your own server: [docs/SERVEUR.md](docs/SERVEUR.md).

## Community

Upstream project: **PC-PORTERS / NEVESYA**. To help, DM `sussy_0897` on Discord and read [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
TikTok: [@splatoon1multiplayer](https://www.tiktok.com/@splatoon1multiplayer). If you post about the port, tag @nevesboy67 @77emul and @splatoon1multiplayer.

## Known issues

- Mii faces show as a **white square** in the friend list unless the Mii resources of your own Wii U are present
  (`FFLResHigh.dat` and the 3 other `FFLRes*.dat` files, dumped from your console, in `Game\mlc01\sys\title\0005001b\10056000\content`).
  Setup copies them by itself when it finds them next to your dump. It is only cosmetic.
- Adding a friend works both ways: you each add the other's pseudo (F4).
- **macOS is not supported for now** (no Mac setup, no Mac renderer). Windows is the supported system for this online edition.
- The online mode has only been tested between players on the same network so far; between two different networks it is the next test.

## Why this is legal

This project contains only original source code and open-source libraries (see [LICENSES/](LICENSES/)). You supply the game
from your own console, the conversion happens on your PC, and nothing generated is shared. The runtime re-implements Wii U
system services in original code, like other emulators. This is not legal advice; laws differ between countries.
Never share game files, dumps, saves or the `build` folder.

## Credits

This online edition is built on the port made by the [PC-PORTERS](https://github.com/PC-PORTERS) team on GitHub (NEVESYA and contributors). All the base work (the recompilation, the renderer, the game itself running on PC) is theirs; this branch only adds the online service on top.

RebrewU, [Cemu](https://github.com/cemu-project/Cemu) (shader decompiler, MPL-2.0, and replacement fonts, SIL OFL 1.1), SDL2,
glslang, volk, Vulkan-Headers, zlib, curl, miniaudio, nlohmann/json and Dear ImGui. Full upstream documentation (Linux build,
runtime switches): see the `main` branch.


---

# Splatoon 1 PC Port - Édition en ligne

Un portage PC **non officiel** de **Splatoon (Wii U)** pour Windows : le code du jeu est recompilé en programme natif,
avec un rendu Vulkan. Il poursuit [RebrewU](https://github.com/ApfelTeeSaft/RebrewU) d'ApfelTeeSaft.
Cette branche (`SplatPc-ONLINE`) ajoute la **mise à jour 288** et le **jeu en ligne sur un serveur communautaire**.

> **Aucun code, aucune ressource ni aucun fichier de Nintendo dans ce dépôt.** Il faut ta propre copie du jeu, extraite de ta
> propre Wii U (jeu + mise à jour 288, EUR). Le portage est construit sur ton PC à partir de tes fichiers.
> *Splatoon est une marque de Nintendo. Ce projet n'est ni affilié ni approuvé par Nintendo.*

> **Travail en cours.** Des bugs, saccades et plantages sont à prévoir. Recommandé : carte NVIDIA récente (Vulkan 1.3),
> 16 Go de RAM (32 Go c'est mieux) et environ 40 Go libres pendant l'installation.

## Installation

Suis **[INSTALL-FR.md](INSTALL-FR.md)** (English: [INSTALL.md](INSTALL.md)). En bref :

1. Télécharge cette branche en ZIP et extrais-la dans un chemin court (par exemple `C:\SplatoonPort`).
2. Lance `Setup.bat`, choisis le dump du **jeu**, puis celui de la **mise à jour 288**. La première construction dure 30 à 90 minutes.
3. Lance le jeu avec le raccourci **Splatoon PC Port** : choisis un pseudo et une langue, puis **JOUER**.

Le lanceur cherche les mises à jour au démarrage et ne reconstruit que ce qui a changé.

## Jouer

| Entrée | Action |
|---|---|
| W A S D / Souris | Déplacement / visée |
| Clic gauche / Clic droit | Tirer (ZR) / Forme calamar (ZL) |
| Espace, Q, E | Saut, arme secondaire, spécial |
| F, G | Carte (+), Options (-) |
| Échap | Menu des réglages (graphismes, touches, volume) |
| F4 | Fenêtre des amis (ajouter un ami par son pseudo) |
| F11 / F12 | Fenêtre / photo |

Les manettes fonctionnent aussi (boutons placés comme sur le GamePad). Toutes les touches se changent dans le menu Échap.

## Jeu en ligne

- JOUER se connecte à un **serveur communautaire**, tenu par des bénévoles, **sans garantie**, qui peut s'arrêter à tout moment.
- Ton compte est créé automatiquement au premier JOUER et reste lié à ton PC (`Game\online_identity.ini` : garde-le,
  ne le partage jamais). Aucun mot de passe à retenir.
- **Amis :** appuie sur **F4**, tape le pseudo d'un ami, Entrée. Il doit t'ajouter aussi (les pseudos sont uniques). Vous vous voyez alors
  dans la liste d'amis du jeu et pouvez rejoindre vos salons. Ta liste d'amis reste sur ton PC.
- **Vie privée :** le serveur garde un numéro de compte, une clé dérivée (jamais ton mot de passe), ton pseudo et les dates de création et de
  dernière connexion ; pendant que tu joues, ton état en ligne et ton mode de jeu ne sont montrés qu'aux amis qui t'ont ajouté en retour. Ton adresse IP apparaît dans ses journaux (3 fichiers tournants) et ne sert qu'aux limites d'inscription et aux bannissements.
  Pendant un match, le jeu des autres joueurs se connecte directement au tien (pair à pair), comme sur Wii U : ils peuvent voir ton adresse IP.
- Pour héberger ton propre serveur : [docs/SERVEUR.md](docs/SERVEUR.md).

## Communauté

Projet d'origine : **PC-PORTERS / NEVESYA**. Pour aider, envoie un message privé à `sussy_0897` sur Discord et lis [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
TikTok : [@splatoon1multiplayer](https://www.tiktok.com/@splatoon1multiplayer). Si tu publies à propos du portage, identifie @nevesboy67 @77emul et @splatoon1multiplayer.

## Problèmes connus

- Les visages de Mii apparaissent en **carré blanc** dans la liste d'amis, sauf si les ressources Mii de ta propre Wii U sont présentes
  (`FFLResHigh.dat` et les 3 autres fichiers `FFLRes*.dat`, extraits de ta console, dans `Game\mlc01\sys\title\0005001b\10056000\content`).
  Setup les copie tout seul quand il les trouve à côté de ton dump. C'est purement cosmétique.
- L'ajout d'un ami se fait des deux côtés : chacun ajoute le pseudo de l'autre (F4).
- **macOS n'est pas pris en charge pour l'instant** (pas d'installation ni de rendu pour Mac). Windows est le système pris en charge par cette édition en ligne.
- Le mode en ligne n'a été testé jusqu'ici qu'entre joueurs d'un même réseau ; entre deux réseaux différents, c'est le prochain test.

## Pourquoi c'est légal

Ce projet ne contient que du code source original et des bibliothèques libres (voir [LICENSES/](LICENSES/)). Tu fournis le jeu
depuis ta propre console, la conversion se fait sur ton PC et rien de généré n'est partagé. Le runtime réimplémente les services
système de la Wii U avec du code original, comme les émulateurs. Ce n'est pas un avis juridique ; les lois diffèrent selon les pays.
Ne partage jamais de fichiers du jeu, de dumps, de sauvegardes ni le dossier `build`.

## Crédits

Cette édition en ligne repose sur le portage réalisé par l'équipe [PC-PORTERS](https://github.com/PC-PORTERS) sur GitHub (NEVESYA et contributeurs). Tout le travail de base (la recompilation, le rendu, le jeu qui tourne sur PC) est le leur ; cette branche ajoute seulement le service en ligne par-dessus.

RebrewU, [Cemu](https://github.com/cemu-project/Cemu) (décompilateur de shaders, MPL-2.0, et polices de remplacement, SIL OFL 1.1), SDL2,
glslang, volk, Vulkan-Headers, zlib, curl, miniaudio, nlohmann/json et Dear ImGui. Documentation complète d'origine (build Linux,
variables d'exécution) : voir la branche `main`.

# Installer Splatoon PC (édition en ligne)

Cette version est le portage PC de Splatoon 1 avec la **mise à jour 288** et le **jeu en ligne**
sur un serveur communautaire. Elle ne contient aucun fichier Nintendo : l'installateur construit
le jeu sur ton PC à partir de **tes propres** dumps Wii U.

## 1. Ce qu'il faut

| | |
| --- | --- |
| PC | Windows 10 ou 11 (64 bits), carte graphique NVIDIA (Vulkan 1.3), 16 Go de RAM (32 Go c'est mieux) |
| Disque | environ **50 Go libres** pendant l'installation (environ 8 Go une fois fini) |
| Ton jeu | **Splatoon EUR** dumpé depuis ta propre Wii U : le **jeu** et sa **mise à jour** (v288). Chaque dump est un dossier qui contient `code`, `content` et `meta`. |
| Temps | environ **1 à 2 heures** la première fois (le PC construit le jeu lui-même) |

Les outils manquants (Visual Studio Build Tools, Python 3.12, Git) sont proposés par l'installateur.

## 2. Installer

1. Télécharge ce projet (**Code → Download ZIP**) et décompresse-le dans un **chemin court**,
   par exemple `C:\SplatoonPC`.
2. Double-clique sur **`Setup.bat`**.
   - S'il installe des outils pour toi, ferme la fenêtre quand il le demande et relance
     `Setup.bat`.
3. Quand il le demande, choisis le dossier du dump du **jeu** (celui qui contient `code`,
   `content`, `meta`).
4. Attends la construction du jeu de base (30 à 90 minutes).
5. Quand il le demande, choisis le dossier du dump de la **mise à jour** (288). Attends encore
   (20 à 60 minutes).
6. C'est fini : un raccourci **Splatoon PC Port** est sur ton bureau (ou dans ce dossier si tu as dit non). Pas de raccourci ? Double-clique sur **Play-Online.bat** dans ce dossier : c'est la même chose.

## 3. Jouer

- Double-clique sur le raccourci **Splatoon PC Port**, tape un **pseudo** (2 à 10 lettres ou chiffres)
  et clique sur **PLAY**. **Play offline** lance le jeu sans Internet.
- **Échap** ouvre les réglages (graphismes, touches, souris, volume).
- Garde le fichier `Game\online_identity.ini` : c'est ton compte en ligne sur ce PC.

## 4. Mettre à jour

Quand une nouvelle version sort, le lanceur te prévient au démarrage et propose la mise à jour.
Ta sauvegarde (`Game\save`), tes réglages et ton compte sont gardés.

## 5. En cas de problème

- **L'installateur s'arrête sur une erreur :** fais une capture de la fenêtre et ouvre une
  « issue » sur ce projet, avec ta carte graphique. N'envoie jamais de fichiers du jeu, de dumps
  ni de sauvegardes.
- **« A communication error occurred » en match :** le jeu déconnecte un joueur qui ne bouge
  plus pendant un moment, comme sur Wii U. Sinon, signale l'heure où c'est arrivé.
- **Pas assez de place :** libère au moins 50 Go sur le disque du dossier du projet.

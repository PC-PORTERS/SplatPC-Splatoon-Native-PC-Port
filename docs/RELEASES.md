# Publishing a signed version

The launcher only installs an update whose tag `online-vX.Y.Z` is **signed** by a key listed in `scripts/allowed_signers` of the version the player already has. Without a key there, updates are switched off. Never share the private key.

## One-time setup (maintainer)

1. Make a key used only for signing: `ssh-keygen -t ed25519 -f %USERPROFILE%\.ssh\splatoon_release -C "release"` (give it a passphrase).
2. Add one line to `scripts/allowed_signers`: `release ` followed by the content of `splatoon_release.pub` (for example `release ssh-ed25519 AAAA...`), commit it **before** the first release. It ships with the game, so players trust it from their installed version.
3. Tell git to sign: `git config gpg.format ssh` and `git config user.signingkey %USERPROFILE%\.ssh\splatoon_release.pub`.

## Each version

1. Raise `VERSION`, commit, test.
2. Sign the tag: `git tag -s online-v0.2.0 -m "online-v0.2.0"`.
3. Check it: `git -c gpg.ssh.allowedSignersFile=scripts/allowed_signers verify-tag online-v0.2.0`.
4. Push the branch and the tag only when ready. Protect the tags `online-v*` in the GitHub settings (only maintainers may create them).

To change or revoke a key: publish a version signed by the old key that carries the new `allowed_signers`.

---

# Publier une version signée

Le lanceur n'installe une mise à jour que si son tag `online-vX.Y.Z` est **signé** par une clé listée dans `scripts/allowed_signers` de la version que le joueur a déjà. Sans clé dans ce fichier, les mises à jour sont désactivées. Ne partage jamais la clé privée.

## Une seule fois (mainteneur)

1. Crée une clé réservée à la signature : `ssh-keygen -t ed25519 -f %USERPROFILE%\.ssh\splatoon_release -C "release"` (avec une phrase secrète).
2. Ajoute une ligne à `scripts/allowed_signers` : `release ` suivi du contenu de `splatoon_release.pub` (par exemple `release ssh-ed25519 AAAA...`), et commite-la **avant** la première version. Elle est livrée avec le jeu : les joueurs lui font confiance depuis leur version installée.
3. Dis à git de signer : `git config gpg.format ssh` et `git config user.signingkey %USERPROFILE%\.ssh\splatoon_release.pub`.

## À chaque version

1. Augmente `VERSION`, commite, teste.
2. Signe le tag : `git tag -s online-v0.2.0 -m "online-v0.2.0"`.
3. Vérifie-le : `git -c gpg.ssh.allowedSignersFile=scripts/allowed_signers verify-tag online-v0.2.0`.
4. Envoie la branche et le tag seulement quand tout est prêt. Protège les tags `online-v*` dans les réglages GitHub (seuls les mainteneurs peuvent les créer).

Pour changer ou révoquer une clé : publie une version signée par l'ancienne clé qui contient le nouvel `allowed_signers`.

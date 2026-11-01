# Protocoles NEX observés et non implémentés

**Date** : 28/10/2026. **Source** : 65 journaux `build\diagnostics\*-server.log`
(texte uniquement, jamais les `.dmp`/`.bin`), lignes `RMC request protocol=… method=… unimplemented_protocol=…`.

## Ce que le jeu appelle réellement

| Protocole NEX | Méthodes vues | Nombre | Implémenté |
| --- | --- | --- | --- |
| 3 `MatchMaking` | 3, 4, 5 | 124 | oui (`matchmake.MatchMakingServer`) |
| 10 `Authentication` | 2, 3 | 164 | oui (`nexserver.AuthServer`) |
| 11 `SecureConnection` | 1, 7, 8 | 165 | oui (`nexserver.SecureServer`) |
| 21 `NATTraversal` | 2, 41 | 38 | oui (`matchmake.NATTraversalServer`) |
| 50 `Utility` | 1 | 9 | oui (bibliothèque NintendoClients) |
| 109 `MatchmakeExtension` | 1, 16, 34, 38, 39, 40 | 165 | oui (`matchmake.MatchmakeExtensionServer`) |

## Non implémenté

| Protocole/méthode | Nom | Occurrences | Contexte | Décision |
| --- | --- | --- | --- | --- |
| 109 / 16 | `MatchmakeExtension::GetPlayingSession` | **1** | Reçu sur une connexion qui ne servait pas le protocole 109 (connexion d'authentification, qui n'expose que le protocole 10) | **Rien à implémenter** : la méthode *est* implémentée sur la connexion sécurisée (`get_playing_session`, testée par `test_server_e2e`). Les journaux récents (après l'ajout de `matchmake.servers()`) ne la montrent plus comme non implémentée. |

Aucun autre `unimplemented_protocol=True` dans les 65 journaux. Les méthodes inconnues au sein
d'un protocole implémenté n'apparaissent pas : la bibliothèque répond `RMCError` et le jeu
continue (aucune trace de « unknown method » dans les journaux).

## Méthodes existantes mais jamais appelées (impact probable si un jour appelées)

| Protocole | Méthodes | Ce que le jeu attendrait | Impact probable |
| --- | --- | --- | --- |
| 109 | `CreateCommunity`, `JoinCommunity`, `FindOfficialCommunity`… | gestion des communautés / Splatfest | Splatfest et communautés indisponibles (déjà le cas) |
| 109 | `GetMyBlockList`, `AddToBlockList`, `ReportViolation` | listes de blocage et signalements | menu « signaler » sans effet, pas de crash |
| 109 | `GetFriendNotificationData`, `UpdateNotificationData` | notifications d'amis | amis indisponibles, pas de crash |
| 3 | `FindMatchmakeSessionByOwner/Participant`, `BrowseMatchmakeSession*` | navigation de sessions privées | non utilisé par Splatoon en match classique |
| 21 | `RequestProbeInitiation`, `InitiateProbe` | trous de NAT (PIA) | utilisé seulement si `natf`/`natm` l'exigent ; déjà géré côté natcheck |

## Décision

Aucun ajout n'a été fait : **on ne devine pas les formats**. Les seules méthodes sûres (réponse
vide) seraient les méthodes « set » sans retour, mais aucune n'est appelée aujourd'hui ; les
implémenter sans preuve risquerait de faire croire au jeu qu'une fonctionnalité existe (Splatfest,
amis) et de provoquer un blocage plus loin. Le serveur journalise déjà chaque appel inconnu
(`unimplemented_protocol`) : le prochain journal qui en montre un nouveau suffira à décider.

# How LanNative works

What the mod does, why, and what was learned about Absolver's multiplayer along the way. Addresses (RVA) are for Absolver
1.31 b1.25_575; offsets are from the game's own symbols. Everything here comes from reading the shipped executable and from
two-PC tests; nothing was taken from Sloclap's servers.

## 1. How the game's multiplayer is built
- It is a normal Unreal Engine 4.18 **listen server**: one player's game is the server for the world, others connect over UDP
  (default port 7777) with `open <ip>`. Combat works over the engine's replication plus the game's own "order" RPCs.
- The menus for matchmaking, co-op groups and PvP all depend on **Sloclap's online backend** (login state, a "binder" client).
  That backend is gone, so the menus cannot work. Direct IP travel does not use it, which is what LanNative builds on.
- The join URL carries login options the host reads (`CheckPoint`, `zone`, `LV`, `MLV`, `Token`, `RZ`, `SpawnPoint`, ...).
- There is **no authentication** on the direct path: only "server full" and a splitscreen limit are checked at login.
- Config values that matter: `MaxClientRate = 20000`, `NetClientTicksPerSecond = 30`, `ConnectionTimeout = 30`.
- Host migration code exists (`MigrationAsHost` / `MigrationAsClient`) but is switched off in the shipped config.

## 2. The loader
- `version.dll` is a **proxy**: the game imports `version.dll`, so it loads from the game folder first; every export is forwarded
  to the real one in System32.
- It **inline-hooks** a few game functions by address. Each hook checks the original bytes first and refuses to patch if they
  differ, so a different game build is left alone. All work runs on the game thread inside SEH; a part that faults repeatedly switches
  itself off for the run.
- It does nothing unless the command line contains `-NoEAC`, and never touches EasyAntiCheat.
- Game logic lives in `lan_logic.c` and talks to the engine only through a small function table, so it is tested offline against fake
  memory (including freed objects that fault on access). All settings are in one `config.txt`.

## 3. Joiner and host fixes
| Problem | Cause | Fix |
|---|---|---|
| Joiner cannot walk | Its own pawn arrives as a simulated proxy (`Role` 1) | Set `Role` 1 -> 2 on arrival (hook on `ClientRestart`) |
| Joiner sees a copy of its character | Joining does not reload the world, so the old pawn stays; also the *previous session's* pawn after leave + rejoin (`Role` 3, no controller) | Sweeps after joining remove locally-owned pawns of ours, including controller-less ones |
| Joiner cannot respawn | Host sends remote players away when `m_bCanSpawnAlone` is set or their respawn zone differs from the host's | Host clears the flag and keeps `m_uiRespawnZoneId` equal to its own zone |
| Crash when a player dies or leaves | Co-op voice chat code reads a null `PlayerState` | Clear `m_bSupportCoopVoiceChat` |
| Leaving player's body stays on the host | The game only reclaims it on rejoin | Hook on `OnNetCleanup` plus a periodic pass remove it |
| F6/F7 by accident drops a joiner | - | Joiners need a second press within 5 s |

## 4. Duels (1v1 / 3v3) offline
The duel maps load, but the game assumes the online backend in four places:
1. **Sent back to the main map.** The PvP controller calls `BPF_ReturnToPreviousMap` (RVA 0x3C08B0) when the online status is wrong.
   Hook: skip that call for about 45 s after a travel or a change in player count, only in duel maps.
2. **Match never starts.** Offline, the player count comes from the backend list (1) instead of the real connections (2).
   `GameInstance.m_bDebugFlow` (+0x6D90) makes the game use real counts; LanNative sets it on the PC running the duel and restores it.
3. **No end screen.** For 1v1 the game waits for a verdict byte from the backend (`cBinder` +0x5C90: 1 victory, 2 defeat, 3 draw).
   LanNative writes it locally once the winner is known and clears it afterwards. Rematch then works.
4. **3v3 waits for 6 players.** `AAdversarialGameMode.m_iNumPlayersPerTeam` (+0x554) is lowered to 1 by default.

Related: **auto-leave** returns you to your own game when the other player's connection is gone (the game keeps a dead
connection about 30 s; LanNative treats 10 s without packets as gone, so it takes about 13 s). **NPC kills do not score**: an
NPC's AI controller has no player state, so `AAdversarialPlayerState::ScoreKill` (RVA 0x3E20E0) was recorded with no victim and
counted for the match; the hook drops any kill whose victim is not a real player's player state.

## 5. The Forsaken parry bug
`UDefenseComponent::SetParryWindow` (RVA 0x34D6C0) pairs the two parry-property weak pointers wrongly (+0x1F4 vs +0x1FC). The
first parry after a respawn, or after being parried, therefore finds a null pointer and silently skips the stamina return, the
availability layer and the guard-gauge gain. Fix: change one byte (`FC` -> `F4` at RVA 0x33F472), applied in memory at start.
Full write-up: `forsaken-parry-bug-findings.md`. A read-only log line records for every parry whether the pointer is valid.

## 6. NPC spawning (host only)
The training dummy is not replicated, but every other AI character Blueprint is. The mod creates its own `AAISpawner` next to
the host's character (`UWorld::SpawnActor`), sets its character class (`m_SpawningClass`, taken from the game's AI manager),
no-respawn, and the chosen archetype asset, then calls `BPF_WantsSpawn`. The game's own spawn code does the rest (controller,
behaviour tree, equipment). It works in the open world and in duel maps, any number of NPCs. Bosses and minibosses use their own
character Blueprint (optional 4th column of `npcs.txt`); if that class produces nothing, the generic AI character is tried, then the
level's own `BP_AISpawner_Calbot`. Every engine address used is byte-checked before first use. Details: `npc-spawn-research.md`.

## 7. Hosting over a VPN
By default the host listens on every adapter, so a joiner can use the host's VPN address. `hostip` in `config.txt` makes the host
bind one address through the engine's `-MULTIHOME=` option (written into the in-memory command line only while hosting).

## 8. What the telemetry showed
Read-only logging (`telemetry_*.csv`, `lannative.log`) on a two-PC LAN session:
- Round-trip lag about 14 ms median, rare spikes above 40 ms.
- The host's inbound rate sits almost exactly at **20,000 bytes/s**, which is the configured `MaxClientRate`; the client runs at
  about 330 fps against the host's 60 and sends about 300 packets/s. Hypothesis (not yet tested): capping the client's frame rate
  would lower the packet rate.
- After a rejoin the old connection stays on the host for 44-61 s before the engine drops it.

## 9. Testing
- Offline: `lannative/test/logic_test.c` (368 checks) runs the logic against fake memory; freed objects fault on access, and key
  checks were verified by breaking the code on purpose and seeing them fail.
- In game: everything above was tried on two PCs. The release notes list what changed per version.

## 10. Open items
- Netcode experiment above; a closer look at which host functions a joiner can trigger (there is no login check).
- Respawn at a chosen altar (needs the save's checkpoint, which the mod deliberately does not touch).
- 3v3 played to the end, and the game's own debug menu / console commands are untested or not done.
- An NPC that kills a player in a duel still counts as that player's death.

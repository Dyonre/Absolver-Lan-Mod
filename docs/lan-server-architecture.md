# How Absolver's multiplayer works (from the decompiles), as it matters for LAN

Sources: `absolver_lan*_decomp.c` (names files `tools/ghidra/names_absolver_lan.txt`), symbol searches with `tools/symtool`,
`tools/disasm/find_field_access.py` (finds which code touches a struct offset), crash dumps. Everything here is static analysis plus
what the two-PC tests showed; items marked (unverified) have not been seen in the game.

## Shape of the system
- **Listen servers per zone.** One player's game is the server for its area (`HostPvPGame` builds `<map>?listen`). The world is split
  into zones; `AThePlainesGameMode` tracks `m_ControllersPerZone`, `AddControllerToZone`, `RemoveControllerFromZone`, and each
  controller reports its zone with `Server_ChangingZone`. A joiner keeps the NPCs of the zone it stands in; it does not reload a map
  when joining (same world address before and after).
- **Login options in the join URL** (`AThePlainesGameMode::InitNewPlayer`, `USCGameEngine::LoadMap`, `SCloginOptions`): `CheckPoint`
  (altar), `zone`, `LV`, `MLV`, `Token`, `RZ`, plus `SpawnPoint`, `bJoinWithPassword`, `bIsFromInvite`.
- **"Online" is Sloclap's backend, not the engine's online subsystem.** `BPF_IsOnline` = `!m_bOfflineMode && m_eOnlineStatus == LOG_IN`,
  driven by the start-up login flow and a `Binder` (`UThePlainesGameInstance::m_Binder`, `[HostMigration]` Engine.ini keys: UseBinder,
  CloudDNSName, MentoringDNSName, TrackingDNSName, RedirectorDNSName, ForceClient/ForceServer/ForceRelay, SpawnAlone). The menus
  (matchmaking, social, coop groups) all depend on it, so they cannot work on LAN; direct IP travel bypasses them.
- **Host migration is built in.** `USCGameEngine::MigrationAsHost/MigrationAsClient`, `StayAsHostCallBack`,
  `AThePlainesGameMode::OnHostMigration`, `AFightingCharacter::InitializeForServerHostMigration/InitializeForClientHostMigration`,
  `AFightingPlayerController::SpawnOnHostMigration`, `UZoneTransitionComponent::OnMigrationAsHost/Client`. When the host of a zone
  leaves, a client is meant to take over the world (unverified on LAN; the config key `[HostMigration] UseHostMigration` is False in
  DefaultEngine.ini). A possible later feature: keep the session alive when the host quits.

## Disconnect handling and why a joiner's body stays on the host
- The engine is customised: `AActor::SetPossessTimeOut(float)`, `AActor::CheckPossessTimeOut()`, `AActor::PossessTimeOutDuration()`,
  `bIsActorNetworkLinkBroken` (+0x2C9) and `AActor::SetActorNetworkLinkStatus(bool)` (the only writer of that byte, virtual, no direct
  callers).
- `AFightingCharacter::PossessTimeOutDuration`: AI 10 s; player 5 s by default; 10 s when the game instance is a client of the host it
  matches; 0 s when the game instance is the host and AcceptPlayers is false. `CanPossessTimeOut` = "pawn is not the local master".
- `CheckPossessTimeOut` destroys the actor only if `bIsActorNetworkLinkBroken` is set. In vanilla that flag is set by the host-migration
  code when the link to a host is lost, so a pawn can survive a migration. A plain LAN disconnect never sets it, so the host's copy of
  the departed player stays until something else removes it. The game's own reclaim happens when the same character joins again
  (`RequestSpawnNewPawn`, `NotifyPendingForPawnReconciliation`, `m_FightingCharacterGuid`), which is why the body vanishes on rejoin.
- `AFightingPlayerController::OnNetCleanup` / `PawnLeavingGame` are stock behaviour (destroy controller, destroy pawn) but the
  controller was observed lingering after the connection dropped, with a dangling `Pawn` pointer. v14 therefore never reads
  `pc.Pawn` of a disconnected controller: the host records the pawn's path name while the joiner is connected and removes that
  actor by name when the connection is gone (v13 read the stale pointer first and never reached the saved name).

## Respawn and zones (confirmed in game)
`UHealthComponent::TryAskForRespawn` -> `WantSwitchZoneOnRespawn`: a remote player is sent away (black screen) when killed by a non-coop
player while `m_bCanSpawnAlone` is set, or when `AFightingPlayerState.m_uiRespawnZoneId` differs from the host's
`USCLocalPlayer.m_uiCurrentZone`. The mod clears the flag and keeps the respawn zone equal.

## Crashes seen
| when | where | cause |
|---|---|---|
| joiner, host died | `AFightingPlayerController::OnPlayerRelationshipChanged`, null PlayerState (+0x3C0) | coop voice chat branch; fixed by clearing `AThePlainesGameState.m_bSupportCoopVoiceChat` |
| host, joiner on quit, joiner on game close | inside `UE4SS.dll` (RVA 0x4BAAAF / 0x512AC1 / 0x4BDDAE), under a Blueprint event or tick, reading garbage (a float's bits as an address) | UE4SS 3.0.1 touching freed objects/properties. v10-v13 removed every cached UObject pointer. The close crash at 06:0x still happened with v13, so either UE4SS's custom-property or hook bookkeeping misbehaves at teardown, or another UE4SS mod does. Not resolved. |
| joiner, v10 | hang in `K2_DestroyActor` inside a callback | fixed by `SetLifeSpan` |

To separate causes: run once with only LanDirect enabled (turn the other UE4SS mods to 0 in mods.txt), once with LanDirect off.
UE4SS 3.0.1 is old (Feb 2024); a newer UE4SS release is the other obvious thing to try.

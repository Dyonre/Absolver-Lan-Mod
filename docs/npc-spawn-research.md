# Replicated NPC spawn for a LAN listen host (research only)

Build b1.25_575 (UE 4.18), `-NoEAC`, offline. Nothing was run in game, nothing under tools\lannative, dist\ or the game folder changed.
Tags: **[C]** confirmed from decompile/disassembly/UAssetAPI dump, **[I]** inferred, needs an in-game test. RVAs are image-relative.
Evidence: `work\structs_npc.txt` / `structs_npc2.txt` (PrintStruct via `work\run_printstruct_npc*.ps1`), `work\npc_decomp.c` `npc_decomp2.c`
`npc_decomp3.c` `npc_decomp4.c` (Ghidra; `run_npc_decomp3/4.ps1`, names `names_npc_repl*.txt`), `work\debugmenu_bc.txt` (DebugMenu bytecode),
UAssetAPI dumps of the .umap/.uasset files (scratch tools `...\scratchpad\cdo`, `...\scratchpad\umap`).
Previous report: `docs\npc-spawn-research.md` (training dummy is non-replicated; still valid, corrected/extended below).

## 0. Short answer

1. **Every AI character Blueprint except the training dummy is replicated**, because `ABaseCharacter::ABaseCharacter` (0x319560) sets
   `bReplicates` (byte +0x87 |= 0x20) and no BP overrides it. Only `BP_AICharacter_Training_C` overrides replication to off. [C]
2. **The game already has a replicated hostile-NPC path that the host can trigger with 3 native calls**: a placed spawner
   `BP_AISpawner_Calbot` (tag `Calbot`, archetype `/Game/DB/AI/NPCs/Trickster/Trickster`, `OnAnEvent`) sits in the always-loaded sublevel
   `MainMap_HUB_LD` of `MainMap_Main`. The DebugMenu "SpawnCalbot" button does exactly `GetAllActorsWithTag("Calbot")` then `BPF_WantsSpawn`. [C]
   Move that spawner (`SetActorLocation`, root is Movable) next to the host, then `BPF_WantsSpawn(spawner, null, 0, -1)`.
   The pawn is a normal open-world enemy: class `BP_AICharacter_C`/`_W_C`, `Faction2`, 500 HP, not invincible, attacks and defends. [C]
3. **Duel maps (1v1/3v3/CrossArena) have no AI zone volume, no AISpawner, and (except Opti_GM_1v1_City_Cross and the 3v3 maps) no NavMeshBoundsVolume.** [C: byte scan of the .umaps and
   their sublevels] An NPC can be spawned there with our own `AAISpawner`, but its behaviour tree uses `BTTask_MoveTo` and EQS, so without a navmesh it may stand still. [I]
4. Replication to the joiner needs nothing special: actor channel + standard distance relevancy (no game-specific `IsNetRelevantFor`). The archetype
   is replicated as `m_ArchetypeDescription`, so the joiner needs only the cooked assets it already has. [C for the replicated properties, I for end-to-end behaviour]
5. Smallest in-game experiment: section 7.

## 1. AI character Blueprints and flags

CDO overrides read with UAssetAPI (`Default__<BP>_C`), parents via ClassExport SuperStruct. Flags not listed are inherited from
`ABaseCharacter`: bReplicates=true [C], bNetLoadOnClient/bOnlyRelevantToOwner/bReplicateMovement = engine defaults for a `Character` (true/false/true) [I, no BP overrides them].

| Asset (`Content\Blueprints\...`) | Class : parent | Replication flags | Other CDO overrides / use |
|---|---|---|---|
| `AI\BP_AICharacter` | `BP_AICharacter_C` : `BP_TPSCharacter_C` | **replicated** (inherited) | `m_eFaction=Faction2`, `AutoPossessAI=Spawned`, `AIControllerClass=BPFightingAiController_C`, `m_bSpawnOccured=True`. Generic open-world enemy (male). `UAIManager.m_AiClassToSpawn[0]` |
| `AI\BP_AICharacter_W` | `BP_AICharacter_W_C` : `BP_AICharacter_C` | replicated | female variant, `m_AiClassToSpawn[1]` (BPAIManager CDO) |
| `AI\BP_AICharacter_MiniBoss(_W)` | : `BP_AICharacter_C` | replicated | used by `BP_AISpawner_MiniBoss` / Coliseum miniboss spawners (`m_SpawningClass` set) |
| `AI\Boss\BP_AICharacter_Boss` (+`_T1/_T2/_T3`, `_Kuretz`, `_Risryn`, `_CargalKilnor`) | : `BP_AICharacter_C` | replicated | bosses; spawners in MainMap_Main (Risryn), Coliseum (Kuretz). Not sparring material (phases, scripted) |
| `AI\BP_AICharacter_NoFight(_W)` | : `BP_AICharacter_C` | replicated | dialog NPCs, `FightingAITree_NoFight`; `MainMap_LD_NPC` (14 spawners). Never attack |
| **`AI\BP_AICharacter_Training`** | : `BP_AICharacter_C` | **`bReplicates=False`, `bReplicateMovement=False`, `bNetLoadOnClient=False`, `bOnlyRelevantToOwner=True`** | training dummy (invincible archetype `TrainingAI`, cannot attack/defend). Local per machine, as in the old report |
| `BP_StartUpCharacter` | `BP_StartUpCharacter_C` : `FightingCharacter` | replicated (inherited) | not an AI pawn (`AutoPossessAI=Disabled`, controller `FightingAIController`); referenced by DebugMenu only as a cast target [I on purpose]. Not the Calbot |
| `BP_TPSCharacter` | : `FightingCharacter` | replicated | base of the AI BPs, `m_eFaction=TMP_Neutral` |

Archetype data (`Content\DB\AI\...`, UArchetypeAsset): `NPCs\Trickster\Trickster` = 500 HP, `m_uiCombatStyle=1`, 1 equipment selection, sex `None`, not invincible, attack/defend default true,
`m_Behavior` unset so the BP's `FightingAITree` is used. `NPCs\Grunt\Forest_Grunt_v1` = 250 HP. `TrainingAI` = invincible, no attack/defend. Other NPC archetype folders:
Absorb, Agile, Avoid, Cadencer, Distancer, Evil, Grunt, Guard, Harasser, Jumper, LegSlow, Miner, Parry, Pusher, Sammo, Slow, Spartan, Spinner, Trickster, `*_Weaponized`, `*_MiniBoss`, `Tutorial\Tuto_AI_*`, `Bots\AI_Classic_Hard`. [C names]
**Best sparring candidate: Trickster** (human-like, 500 HP, fights back, same thing the game's own debug "Calbot" spawns). Tuto_AI_Nothing might be a passive dummy (not opened) [I].

## 2. How the open world spawns them

Placed actors (counts from the umaps, all in sublevels that `MainMap_Main` lists as `LevelStreamingAlwaysLoaded`, including all `*_LD` levels, so their archetypes stay resident) [C]:
`BP_AISpawner_C` (HUB 1 = Calbot, Coliseum 39 + 2 miniboss, Forest_Path 29, Temple_Stock 39, LD_NPC 14, Tutorial 17...), `AIZoneVolume` (7-9 per region), `SpawnerGroup` (7-11 per region), `WeaponSpawner*`.

- A normal spawner has `m_SpawningClass` null (except boss/miniboss/NPC spawners), `m_Level[level][i]` = archetype list, `m_eFirstSpawnMethod` default in `BP_AISpawner_C` = `AtTheBeginning`.
  `AAISpawner::Tick` (0x2D88E0) spawns once when `CanSpawn()` and the actor has begun play. Zone queue path: `ASpawnerGroup::SpawnAiFromDesc` (0x2D7670) -> `UAIManager::RequestSpawn` (0x2D1B10) -> `GS->m_ToSpawnFrom` (FSpawnerPerZone) -> `UAIManager::UpdateSpawners` (0x2E0740) -> `SpawnAI`. [C]
- `UAIManager` is **not** a world object: it is `UThePlainesGameInstance+0x6868 (m_AIManager)` (class `BPAIManager_C`, ticking object). [C: PrintStruct + two decompiles] It exists in every world of the session, only needs a null check.
  `m_AiClassToSpawn[2]` at `UAIManager+0x78` = {BP_AICharacter_C, BP_AICharacter_W_C}. `UAIManager::GetAISpawningClassForGender(world, sex)` (0x2C6350) returns `[sex]` via `world+0x140 (OwningGameInstance)`. [C]
- Minimal data for `SpawnAI` (0x2D6180, decompile in npc_decomp.c): (a) an archetype (`m_ArchetypeOverride`, or the parameter, or `GetArchetype(level)` from `m_Level`);
  **if none is found the whole class-selection block is skipped and `SpawnActor(null class)` yields nothing**; (b) a character class: `m_SpawningClass` if it derives `ABaseCharacter`, else
  `GameMode->vfunc(+0x9D0)(sex)` (AThePlainesGameMode::GetAIClassFromSex), else `AIManager.m_AiClassToSpawn[sex]`; (c) a world and a root component (spawn transform = spawner root component
  `ComponentToWorld` at root+0x180, Z += capsule half-height * archetype scale). Team/faction: the class default (`Faction2`); controller: `AutoPossessAI=Spawned` + `BPFightingAiController_C`. No deck/level data is needed beyond the archetype (its combat deck/equipment live inside the archetype asset). [C]
- After `SpawnActor` the game calls: pawn vfunc +0x2C0 (1), `UAIComponent->vfunc +0x3B8 = Spawned(spawner, archetype, level, equipIdx, sex)` (UAIComponent::Spawned 0x2D78E0 / UAIFightingComponent::Spawned 0x2D7B10),
  sets `+0x2E4` idxInSpawner, `BindDelegatesOnAi` (0x2C2160), `AddCanBeHittedFunction` lambda, copies `m_ZoneOverride` into the AI, `InitAI(true)` (vfunc +0x3A8), then `BPE_OnRespawnFinished` + `AISpawnedDelegate`. [C]
  `Spawned` sets `UAIComponent::m_Spawner` and writes `m_ArchetypeDescription`; on the server it calls `Server_SetArchetypeData` (RPC to itself). `StartBehaviorTreeIfNeeded` (0x2D7C20) runs the tree from the archetype's or BP's `m_Behavior` via the AI controller's `UBehaviorTreeComponent`. [C]
- `AIManager` registration (`RegisterAIInZone` 0x2D06F0) is only called from `AFightingCharacter::InitializeForServerHostMigration`; `OnEndPlayAI` only removes from `m_AisPerZone`. **No GameState/AIManager registration is needed to make the AI run.** [C callers via symtool]
- `AAISpawner::BeginPlay` (0x2C1870) with `m_ZoneOverride==null` fires an async overlap at its location for an `AAIZoneVolume`; none found = `m_AIZone` stays invalid. `UAIComponent::InitAI` also tries `GetOverlappingActors(AIZoneVolume)`, and keeps going if none. [C]

## 3. Concrete minimal call sequence (game thread, e.g. from the `UGameEngine_Tick` hook LanNative already has)

All functions are x64 MS ABI (rcx, rdx, r8, r9 then stack). Existing helpers in `tools\lannative\lannative.c` (`world()` = `*(UWorld**)(base+0x38A5890)`, `api_set_location` 0x1247310,
`api_destroy` 0x122F9C0, `api_weak_get/set` 0x8C4240/0x8B9D70, GUObjectArray scan 0x37A5F20, `GetPathName` 0x8C6C20) can be reused unchanged.

### 3A. Open world, no new actors (recommended first)
```
sp = find in GUObjectArray (live items) the object whose GetPathName ends ":PersistentLevel.BP_AISpawner_Calbot"
     (class name BP_AISpawner_C; fall back: any AAISpawner whose Tags contain "Calbot")
gate: world NetMode != Client, *(u8*)(sp+0x120) == 3 (ROLE_Authority), GameState is AThePlainesGameState
FVector p = hostPawnLocation + forward*400 (+ Z margin; SpawnAI itself adds capsule half-height)
AActor::SetActorLocation(sp, &p, 0, NULL, 0)                  // 0x1247310, root comp is Movable (ctor calls SetMobility(2))
AAISpawner::BPF_WantsSpawn(sp, /*arch*/NULL, /*level*/0, /*idx*/-1)   // 0x2C1730: void(AAISpawner*, UArchetypeAsset*, int, int)
```
`BPF_WantsSpawn` = `if (CanSpawn() && *(u8*)(this+0x4C8)==2 /*OnAnEvent*/) { UnSpawnAi(); SpawnAI(this,arch,level,idx); }` [C disasm 0x2C1730]. `m_eFirstSpawnMethod` of `BP_AISpawner_Calbot` is `OnAnEvent` [C umap export 38].
`CanSpawn` (0x2C2BB0): `m_SpawningClass` null here -> returns `Role==Authority`. The archetype comes from the placed `m_Level` ([0][0] = `/Game/DB/AI/NPCs/Trickster/Trickster`). Result in `sp+0x53C` (`m_AISpawned`, weak ptr; use `api_weak_get`).
Calling again respawns (it unspawns the old one first). Cleanup: `AAISpawner::UnSpawnAi(sp)` (0x2DCDE0, void(this)): disables collision, `AActor::Destroy(pawn,0,1)`, clears weak ptr, `UAIManager::RequestUnSpawn`.
Same object also works with a non-null archetype argument or `SetArchetypeOverride`.

### 3B. Own spawner (needed in duel maps or if the Calbot spawner is not found)
```
UClass* cSp  = AAISpawner::GetPrivateStaticClass()          // 0x5614F0 (ClassFlags 0x10000000 = native, NOT abstract) [C]
UClass* cNpc = *(UClass**)(*(u8**)(*(u8**)(world+0x140) + 0x6868) + 0x78)   // UAIManager.m_AiClassToSpawn[0]; null-check every hop (GI must be UThePlainesGameInstance)
UArchetypeAsset* arch = GUObjectArray scan for object path "/Game/DB/AI/NPCs/Trickster/Trickster.Trickster"; if absent (duel maps) StaticLoadObject (0x8D1C10, ABI not yet verified, check with disasm.py)
FActorSpawnParameters prm (0x30 bytes): call ctor 0x17E6D40(&prm); *(u8*)(prm+0x28)=2 /*AdjustIfPossibleButAlwaysSpawn*/; *(u16*)(prm+0x2A) |= 2 /*bNoFail*/   // exactly what SpawnAI does [C]
FVector loc; FRotator rot;
sp = UWorld::SpawnActor(world, cSp, &loc, &rot, &prm)       // 0x1507AA0 overload (this, UClass*, FVector*, FRotator*, FActorSpawnParameters*) -> AActor*; main overload 0x15070B0 takes FTransform* (0x30 B: quat, translation, scale; identity quat 0,0,0,1)
*(UClass**)(sp+0x480) = cNpc                                // m_SpawningClass (field is a plain pointer; check IsA ABaseCharacter first, CODEX rule)
AAISpawner::SetArchetypeOverride(sp, arch)                  // 0x2D58A0 -> FWeakObjectPtr::operator=(sp+0x5A8, arch)
(m_eFirstSpawnMethod is already OnAnEvent in the native ctor: no write needed)
AAISpawner::BPF_WantsSpawn(sp, NULL, 0, -1)
```
Notes: native `AAISpawner` has `m_bCanRespawn=true`; death triggers `RespawnAsked` -> timer -> `SpawnTimer` respawn; set `*(u8*)(sp+0x4BC)=0` right after SpawnActor to stop it, or destroy the spawner on death. [C ctor / RespawnAsked path]
Because the spawner is a *dynamic* actor, `USCSaveGameComponent::OnRegister` (0x4D55B0) adds it to `AThePlainesLevel::m_actorsToSave` (only when the level script is `AThePlainesLevel`, so MainMap yes, duel maps no); `OnUnregister` removes it on destroy. Destroy the spawner when done and avoid saving while it exists. [C, effect on save file I]
Struct offsets (PrintStruct, `work\structs_npc.txt`):
- `AAISpawner` size 0x5C0: `AIDownDelegate +0x3A0`, `AISpawnedDelegate +0x410`, `m_SpawningClass +0x480`, `m_Archetypes_DEPRECATED +0x488`, `m_Level +0x498 (TArray<FAILevel>)`, `m_EquipmentSelection +0x4A8`,
  `m_eSex +0x4B0`, `m_eFaction +0x4B1`, `m_fRespawnTime +0x4B4`, `m_fSpawnDelay +0x4B8`, `m_bCanRespawn +0x4BC`, `m_bUseRespawnFxAtSpawn +0x4BD`, `m_TimerRespawn +0x4C0`, `m_eFirstSpawnMethod +0x4C8` (0 AtTheBeginning, 1 WithATimer, 2 OnAnEvent), `m_RootComp +0x4D0` (also `AActor::RootComponent +0x170`),
  `m_ZoneOverride +0x4D8`, `m_SaveGameComponent +0x530`, `m_bLooted +0x539`, `m_AISpawned +0x53C`, `m_AIZone +0x544`, `m_bHasSpawned +0x54C`, `m_Stream +0x5A0`, `m_ArchetypeOverride +0x5A8`, `m_PendingSpawn +0x5B0`. `AActor::Role +0x120`.
- `FSpawnerPerZone` size 0x1C: `m_Spawner +0` (weak), `m_Archertype +8` (weak), `m_iLevel +0x10`, `m_uiZone +0x14`, `m_iWeight +0x18`. `AThePlainesGameState::m_ToSpawnFrom +0x628`, `m_TrainingLocation +0x560`, `m_TrainingAISpawner +0x568`.
- `UAIManager` size 0xF0: `m_fCloseRangeDist +0x30`, `m_fMidRangeDist +0x34`, `m_uiMidRangeMaxAI +0x38`, `m_GameState +0x5C`, `m_AiClassToSpawn +0x78 (TSubclassOf[2])`, `m_AisPerZone +0x88`, `m_AiToUnspawn +0xD8`. Owner: `UThePlainesGameInstance +0x6868`; `UWorld+0x140 = OwningGameInstance`.
- `AFightingCharacter` size 0x1F90: `m_AIComponent +0x1C30` (UAIFightingComponent*) (there is no `AAICharacter`; the `AAIBaseCharacter` PDB name is a folded 0x1200-byte alias of `ABaseCharacter`). `UAIComponent` size 0x148: `OnSpawned +0xF0`, `m_Spawner +0x100` (replicated InitialOnly), `m_PlayerControllerRequester +0x108`,
  `m_AiZone +0x110`, `m_ArchetypeDescription +0x118` (replicated), `m_FightingClass +0x128`, `m_Behavior +0x130`, `m_uiMatchMakingZone +0x138` (replicated), `m_iLevel +0x13C`, `m_bCanBeHitted +0x140`. `UArchetypeAsset::m_Behavior +0x1B8`.
- Enum values: `ECharacterSex::None` takes the sex from the archetype's equipment selection (random if the selection says "any").

### What the game does *not* need from us
GameState registration, AIManager registration, controller creation (engine does it in `SpawnActor` via `AutoPossessAI=Spawned`; `AFightingAIController::Possess` 0x3A0220 binds perception, blackboard, hit/guard/death delegates), `RequestSpawn` queue. Do NOT write `m_ToSpawnFrom`. [C]

## 4. Replication to the joiner

- Server side: pawn is a dynamic replicated actor (bReplicates [C]), so it gets an actor channel per connection when relevant. Relevancy is the stock `AActor::IsNetRelevantFor`/`APawn::IsNetRelevantFor` (distance via NetCullDistance, bAlwaysRelevant false); the game defines no override of its own (symbol search) [C]. The joiner must be near it to see it (the standard open-world behaviour).
- Replicated NPC state: `UAIComponent` replicates `m_ArchetypeDescription` (archetype asset reference + equipment index), `m_Spawner` (InitialOnly) and `m_uiMatchMakingZone` (`GetLifetimeReplicatedProps` 0x2C8090, [C]); `UAIFightingComponent` has its own list (0x2C8480). `OnRep_Archetype`
  (0x2CEC40) re-runs `InitFromArchetype` on the client to apply health, scale and equipment. `AFightingCharacter` replicates the usual combat/health components (same as any open-world enemy).
- `m_Spawner` on the client: a level-placed spawner (3A) resolves by its stable name. A runtime-spawned spawner (3B) is **not** replicated (AActor default), so the client gets a null `m_Spawner`; `UAIComponent::OnRep_Spawner` (0x2CEE30) and `UAIFightingComponent::OnRep_Spawner` return early / null-check before using it, but the client-side sex (+0x12D8 write) and perception whitelist tweak
  are skipped [C code, I on visible effect: maybe default idle/appearance differences]. To avoid that, either use 3A, or `AActor::SetReplicates(sp,1)` (0x1249D30, `void(this, bool)`; only acts when Role==3) right after SpawnActor so the joiner also gets the spawner. [I]
- Joiner needs locally: `BP_AICharacter_C`/`_W_C`, the archetype asset, the BT/EQS assets, all cooked into the same paks on both PCs (same build) so nothing to ship; the class is loaded from the replicated class path, the archetype from its asset path. [I]
- Authority: `CanSpawn` forbids replicated AI on a client (`Role==Authority` or non-dedicated + non-replicated class). Joiner presses nothing; it only receives. The joiner hits the NPC like any enemy; damage is server-validated. [I, co-op open world was a shipped feature]
- Cleanup/weak pointers: keep only the spawner as `FWeakObjectPtr`/`m_AISpawned` (weak). On map travel the world destroys all actors; our static pointer to the spawner is invalidated by the weak check (`api_weak_get` returns null). Cap: 1 NPC (`BPF_WantsSpawn` already unspawns the previous), 5 s cooldown, destroy spawner on world change and on host/leave hooks LanNative already has, permanent off after 2 SEH faults.

## 5. Duel arenas and the DebugMenu path

**Duel maps** (checked by byte scan of every root .umap plus all `1v1\Sub-Levels` / `3v3\Sub-Levels`) [C]: no `AIZoneVolume`, no `AISpawner`, no `RecastNavMesh`/`NavMeshBoundsVolume` except `Opti_GM_1v1_City_Cross(_Night)` (NavBounds in the root) and the five `3v3` roots (`GM_3v3_Domination_*` etc.). `MainMap_CrossArena.umap` has none of them either. Training.umap and Zoo_Training.umap do have NavBounds/Recast.
What works/breaks (3B only; there is no spawner to move): [I unless noted]
- Spawning succeeds (SpawnActor on any world; `UAIManager` is on the GameInstance, so exists; archetype must be loaded: `StaticLoadObject`).
- Zone: AI has no `m_AiZone`; `InitAI` tolerates it [C]. Without a zone the open-world logic that gates AI to occupied zones (`UpdateSpawners`) is irrelevant since we bypass the queue.
- NavMesh: the shipped tree (`FightingAITree`) uses `BTTask_MoveTo`, `BTTask_MoveDirectlyToward` and `BTService_RunEQS (FindPlayer/FindStatue)` [C]. Without a navmesh the pathing parts fail; the NPC may only stand and attack when the player walks into range. Best arenas: City_Cross and the 3v3 maps.
- PvP rules: `AAdversarialGameMode` counts player controllers (`UpdateNumConnectedPlayers`), the AI has an `AIController`, not a PlayerController, so match flow should be unaffected; an enemy-faction NPC will probably attack both players. Kills of the NPC may grant XP or hit "friendly fire" rules (`m_bFriendlyFire +0x554` in GameState). Unverified.
- The `1V1_GameMode`/PvP return-to-main-map behaviour from the earlier LAN notes is independent of the NPC.
- The training sublevel is not loaded there, and the stock "Start training" flow must not be used for sparring (it teleports the host and sets invincibility, see the old report).

**DebugMenu "SpawnCalbot"** [C bytecode, `work\debugmenu_bc.txt`]: `DebugMenu_C` (class under `UI\Menu\Debug`, native parent `UDebugMenu`) button `SCButton_SpawnCalbot` -> `ExecuteUbergraph_DebugMenu(14265)` -> `GetAllActorsWithTag(<AISpawner class>, "Calbot")` -> for each `AISpawner::BPF_WantsSpawn(null archetype, ...)` -> `PrintString("Trickster (Calbot) spawned in HUB")` (PrintString itself is stripped in this build).
- It is the same replicated path as 3A (the spawner is the HUB placed actor, so the NPC appears **in the HUB area at (-42465, -22874, -580)**, not next to the player; `m_SpawningClass` null -> BP_AICharacter(_W)_C from the AIManager). Works only on the host (`CanSpawn` needs Role==Authority); on a joiner it silently does nothing. [C]
- Only the HUB (MainMap_HUB_LD) has a Calbot-tagged spawner; no other map does (`Calbot` string appears in that umap and in the DebugMenu only; the other hits in 3v3 Garden/Temple LA and Tutorial_Graph were not decoded and are not spawners in the class scan). [C partly]
- Triggering it: `AFightingPlayerController::BPE_ToggleDebugMenu` (UFunction, Z_Construct 0x5AE650) via `FindFunction` + `ProcessEvent` (0x8A8280 / 0x8ADC30) opens `DebugMenu_C`; clicking a button needs UI input, we cannot call the button from native without a widget reference, so for NPC spawning **our own 3A call is simpler and better** (also lets us place the NPC next to the host). Whether the shipped build still honours `BPE_ToggleDebugMenu` or the gamepad combo (LB+RB+LT+RT) is unknown [I]; try the combo once, it costs nothing.

## 6. Design proposal (not implemented)

Host-only, `-NoEAC`-only, key-triggered, local-only input (no remote command channel):
1. Gate: NoEAC active, NetMode ListenServer/Standalone, local controller valid, GameState `AThePlainesGameState`, not already spawned, cooldown, fault budget (same pattern as existing pvp/telemetry features).
2. Open world: mode 3A. Pick the Calbot spawner by path/tag from GUObjectArray (live item check `object_item_is_live`), class-check `IsA AAISpawner`, `Role==3`, then SetActorLocation + `BPF_WantsSpawn`. Log: spawner ptr, root location before/after, `m_AISpawned` pointer + path, `m_SpawningClass`, NetMode, SEH result.
3. Duel / no Calbot spawner: mode 3B behind a separate switch (`npc.txt`: off|world|any), only after 3A proved the call chain. Verify `world+0x140` is a `UThePlainesGameInstance`, AI class non-null and derives `ABaseCharacter`, archetype non-null.
4. Wrap in `__try/__except`; never keep raw pointers across frames (weak ptr via `api_weak_set/get`); max 1 NPC; unspawn on key toggle, world change, host/leave; unit-testable guard logic in `lan_logic.c` with fake objects.
5. Menu entry: add to the planned LanNative menu (docs\lannative-gui-menu-design.md item 10 "Own NPC spawn") as "Spawn Trickster sparring partner / Remove NPC".

## 7. Risks and smallest experiment

| # | Risk | Mitigation / note |
|---|---|---|
| 1 | Moving a placed spawner affects its own save state (`USCSaveGameComponent`, `m_bLooted`) | spawner is OnAnEvent, `m_bCanRespawn=false` in BP; restore the original location when removing the NPC |
| 2 | Calbot spawner's `m_SpawningClass` null relies on GameMode vfunc +0x9D0 then AIManager; if both fail the spawn silently produces nothing | log `m_AISpawned` valid after the call; fall back to 3B with an explicit class |
| 3 | `BPF_WantsSpawn` is not harmless when `CanSpawn` false (nothing happens) | gate on Role==3 and log CanSpawn (0x2C2BB0) result first |
| 4 | Level-placed spawner might be referenced by level-script events (Calbot hub event or tutorial); an out-of-place NPC may confuse scripted logic | only HUB uses it, DebugMenu is the only caller found in assets (bytecode/umap strings) |
| 5 | NPC kills/XP/loot give the host progression and might desync save; AI death -> `OnAiDeath`/`SpawnLootIfNeeded` (0x2D7740) | accepted for a sparring tool, document it |
| 6 | Client has null `m_Spawner` in 3B | use 3A or SetReplicates on the spawner |
| 7 | Duel map without nav: idle NPC | choose City_Cross or 3v3 first |
| 8 | AI hostile to both players in PvP, may break match rules | restrict 3B to non-match test sessions |
| 9 | Build lock-in | byte-check each RVA prologue before use like the other hooks |
| 10 | EAC | NoEAC only |

**Smallest in-game experiment** (cheapest first):
0. Offline, no code: gamepad hold LB+RB+LT+RT on a normal map; if the DebugMenu opens, press "SpawnCalbot", walk to the HUB spawner location and see whether a Trickster appears, and whether a joined client sees and can hit it.
1. Code experiment, one key (host, `MainMap_Main`, joiner connected nearby): log-only first (find spawner, print ptrs and `CanSpawn`); second press: `SetActorLocation` + `BPF_WantsSpawn`; third press: `UnSpawnAi`. Pass criteria: host sees NPC and it attacks, joiner sees it, both can hit it, health drops on both screens, `UnSpawnAi` removes it on both, no log faults. Then travel to another map and confirm no crash.
2. Only after that: 3B in `City_Cross` (host + joiner) to see whether AI moves without a hub navmesh.

// LAN session game logic, as plain C over raw game memory.
// It talks to the engine only through the Api table below, so it can run against fake memory.
#pragma once
#include <stdint.h>

// engine layout (Absolver 1.31, build b1.25_575; from the game's PDB)
#define OFF_OBJ_CLASS        0x10    // UObject::ClassPrivate
#define OFF_OBJ_FLAGS        0x08    // UObject::ObjectFlags (exclude class defaults and archetypes during scans)
#define OFF_STRUCT_SUPER     0x30    // UStruct::SuperStruct
#define OFF_ACTOR_REMOTEROLE 0x88
#define OFF_ACTOR_ROLE       0x120
#define OFF_ACTOR_ROOTCOMP   0x170
#define OFF_ROOT_C2W_POS     0x190   // USceneComponent::ComponentToWorld (0x180) + FTransform::Translation (0x10)
#define OFF_CTRL_PAWN        0x3A8   // AController::Pawn
#define OFF_PAWN_CONTROLLER  0x3D8   // APawn::Controller (confirmed through APawn::execGetController)
#define OFF_CTRL_PLAYERSTATE 0x3C0   // AController::PlayerState
#define OFF_PC_PLAYER        0x408   // APlayerController::Player (ULocalPlayer on the owner, UNetConnection on the server)
#define OFF_PLAYER_CONTROLLER 0x30    // UPlayer::PlayerController (set by UPlayer::SwitchController)
#define OFF_PS_RESPAWNZONE   0x468   // AFightingPlayerState::m_uiRespawnZoneId
#define OFF_LP_ZONE          0x240   // USCLocalPlayer::m_uiCurrentZone
#define OFF_GS_COOPVOICE     0x60E   // AThePlainesGameState::m_bSupportCoopVoiceChat
#define OFF_GI_LOCALPLAYERS  0x38    // UGameInstance::LocalPlayers (TArray)
#define OFF_GI_CANSPAWNALONE 0x6C60  // UThePlainesGameInstance::m_bCanSpawnAlone
#define OFF_W_AUTHGM         0xF0    // UWorld::AuthorityGameMode (non-null only on the server)
#define OFF_W_GAMESTATE      0xF8
#define OFF_W_GAMEINST       0x140
#define OFF_W_PCLIST         0x188   // UWorld::PlayerControllerList (TArray<TWeakObjectPtr<APlayerController>>)

#define ROLE_SIM  1
#define ROLE_AUTO 2
#define ROLE_AUTH 3

typedef struct { int32_t idx, serial; } WeakPtr;   // FWeakObjectPtr
typedef struct { WeakPtr pc, pawn; } CleanupPair;
typedef void (*ObjectVisitor)(void *object, void *context);

typedef struct {
    void *(*weak_get)(WeakPtr *w);                 // FWeakObjectPtr::Get: NULL once the object is gone
    void  (*weak_set)(WeakPtr *w, void *obj);      // FWeakObjectPtr::operator= (NULL clears)
    uint8_t (*destroy)(void *actor);               // AActor::Destroy(actor, false, true), C++ bool returned in AL
    void *(*cls_localplayer)(void);                // USCLocalPlayer::GetPrivateStaticClass
    void *(*cls_gamestate)(void);                  // AThePlainesGameState
    void *(*cls_gameinst)(void);                   // UThePlainesGameInstance
    void *(*cls_playerstate)(void);                // AFightingPlayerState
    void *(*cls_fightingchar)(void);               // AFightingCharacter
    void  (*scan_objects)(ObjectVisitor visit, void *context); // live GUObjectArray entries, game thread only
    uint8_t (*set_location)(void *actor, const float xyz[3]); // AActor::SetActorLocation, C++ bool returned in AL
    void *(*world)(void);                          // the current UWorld
    void  (*log)(const char *fmt, ...);
    // telemetry (read-only)
    int   (*object_path)(void *obj, char *out, int cap);   // UObject::GetPathName as UTF-8 (0 = unavailable); a class object gives its class path
    void *(*cls_netdriver)(void);                  // UNetDriver
    void *(*cls_netconn)(void);                    // UNetConnection
    uint64_t (*now_ms)(void);                      // monotonic milliseconds
    void  (*csv)(int file, const char *line);      // append one line to telemetry CSV `file` (header written when the file is new)
    void  (*wall)(char *out, int cap);             // local wall clock "YYYY-MM-DD HH:MM:SS.mmm"
    void *(*cls_defense)(void);                    // UDefenseComponent (parry observer)
    // LAN-PvP flow and match probe
    void *(*cls_advgm)(void);                      // AAdversarialGameMode
    void *(*cls_advgs)(void);                      // AAdversarialGameState
    void *(*cls_advps)(void);                      // AAdversarialPlayerState
    void *(*cls_fightingpc)(void);                 // AFightingPlayerController
    int   (*fname_text)(uint64_t fname, char *out, int cap);   // FName::ToString as UTF-8 (0 = unavailable)
    uint64_t (*fname_waitingpost)(void);           // the engine's global FName MatchState::WaitingPostMatch (0 = unavailable)
} Api;

// duel result
#define OFF_GI_BINDER        0x6DA8  // UThePlainesGameInstance::m_Binder (cBinder *, the Sloclap backend client; exists offline)
#define OFF_BINDER_RESULT    0x5C90  // cBinder: the backend's 1v1 verdict byte (1 victory, 2 defeat, 3 draw); read by BPF_GetMatchResult and IsWaitingForMatchValidation
#define OFF_ADVGS_CACHESTATE_L 0x7B0 // AAdversarialGameState::m_CacheMatchState (FName)
#define OFF_ADVGS_WINNER     0x7A4   // AAdversarialGameState::m_winnerInfos.m_iTeamIndex (-1 = no winner yet)
#define OFF_ADVPS_TEAM       0x45C   // AAdversarialPlayerState team index

#define OFF_GI_DEBUGFLOW     0x6D90  // UThePlainesGameInstance::m_bDebugFlow (the game's own developer flag; read only by the duel game mode)

#define OFF_ADVGM_NUMTEAMS   0x550   // AAdversarialGameMode::m_iNumTeams
#define OFF_ADVGM_PERTEAM    0x554   // AAdversarialGameMode::m_iNumPlayersPerTeam (expected players = teams * this; GetGameModeExpectedPlayerCount)

#define OFF_CHAR_DEFENSE    0x1BE8  // AFightingCharacter::m_DefenseComponent (read by NotifyParrySuccessful at +0x7A)

void  L_init(const Api *api);
void  L_clientrestart_pre(void *pc, WeakPtr *old_pawn);                    // before the original ClientRestart_Implementation
int   L_clientrestart_post(void *pc, void *new_pawn, WeakPtr *old_pawn);   // after it; 1 = schedule joiner clone sweeps
void  L_serverack_post(void *pc, void *pawn);                              // after ServerAcknowledgePossession_Implementation
void  L_netcleanup_pre(void *pc, CleanupPair *leaving);                    // before APlayerController::OnNetCleanup
void  L_netcleanup_post(CleanupPair *leaving);                             // after it
void  L_periodic(void);                                                    // every 30 frames
int   L_sweep_joiner_clones(void);                                         // one bounded live-object sweep after joining
int   L_is_joiner(void);
int   L_guard_leave(uint64_t *armed_at, uint64_t now_ms, const char *what);// double-press guard for menu Host/Join on a joiner (1 = proceed)
void  L_summon(void);                                                      // host: bring joiners next to you
void  L_state(void);                                                       // log a state dump
int   L_ps_is_player(void *ps);                                            // is this the PlayerState of a real player (not NULL, not an NPC)? Server only
void *L_local_pc(void);                                                    // this PC's own APlayerController (NULL when none)
int   L_teleport_local(const float xyz[3]);                                 // move this PC's own character (host / single player only); 1 = done
// Parry observer: NotifyParrySuccessful(this = the parried attacker's component, ..., actor = the PARRIER) reads the ParryPropertyDB weak
// pointer from the PARRIER's defense component. This inspects exactly that pointer, read-only. Returns 1 = the rewards pointer is valid,
// 0 = it is not (the Forsaken bug), -1 = not inspectable (the reason is written into out). Never writes.
int   L_parry_inspect(void *parrier, int fix_applied, char *out, int cap);
// LAN-PvP flow, every frame: while this PC runs the duel game mode in a 1v1/3v3 map, holds GameInstance.m_bDebugFlow = 1 (why: lan_logic.c above
// L_pvp_flow) and restores it afterwards. Returns 1 while the flag is held by us.
int   L_pvp_flow(int enabled);
// A 3v3 map waits for 2 teams x 3 players = 6 (ReadyToStartMatch needs NumPlayers >= that), so two players never start. With n in 1..2
// the same hold-and-restore as the debug flow lowers AAdversarialGameMode.m_iNumPlayersPerTeam to n on this PC's 3v3 game mode only (n = 0 or 3 = leave
// the game's value). Takes effect through L_pvp_flow, which the caller already runs every frame.
void  L_set_perteam(int n);
int   L_perteam_held(void);
void  L_set_soloduel(int on);                      // 1 = a 1v1 or 3v3 map this PC hosts expects only one player (map testing without a second PC)
int   L_soloduel_held(void);
// Called every frame on every PC. For 1v1 the game shows the end screen and continues the post-match flow only after the Sloclap backend has
// delivered the match verdict into cBinder (+0x5C90); offline it never does, so IsWaitingForMatchValidation stays true and BPF_GetMatchResult stays
// "none". While a duel map is loaded, the match is in WaitingPostMatch and a winner team exists (GameState +0x7A4), this writes that byte on THIS PC:
// 1 (victory) when the local player's team is the winner team, else 2 (defeat), only if it is 0 now; it puts 0 back when the post-match state ends.
// Returns 1 while the byte is held by us.
int   L_pvp_result(int enabled);

// NPC spawning. The host creates its own AAISpawner next to its character and
// asks it to spawn an AI character from a chosen archetype asset; the game's own spawner code does the rest (class, controller, behaviour tree).
// Works in every map where this PC is the server (open world, duel maps). Offsets are from the PDB structure dumps.
#define OFF_SPAWNER_CLASS      0x480   // AAISpawner::m_SpawningClass (TSubclassOf<ABaseCharacter>)
#define OFF_SPAWNER_CANRESPAWN 0x4BC   // m_bCanRespawn
#define OFF_SPAWNER_METHOD     0x4C8   // m_eFirstSpawnMethod (2 = OnAnEvent, the only method BPF_WantsSpawn accepts)
#define OFF_SPAWNER_AISPAWNED  0x53C   // m_AISpawned (FWeakObjectPtr to the character)
#define OFF_GI_AIMANAGER       0x6868  // UThePlainesGameInstance::m_AIManager
#define OFF_AIMGR_CLASSES      0x78    // UAIManager::m_AiClassToSpawn[sex] (BP_AICharacter_C, BP_AICharacter_W_C)

typedef struct {
    void *(*cls_aispawner)(void);                                   // AAISpawner::GetPrivateStaticClass
    void *(*cls_archetype)(void);                                   // UArchetypeAsset::GetPrivateStaticClass
    void *(*spawn_actor)(void *world, void *cls, const float loc[3]); // UWorld::SpawnActor with the game's own spawn parameters; NULL = failed
    uint8_t (*can_spawn)(void *spawner);                            // AAISpawner::CanSpawn
    void  (*wants_spawn)(void *spawner, void *archetype);           // AAISpawner::BPF_WantsSpawn(spawner, archetype, level 0, index -1)
    void  (*unspawn)(void *spawner);                                // AAISpawner::UnSpawnAi
    void  (*set_replicates)(void *actor, int on);                   // AActor::SetReplicates
    void  (*set_archetype)(void *spawner, void *archetype);         // AAISpawner::SetArchetypeOverride
    void *(*load_object)(void *cls, const char *path);              // StaticLoadObject(cls, NULL, path): NULL when it does not exist
    void *(*spawn_actor_rot)(void *world, void *cls, const float loc[3], const float rot[3]);   // like spawn_actor with an FRotator (pitch, yaw, roll degrees); optional (NULL = no yaw)
} NpcApi;

void L_npc_init(const NpcApi *api);
void L_npc_set_replicated(int on);                                  // 1 = also replicate the spawner actor itself to the joiner (default 0)
int  L_npc_spawn(const char *archetype_path, const char *label, const char *class_path); // 1 = an NPC now exists. Host / single player only; every call adds one more NPC.
                                                                    // class_path (NULL or "") = a character Blueprint class to use instead of the game's generic AI character (bosses, minibosses); if it yields nothing the generic one is tried
int  L_npc_spawn_at(const char *archetype_path, const char *label, const char *class_path, const float *at, const float *yaw_deg, void **out_ch, void **out_sp);
                                                                    // like L_npc_spawn, but the spawner is created AT `at` (NULL = 350 uu in front of the local character; then a local character is needed) facing yaw_deg (NULL = 0);
                                                                    // out_ch / out_sp (may be NULL) receive the new character and the spawner that made it
int  L_npc_unspawn(void *spawner);                                  // UnSpawnAi + destroy one spawner from L_npc_spawn_at (the level's own Calbot is put back instead); 1 = done
int  L_npc_remove_all(void);                                        // removes every NPC we spawned (and our spawners); returns how many spawners were cleaned
int  L_npc_live(void);                                              // tracked NPCs that still exist

// LanNative: LAN host/join for Absolver without UE4SS (offline / LAN play only; no EAC or online-service involvement).
//
// This DLL is installed AS version.dll next to Absolver-Win64-Shipping.exe. The game imports version.dll, Windows loads this copy
// (version.dll is not a KnownDLL), every export is forwarded to the real System32 copy (version_exports.h), and the code below
// patches the game's own functions with inline hooks (same technique as tools/sifuhook, tested on this build).
//
// Milestone 1 (this file): loader + log + a hook on UGameEngine::Tick that runs our work on the game thread every frame,
// F6 = host ("<map>?listen"), F7 = join (<ip>[:port][options] from LanNative\ip.txt), F5 = state dump.
// Everything runs in the game's own thread, inside a __try block, so there are no timers, no Lua and no callbacks from other threads.
//
// Offsets/RVAs are for Absolver 1.31 b1.25_575 (the Steam build); the loader verifies the original bytes before patching.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>
#include "version_exports.h"
#include "lannative_targets.h"
#include "lan_logic.h"
#include "lan_telemetry.h"
#include "noeac.h"
#include "parryfix.h"
#include "objectscan.h"
#include "console.h"
#include "joinurl.h"
#include "pvpgate.h"
#include "parrylog.h"
#include "menu.h"
#include "hostip.h"
#include "config.h"

#define RVA_GENGINE         0x38A2E68u   // UEngine *GEngine
#define RVA_GWORLD          0x38A5890u   // UWorldProxy GWorld (first 8 bytes = UWorld *)
#define IDX_TICK            0
#define IDX_CLIENTRESTART   1
#define IDX_SERVERACK       2
#define IDX_SETCLIENTTRAVEL 4
#define IDX_NETCLEANUP      6
#define IDX_RETURNPREV      7            // UThePlainesGameInstance::BPF_ReturnToPreviousMap (bool, this, one pointer argument)
#define IDX_POSTRENDER      9            // UGameViewportClient::PostRender (this, UCanvas *): M4.0 menu text
#define IDX_SCOREKILL       10           // AAdversarialPlayerState::ScoreKill (this, victim PlayerState, points): M4.3 NPC kills do not score in duels
#define IDX_NOTIFYPARRY     8            // UDefenseComponent::NotifyParrySuccessful (this, float, AActor *, FName, uchar)
#define RVA_WEAK_GET        0x8C4240u    // FWeakObjectPtr::Get
#define RVA_WEAK_SET        0x8B9D70u    // FWeakObjectPtr::operator=
#define RVA_ACTOR_DESTROY   0x122F9C0u   // AActor::Destroy(actor, bNetForce, bShouldModifyLevel)
#define RVA_SET_LOCATION    0x1247310u   // AActor::SetActorLocation(actor, FVector&, bSweep, FHitResult*, ETeleportType)
#define RVA_PARRY_FIX       0x33F46Fu    // add rcx, 0x1FC; FC byte becomes F4
#define RVA_GUOBJECTARRAY    0x37A5F20u  // FUObjectArray (PDB); ObjObjects.Items +0x10, NumElements +0x1C, item size 24
#define RVA_CLS_LOCALPLAYER 0x6199A0u    // USCLocalPlayer::GetPrivateStaticClass
#define RVA_CLS_GAMESTATE   0x63F320u    // AThePlainesGameState
#define RVA_CLS_GAMEINST    0x634620u    // UThePlainesGameInstance
#define RVA_CLS_PLAYERSTATE 0x5B9B80u    // AFightingPlayerState
#define RVA_CLS_FIGHTINGCHAR 0x5A31A0u    // AFightingCharacter
#define RVA_CLS_VIEWPORT     0x185EC00u   // UGameViewportClient::GetPrivateStaticClass
#define RVA_STATIC_CONSTRUCT 0x8CFB50u    // StaticConstructObject_Internal
#define RVA_CLS_NETDRIVER    0x18D1400u   // UNetDriver::GetPrivateStaticClass
#define RVA_CLS_NETCONN      0x18D1350u   // UNetConnection::GetPrivateStaticClass
#define RVA_CLS_DEFENSE      0x594080u    // UDefenseComponent::GetPrivateStaticClass
#define RVA_CLS_ADVGM        0x55BAD0u    // AAdversarialGameMode::GetPrivateStaticClass
#define RVA_CLS_ADVGS        0x55BB80u    // AAdversarialGameState::GetPrivateStaticClass
#define RVA_CLS_ADVPS        0x55BC30u    // AAdversarialPlayerState::GetPrivateStaticClass
#define RVA_CLS_FIGHTINGPC   0x5AD4D0u    // AFightingPlayerController::GetPrivateStaticClass
#define RVA_FNAME_TOSTRING   0x7447D0u    // FName::ToString(const FName *this, FString *out)
#define RVA_FNAME_WAITINGPOST 0x38838D8u  // global FName MatchState::WaitingPostMatch (8 bytes: index + number)
#define RVA_GETPATHNAME      0x8C6C20u    // UObjectBaseUtility::GetPathName(this, FString *ret, StopOuter) - by-value wrapper, zeroes *ret itself
#define RVA_FMEMORY_FREE     0x67C830u    // FMemory::Free(void *)
#define OFF_ENGINE_CONSOLECLASS 0xF8u     // UEngine::ConsoleClass (PDB)
#define OFF_ENGINE_VIEWPORT     0x720u    // UEngine::GameViewport (PDB)
#define RVA_CMDLINE_BUF         0x366D090u // FCommandLine::CmdLine (wchar_t[0x4000]); FCommandLine::Get() returns exactly this buffer
#define RVA_CMDLINE_INIT        0x366D012u // FCommandLine::bIsInitialized
#define CMDLINE_CAP             0x3FFF     // characters the engine keeps (FCommandLine::Set copies 0x3FFF)
#define OFF_ENGINE_MEDIUMFONT   0x70u     // UEngine::MediumFont (PDB, work/structs_console.txt)
#define OFF_CANVAS_DRAWCOLOR    0x38u     // UCanvas::DrawColor (FColor B,G,R,A; read by UCanvas::DrawText at +0x38)
#define RVA_NPC_CLS_SPAWNER     0x5614F0u  // AAISpawner::GetPrivateStaticClass
#define RVA_NPC_CLS_ARCHETYPE   0x561E80u  // UArchetypeAsset::GetPrivateStaticClass
#define RVA_NPC_SPAWNACTOR      0x1507AA0u // UWorld::SpawnActor(this, UClass *, const FVector *, const FRotator *, const FActorSpawnParameters &)
#define RVA_NPC_PARAMS_CTOR     0x17E6D40u // FActorSpawnParameters::FActorSpawnParameters (0x30 bytes)
#define RVA_NPC_WANTS_SPAWN     0x2C1730u  // AAISpawner::BPF_WantsSpawn(this, UArchetypeAsset *, int level, int index)
#define RVA_NPC_CAN_SPAWN       0x2C2BB0u  // AAISpawner::CanSpawn
#define RVA_NPC_UNSPAWN         0x2DCDE0u  // AAISpawner::UnSpawnAi
#define RVA_NPC_SET_ARCHETYPE   0x2D58A0u  // AAISpawner::SetArchetypeOverride
#define RVA_NPC_SET_REPLICATES  0x1249D30u // AActor::SetReplicates(this, bool)
#define RVA_NPC_LOAD_OBJECT     0x8D1C10u  // StaticLoadObject(cls, outer, name, filename, loadFlags, sandbox, bAllowReconciliation)
#define RVA_CANVAS_DRAWTEXT     0x17CE110u // UCanvas::DrawText(UFont*, const FString&, x, y, xscale, yscale, const FFontRenderInfo&) - ABI from the disassembly
#define OFF_VIEWPORT_CONSOLE    0x38u     // UGameViewportClient::ViewportConsole (PDB)
#define OFF_GI_CURRENTSAVE       0x6970u  // UThePlainesGameInstance::m_CurrentSave
#define OFF_SAVE_SAVEDDATA       0xA8u    // USaveThePlaines::m_SavedData
#define OFF_SAVED_SPAWNPOINT     0xAE8u   // FAbsolverSaveData::m_SpawnPoint (FString)
#define OFF_SAVED_ZONE           0xAF8u   // FAbsolverSaveData::m_MatchmakingZone
#define TRAVEL_ABSOLUTE     0
#define DEFAULT_PORT        7777
#define MAP_URL             L"/Game/Maps/MainMap/MainMap_Main?listen"

static uint8_t *g_base;
static wchar_t g_dir[MAX_PATH];          // the LanNative folder next to the DLL, with a trailing backslash
static CRITICAL_SECTION g_lock;
static volatile LONG g_disabled;         // set if our own code ever faults: the game then runs untouched
static int g_clone_sweep;
static uint64_t g_clone_sweep_due;
static int g_console_state;              // 0 = wait, 1 = installed/present, 2 = failed (do not retry)
static TelCfg g_tel_cfg;                 // LanNative\telemetry.txt (off / basic / verbose [seconds])
static int g_tel_faults, g_tel_off;      // telemetry has its own fault budget so a fault there never switches the LAN fixes off
static uint64_t g_exe_size, g_self_base, g_self_size;   // for naming the caller of a travel request
static volatile uint64_t g_last_travel_ms;               // GetTickCount64 of the latest client-travel request (0 = none yet)
static volatile uint64_t g_last_peer_change_ms;          // GetTickCount64 of the latest change in the number of connected peers (0 = none yet)
static int g_peers_seen = -1;                            // peer count at the last look (-1 = never looked)
static int g_pvpfix_on = 1;                              // LanNative\pvpfix.txt: send-back guard (on by default)
static int g_pvpflow_on = 1;                             // LanNative\pvpfix.txt: duel debug flow (on by default, off with "guard")
static int g_flow_faults;                                // the duel flow switches itself off after 2 faults
static int g_result_faults;                              // the duel result writer switches itself off after 2 faults
static int g_parry_fix_applied;                          // the one-byte parry fix is in memory (for the parry observer's wording)
static int g_parrylog_faults;                            // the parry observer switches itself off after 3 faults
static unsigned g_parry_seen, g_parry_ok;                // parries observed / with a valid pointer

// ---------------------------------------------------------------------------------------------------------------------
// logging (plain Win32 so it is safe from DllMain too)
static void logf_(const char *fmt, ...) {
    char msg[1024], line[1100];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    wchar_t path[MAX_PATH];
    EnterCriticalSection(&g_lock);
    swprintf(path, MAX_PATH, L"%slannative.log", g_dir);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, line, (DWORD)n, &w, NULL); CloseHandle(f); }
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------------------------------------------------
// inline hooks: copy the stolen bytes into a trampoline, then jmp [rip+0] to the replacement (14 bytes)
static void emit_abs_jmp(uint8_t *p, uint64_t dest) { p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0; memcpy(p + 6, &dest, 8); }

static void *hook(int idx, void *replacement) {
    const Target *t = &g_targets[idx];
    uint8_t *target = g_base + t->rva;
    if (memcmp(target, t->bytes, t->stolen) != 0) {
        char got[80] = "", *g = got;
        for (unsigned i = 0; i < t->stolen && i < 16; i++) g += sprintf(g, "%02X ", target[i]);
        logf_("hook %s: live bytes differ from this build (%s) - NOT patched", t->name, got);
        return NULL;
    }
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("hook %s: VirtualAlloc failed", t->name); return NULL; }
    memcpy(tramp, target, t->stolen);
    emit_abs_jmp(tramp + t->stolen, (uint64_t)(target + t->stolen));
    DWORD old;
    if (!VirtualProtect(target, t->stolen, PAGE_EXECUTE_READWRITE, &old)) { logf_("hook %s: VirtualProtect failed", t->name); return NULL; }
    emit_abs_jmp(target, (uint64_t)replacement);
    for (unsigned i = 14; i < t->stolen; i++) target[i] = 0xCC;
    VirtualProtect(target, t->stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, t->stolen);
    logf_("hook %s: installed at base+0x%X", t->name, t->rva);
    return tramp;
}

// ---------------------------------------------------------------------------------------------------------------------
// game access
typedef void (*Tick_t)(void *self, float dt, uint8_t idle);
typedef void (*SetClientTravel_t)(void *engine, void *world, const wchar_t *url, int travelType);
static Tick_t g_orig_tick;
typedef void (*ClientRestart_t)(void *pc, void *new_pawn);
typedef void (*ServerAck_t)(void *pc, void *pawn);
typedef void (*NetCleanup_t)(void *pc, void *conn);
static ClientRestart_t g_orig_clientrestart;
static ServerAck_t g_orig_serverack;
static NetCleanup_t g_orig_netcleanup;
static SetClientTravel_t g_orig_settravel;
typedef uint8_t (*ReturnPrev_t)(void *gi, void *arg);              // UThePlainesGameInstance::BPF_ReturnToPreviousMap
static ReturnPrev_t g_orig_returnprev;

static void *engine(void) { return *(void **)(g_base + RVA_GENGINE); }
static void *world(void)  { return *(void **)(g_base + RVA_GWORLD); }

// engine function table for lan_logic.c
static void *api_weak_get(WeakPtr *w) { return ((void *(*)(WeakPtr *))(g_base + RVA_WEAK_GET))(w); }
static void  api_weak_set(WeakPtr *w, void *o) { ((void (*)(WeakPtr *, void *))(g_base + RVA_WEAK_SET))(w, o); }
static uint8_t api_destroy(void *a) { return ((uint8_t (*)(void *, int, int))(g_base + RVA_ACTOR_DESTROY))(a, 0, 1); }
static uint8_t api_set_location(void *a, const float v[3]) { return ((uint8_t (*)(void *, const float *, int, void *, int))(g_base + RVA_SET_LOCATION))(a, v, 0, NULL, 0); }
static void *api_cls_lp(void) { return ((void *(*)(void))(g_base + RVA_CLS_LOCALPLAYER))(); }
static void *api_cls_gs(void) { return ((void *(*)(void))(g_base + RVA_CLS_GAMESTATE))(); }
static void *api_cls_gi(void) { return ((void *(*)(void))(g_base + RVA_CLS_GAMEINST))(); }
static void *api_cls_ps(void) { return ((void *(*)(void))(g_base + RVA_CLS_PLAYERSTATE))(); }
static void *api_cls_fc(void) { return ((void *(*)(void))(g_base + RVA_CLS_FIGHTINGCHAR))(); }
static void *api_cls_viewport(void) { return ((void *(*)(void))(g_base + RVA_CLS_VIEWPORT))(); }
static void api_scan_objects(ObjectVisitor visit, void *context) {
    uint8_t *array = g_base + RVA_GUOBJECTARRAY;
    uint8_t *items = *(uint8_t **)(array + 0x10);
    int capacity = *(int32_t *)(array + 0x18);
    int count = *(int32_t *)(array + 0x1C);
    if (!items || count < 0 || count > capacity || capacity > 10000000) {
        logf_("clone sweep: invalid GUObjectArray items=%p count=%d capacity=%d", items, count, capacity);
        return;
    }
    for (int i = 0; i < count; i++) {
        uint8_t *item = items + (size_t)i * 24;
        if (object_item_is_live(*(uint32_t *)(item + 8)) && *(void **)item) visit(*(void **)item, context);
    }
}
static void *api_world(void) { return world(); }

// telemetry accessors (read-only)
static void *api_cls_netdriver(void) { return ((void *(*)(void))(g_base + RVA_CLS_NETDRIVER))(); }
static void *api_cls_netconn(void) { return ((void *(*)(void))(g_base + RVA_CLS_NETCONN))(); }
static uint64_t api_now_ms(void) { return GetTickCount64(); }
static void *api_cls_defense(void) { return ((void *(*)(void))(g_base + RVA_CLS_DEFENSE))(); }
static void *api_cls_advgm(void) { return ((void *(*)(void))(g_base + RVA_CLS_ADVGM))(); }
static void *api_cls_advgs(void) { return ((void *(*)(void))(g_base + RVA_CLS_ADVGS))(); }
static void *api_cls_advps(void) { return ((void *(*)(void))(g_base + RVA_CLS_ADVPS))(); }
static void *api_cls_fightingpc(void) { return ((void *(*)(void))(g_base + RVA_CLS_FIGHTINGPC))(); }
static void api_wall(char *out, int cap) {
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(out, (size_t)cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

typedef struct { wchar_t *data; int num, max; } FStringRaw;    // TArray<wchar_t> as laid out by UE 4.18
static int api_object_path(void *obj, char *out, int cap) {
    if (!obj || cap < 2) return 0;
    FStringRaw s = { 0, 0, 0 };
    char wide_utf8[1024];
    int ok = 0;
    ((FStringRaw *(*)(void *, FStringRaw *, void *))(g_base + RVA_GETPATHNAME))(obj, &s, NULL);
    if (s.data && s.num > 1 && s.num < 4096) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data, s.num - 1 > 400 ? 400 : s.num - 1, wide_utf8, (int)sizeof wide_utf8 - 1, NULL, NULL);
        if (n > 0) {
            if (n > cap - 1) n = cap - 1;
            memcpy(out, wide_utf8, (size_t)n); out[n] = 0; ok = 1;
        }
    }
    if (s.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(s.data);
    return ok;
}

static uint64_t api_fname_waitingpost(void) { return *(uint64_t *)(g_base + RVA_FNAME_WAITINGPOST); }
static int api_fname_text(uint64_t fname, char *out, int cap) {        // FName (index, number) as UTF-8 through the engine's own FName::ToString
    if (cap < 2) return 0;
    FStringRaw s = { 0, 0, 0 };
    char tmp[512];
    int ok = 0;
    ((FStringRaw *(*)(const void *, FStringRaw *))(g_base + RVA_FNAME_TOSTRING))(&fname, &s);
    if (s.data && s.num > 1 && s.num < 512) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data, s.num - 1, tmp, (int)sizeof tmp - 1, NULL, NULL);
        if (n > 0) { if (n > cap - 1) n = cap - 1; memcpy(out, tmp, (size_t)n); out[n] = 0; ok = 1; }
    }
    if (s.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(s.data);
    return ok;
}

static void api_csv(int file, const char *line) {
    static const char *names[] = { "telemetry_frames.csv", "telemetry_conns.csv", "telemetry_players.csv" };
    static const char *heads[] = { TEL_HDR_FRAMES, TEL_HDR_CONNS, TEL_HDR_PLAYERS };
    if (file < 0 || file > 2) return;
    wchar_t path[MAX_PATH];
    EnterCriticalSection(&g_lock);
    swprintf(path, MAX_PATH, L"%s%S", g_dir, names[file]);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w; LARGE_INTEGER size;
        if (GetFileSizeEx(f, &size) && size.QuadPart == 0) { WriteFile(f, heads[file], (DWORD)strlen(heads[file]), &w, NULL); WriteFile(f, "\r\n", 2, &w, NULL); }
        WriteFile(f, line, (DWORD)strlen(line), &w, NULL); WriteFile(f, "\r\n", 2, &w, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_lock);
}

static const Api g_api = { api_weak_get, api_weak_set, api_destroy, api_cls_lp, api_cls_gs, api_cls_gi, api_cls_ps, api_cls_fc, api_scan_objects, api_set_location, api_world, logf_,
                           api_object_path, api_cls_netdriver, api_cls_netconn, api_now_ms, api_csv, api_wall, api_cls_defense,
                           api_cls_advgm, api_cls_advgs, api_cls_advps, api_cls_fightingpc, api_fname_text, api_fname_waitingpost };

// ---------------------------------------------------------------------------------------------------------------------
// M4.2 NPC spawning: the engine calls (the logic is in lan_logic.c). Every address is byte-checked once before the first use.
typedef struct { uint32_t rva; const char *what; int n; uint8_t bytes[20]; } NpcCheck;
static const NpcCheck g_npc_checks[] = {
    { RVA_NPC_CLS_SPAWNER,    "AAISpawner::GetPrivateStaticClass",  10, { 0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0x05 } },
    { RVA_NPC_CLS_ARCHETYPE,  "UArchetypeAsset::GetPrivateStaticClass", 10, { 0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0x05 } },
    { RVA_NPC_SPAWNACTOR,     "UWorld::SpawnActor",                  8, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x70 } },
    { RVA_NPC_PARAMS_CTOR,    "FActorSpawnParameters ctor",          9, { 0x33, 0xC0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08 } },
    { RVA_NPC_WANTS_SPAWN,    "AAISpawner::BPF_WantsSpawn",         16, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
    { RVA_NPC_CAN_SPAWN,      "AAISpawner::CanSpawn",               14, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x80, 0x04, 0x00, 0x00, 0x00 } },
    { RVA_NPC_UNSPAWN,        "AAISpawner::UnSpawnAi",              13, { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9 } },
    { RVA_NPC_SET_ARCHETYPE,  "AAISpawner::SetArchetypeOverride",    8, { 0x48, 0x81, 0xC1, 0xA8, 0x05, 0x00, 0x00, 0xE9 } },
    { RVA_NPC_SET_REPLICATES, "AActor::SetReplicates",              17, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x20, 0x01, 0x00, 0x00, 0x03 } },
    { RVA_NPC_LOAD_OBJECT,    "StaticLoadObject",                   20, { 0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xF0, 0xFD, 0xFF, 0xFF } },
};
static int g_npc_verified;     // 0 = not checked yet, 1 = all bytes match, -1 = something differs (NPC spawning stays off)

static int npc_verify(void) {
    if (g_npc_verified) return g_npc_verified > 0;
    g_npc_verified = 1;
    for (size_t i = 0; i < sizeof g_npc_checks / sizeof g_npc_checks[0]; i++) {
        const NpcCheck *c = &g_npc_checks[i];
        if (memcmp(g_base + c->rva, c->bytes, (size_t)c->n) != 0) {
            logf_("npc: %s at base+0x%X has different bytes than expected - NPC spawning is off for this run", c->what, c->rva);
            g_npc_verified = -1;
        }
    }
    if (g_npc_verified > 0) logf_("npc: all %d engine functions verified", (int)(sizeof g_npc_checks / sizeof g_npc_checks[0]));
    return g_npc_verified > 0;
}

static void *npc_cls_spawner(void) { return ((void *(*)(void))(g_base + RVA_NPC_CLS_SPAWNER))(); }
static void *npc_cls_archetype(void) { return ((void *(*)(void))(g_base + RVA_NPC_CLS_ARCHETYPE))(); }
static void *npc_spawn_actor(void *w, void *cls, const float loc[3]) {
    uint8_t params[0x30];
    ((void *(*)(void *))(g_base + RVA_NPC_PARAMS_CTOR))(params);
    params[0x28] = 1;                                    // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    *(uint16_t *)(params + 0x2A) |= 2;                   // bNoFail (the game's own AAISpawner::SpawnAI sets the same)
    const float rot[3] = { 0.f, 0.f, 0.f };
    return ((void *(*)(void *, void *, const float *, const float *, void *))(g_base + RVA_NPC_SPAWNACTOR))(w, cls, loc, rot, params);
}
static uint8_t npc_can_spawn(void *sp) { return ((uint8_t (*)(void *))(g_base + RVA_NPC_CAN_SPAWN))(sp); }
static void npc_wants_spawn(void *sp, void *arch) { ((void (*)(void *, void *, int, int))(g_base + RVA_NPC_WANTS_SPAWN))(sp, arch, 0, -1); }
static void npc_unspawn(void *sp) { ((void (*)(void *))(g_base + RVA_NPC_UNSPAWN))(sp); }
static void npc_set_replicates(void *a, int on) { ((void (*)(void *, uint8_t))(g_base + RVA_NPC_SET_REPLICATES))(a, (uint8_t)(on != 0)); }
static void npc_set_archetype(void *sp, void *arch) { ((void (*)(void *, void *))(g_base + RVA_NPC_SET_ARCHETYPE))(sp, arch); }
static void *npc_load_object(void *cls, const char *path) {
    wchar_t wide[300];
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 300);
    if (n <= 0) return NULL;
    return ((void *(*)(void *, void *, const wchar_t *, const wchar_t *, uint32_t, void *, int))(g_base + RVA_NPC_LOAD_OBJECT))(cls, NULL, wide, NULL, 0, NULL, 1);
}
static const NpcApi g_npcapi = { npc_cls_spawner, npc_cls_archetype, npc_spawn_actor, npc_can_spawn, npc_wants_spawn, npc_unspawn, npc_set_replicates, npc_set_archetype, npc_load_object };

static int do_travel(const wchar_t *url) {
    void *e = engine(), *w = world();
    if (!e || !w) { logf_("travel refused: GEngine=%p GWorld=%p (get past the title screen first)", e, w); return 0; }
    logf_("travel -> %ls", url);
    ((SetClientTravel_t)(g_base + g_targets[IDX_SETCLIENTTRAVEL].rva))(e, w, url, TRAVEL_ABSOLUTE);
    return 1;
}

// UObject class is +0x10 and each UStruct superclass link is +0x30.  Caller supplies live pointers and SEH guards access.
static int object_is_a(void *object, void *wanted_class) {
    void *cls = *(void **)((uint8_t *)object + 0x10);
    for (int i = 0; cls && i < 64; i++) {
        if (cls == wanted_class) return 1;
        cls = *(void **)((uint8_t *)cls + 0x30);
    }
    return 0;
}

typedef void *(*StaticConstructObject_t)(void *cls, void *outer, uint64_t name, uint32_t flags,
                                          uint32_t internal_flags, void *templ, int copy_transients,
                                          void *instance_graph, int assume_template_is_archetype);

static void ensure_console(void) {
    if (g_console_state) return;
    __try {
        void *e = engine();
        void *viewport = e ? *(void **)((uint8_t *)e + OFF_ENGINE_VIEWPORT) : NULL;
        void *console_class = e ? *(void **)((uint8_t *)e + OFF_ENGINE_CONSOLECLASS) : NULL;
        void *current = viewport ? *(void **)((uint8_t *)viewport + OFF_VIEWPORT_CONSOLE) : NULL;
        if (!console_should_construct(e, viewport, console_class, current)) {
            if (current) { g_console_state = 1; logf_("console: already present on viewport %p", viewport); }
            return;
        }
        if (!object_is_a(viewport, api_cls_viewport())) {
            g_console_state = 2;
            logf_("console: viewport %p is not a UGameViewportClient - disabled", viewport);
            return;
        }
        void *console = ((StaticConstructObject_t)(g_base + RVA_STATIC_CONSTRUCT))
            (console_class, viewport, 0, 0, 0, NULL, 0, NULL, 0);
        if (!console || !object_is_a(console, console_class)) {
            g_console_state = 2;
            logf_("console: construction returned invalid object %p - disabled", console);
            return;
        }
        *(void **)((uint8_t *)viewport + OFF_VIEWPORT_CONSOLE) = console;
        g_console_state = 1;
        logf_("console: constructed %p and assigned to viewport %p", console, viewport);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_console_state = 2;
        logf_("console: EXCEPTION 0x%08lX - disabled; LAN fixes stay active", GetExceptionCode());
    }
}

// first line of ip.txt = host ip or ip:port, optional second line = extra URL options (e.g. ?CheckPoint=...?zone=3)
static int read_saved_join_options(wchar_t *out, int cap, unsigned *zone_out) {
    __try {
        void *w = world(), *gi = w ? *(void **)((uint8_t *)w + OFF_W_GAMEINST) : NULL;
        if (!gi || !object_is_a(gi, api_cls_gi())) return 0;
        void *save = *(void **)((uint8_t *)gi + OFF_GI_CURRENTSAVE);
        uint8_t *data = save ? (uint8_t *)save + OFF_SAVE_SAVEDDATA : NULL;
        wchar_t *spawn = data ? *(wchar_t **)(data + OFF_SAVED_SPAWNPOINT) : NULL;
        int num = data ? *(int *)(data + OFF_SAVED_SPAWNPOINT + 8) : 0;
        int max = data ? *(int *)(data + OFF_SAVED_SPAWNPOINT + 12) : 0;
        if (!spawn || num <= 1 || num > max || num > 400 || num >= cap) return 0;
        memcpy(out, spawn, (size_t)(num - 1) * sizeof *out); out[num - 1] = 0;
        *zone_out = data[OFF_SAVED_ZONE];
        logf_("join: using current save CheckPoint and zone %u", *zone_out);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("join: could not read current save options (0x%08lX); using a bare URL", GetExceptionCode());
        return 0;
    }
}

static int read_join_url(wchar_t *out, int cap) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%sip.txt", g_dir);
    FILE *f = _wfopen(path, L"r");
    if (!f) { logf_("join: %ls not found", path); return 0; }
    char l1[256] = "", l2[512] = "";
    if (fgets(l1, sizeof l1, f)) { if (!fgets(l2, sizeof l2, f)) l2[0] = 0; }
    fclose(f);
    char *e;
    for (e = l1 + strlen(l1); e > l1 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
    for (e = l2 + strlen(l2); e > l2 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
    if (!l1[0]) { logf_("join: ip.txt is empty"); return 0; }
    wchar_t checkpoint[512] = L"";
    unsigned zone = 0;
    if (join_options_is_none(l2)) {
        l2[0] = 0;                                                       // an explicit "no options" join, used for PvP hosts
        logf_("join: ip.txt line 2 is 'none' - joining with a bare URL (no CheckPoint/zone)");
    } else {
        if (!l2[0] && !read_saved_join_options(checkpoint, (int)(sizeof checkpoint / sizeof checkpoint[0]), &zone))
            logf_("join: no manual or current-save options; using a bare URL");
        if (l2[0]) logf_("join: using manual options from ip.txt");
    }
    if (!join_url_format(l1, l2, checkpoint, zone, out, (size_t)cap)) { logf_("join: URL is invalid or too long"); return 0; }
    return 1;
}

// M4.1.3 single settings file. LanNative\config.txt ("key = value" lines, see config.h) replaces the old one-file-per-setting layout. When config.txt exists it
// is the only source (a missing key = that setting's default); when it does not, the old files (parryfix.txt, pvpfix.txt, ...) are still read.
// src receives the path that was consulted (for the log). 1 = value found.
static char g_cfg_text[16384];
static int g_cfg_present;

static int read_text_file(const wchar_t *path, char *buf, size_t cap) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return 1;
}

static void config_load(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%sconfig.txt", g_dir);
    g_cfg_present = read_text_file(path, g_cfg_text, sizeof g_cfg_text);
    if (g_cfg_present) logf_("config: reading all settings from %ls", path);
    else logf_("config: %ls is absent; using the old per-setting files (parryfix.txt, pvpfix.txt, ...) or defaults", path);
}

static int setting_get(const char *key, const wchar_t *legacy, int nth, char *out, size_t cap, wchar_t *src) {
    if (g_cfg_present) {
        swprintf(src, MAX_PATH, L"%sconfig.txt", g_dir);
        return cfg_get(g_cfg_text, key, nth, out, cap);
    }
    swprintf(src, MAX_PATH, L"%s%s", g_dir, legacy);
    char text[4096];
    return read_text_file(src, text, sizeof text) && cfg_nth_line(text, nth, out, cap);
}

static int parryfix_enabled(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("parryfix", L"parryfix.txt", 0, value, sizeof value, path)) { logf_("parry fix: enabled (default; not set in %ls)", path); return 1; }
    for (char *p = value; *p; p++) if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') { *p = 0; break; }
    if (_stricmp(value, "off") == 0) { logf_("parry fix: disabled by %ls", path); return 0; }
    if (_stricmp(value, "on") != 0) logf_("parry fix: %ls is not on/off; using default on", path);
    else logf_("parry fix: enabled by %ls", path);
    return 1;
}

// LanNative\pvpfix.txt: "on" (default) = send-back guard + duel debug flow, "guard" = send-back guard only, "off" = neither.
// Returns 0 = off, 1 = guard only, 2 = guard + flow.
static int pvpfix_mode(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("pvpfix", L"pvpfix.txt", 0, value, sizeof value, path)) { logf_("pvp: guard + duel flow enabled (default; not set in %ls)", path); return 2; }
    for (char *p = value; *p; p++) if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') { *p = 0; break; }
    if (_stricmp(value, "off") == 0) { logf_("pvp: disabled by %ls", path); return 0; }
    if (_stricmp(value, "guard") == 0) { logf_("pvp: send-back guard only, duel flow off (%ls)", path); return 1; }
    if (_stricmp(value, "on") != 0) logf_("pvp: %ls is not on/guard/off; using default on", path);
    else logf_("pvp: guard + duel flow enabled by %ls", path);
    return 2;
}
// LanNative\perteam.txt: players per team for 3v3 maps (default 1 = a 3v3 map starts with 2 testers; 2 = starts with 4; 3 or "off" = the game's own 6).
static int read_perteam(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("perteam", L"perteam.txt", 0, value, sizeof value, path)) { logf_("pvp teams: 3v3 maps start with 1 player per team, 2 players in total (default; not set in %ls)", path); return 1; }
    int n = atoi(value);
    if (_strnicmp(value, "off", 3) == 0 || n == 3) { logf_("pvp teams: 3v3 maps keep the game's 3 players per team (%ls)", path); return 0; }
    if (n < 1 || n > 2) { logf_("pvp teams: %ls is not 1, 2, 3 or off; using the default 1", path); return 1; }
    logf_("pvp teams: 3v3 maps start with %d player(s) per team (%ls)", n, path);
    return n;
}

// LanNative\autoleave.txt: "on" (default) or "off". In a duel map, when the other player's connection is gone, go back to your own game like F6 does.
static int read_autoleave(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("autoleave", L"autoleave.txt", 0, value, sizeof value, path)) { logf_("autoleave: on (default; not set in %ls)", path); return 1; }
    if (_strnicmp(value, "off", 3) == 0) { logf_("autoleave: off (%ls)", path); return 0; }
    logf_("autoleave: on (%ls)", path);
    return 1;
}
static void apply_parryfix(void) {
    uint8_t *code = g_base + RVA_PARRY_FIX;
    if (memcmp(code, "\x48\x81\xC1\xFC\x01\x00\x00", PARRYFIX_SIZE) != 0) {
        logf_("parry fix: bytes at base+0x%X differ from 48 81 C1 FC 01 00 00 - skipped", RVA_PARRY_FIX);
        return;
    }
    DWORD old;
    if (!VirtualProtect(code, PARRYFIX_SIZE, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("parry fix: VirtualProtect failed - skipped");
        return;
    }
    int applied = parryfix_apply_bytes(code, PARRYFIX_SIZE);
    VirtualProtect(code, PARRYFIX_SIZE, old, &old);
    if (!applied) { logf_("parry fix: bytes changed while applying - skipped"); return; }
    FlushInstructionCache(GetCurrentProcess(), code, PARRYFIX_SIZE);
    g_parry_fix_applied = 1;
    logf_("parry fix: applied at base+0x%X (FC -> F4)", RVA_PARRY_FIX);
}

// ---------------------------------------------------------------------------------------------------------------------
// the per-frame work (game thread)
static int key_edge(int vk, int *prev) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !*prev;
    *prev = down;
    return edge;
}

static int game_has_focus(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void read_telemetry_cfg(void) {
    wchar_t path[MAX_PATH];
    char text[64] = "";
    int have = setting_get("telemetry", L"telemetry.txt", 0, text, sizeof text, path);
    int known = have ? tel_parse_config(text, &g_tel_cfg) : (tel_parse_config(NULL, &g_tel_cfg), 0);
    static const char *names[] = { "off", "basic", "verbose" };
    if (!have) logf_("telemetry: %s, every %d s (default; not set in %ls). Set telemetry = off / basic / verbose [seconds] to change it.", names[g_tel_cfg.level], g_tel_cfg.interval_ms / 1000, path);
    else if (!known) logf_("telemetry: %ls is not off/basic/verbose [seconds]; using basic, every 5 s", path);
    else logf_("telemetry: %s%s enabled by %ls", names[g_tel_cfg.level], g_tel_cfg.level ? "" : " (nothing is recorded)", path);
    if (g_tel_cfg.level)
        logf_("telemetry: events go to lannative.log; per-snapshot data goes to telemetry_frames.csv, telemetry_conns.csv and telemetry_players.csv in this folder (read-only observation, nothing is written into the game)");
}

static void tel_fault(const char *what, DWORD code) {
    g_tel_faults++;
    if (g_tel_faults >= 3) {
        g_tel_off = 1;
        logf_("telemetry: EXCEPTION 0x%08lX in %s (fault %d) - telemetry is off for this run; the LAN fixes stay active", code, what, g_tel_faults);
    } else {
        logf_("telemetry: EXCEPTION 0x%08lX in %s (fault %d of 3) - skipped this time", code, what, g_tel_faults);
    }
}

// Notes every change in the number of connected peers (called every frame and again inside the PvP guard, so the guard never depends on the
// 2-per-second telemetry check). Caller provides the SEH. 1 = the count changed.
static int note_peers(uint64_t now) {
    int peers = T_peers_now();
    if (peers == g_peers_seen) return 0;
    if (g_peers_seen != -1 || peers > 0) g_last_peer_change_ms = now;   // the first look at "0 peers" is not an event
    g_peers_seen = peers;
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// M4.1.1 host address. The game's socket code reads "-MULTIHOME=<ip>" from the command line buffer when a listen server picks the address it binds
// (ISocketSubsystem::GetLocalHostAddr). Without it the host binds every adapter, which already accepts a joiner that connects to the host's Radmin/VPN
// address; with it the host binds exactly that one address. hostip.txt lists the choices (first valid line = default, "auto" = every adapter); the
// menu cycles them. The option is written into the buffer only when a "?listen" travel is requested and removed again for every other travel.
static char g_hostips[HOSTIP_MAX][HOSTIP_LEN];
static int g_hostip_n = 1, g_hostip_sel, g_cmd_saved, g_hostip_faults;
static wchar_t g_cmd_orig[CMDLINE_CAP + 1];
static char g_cmd_applied[HOSTIP_LEN] = "auto";

static void hostip_load(void) {
    wchar_t path[MAX_PATH];
    strcpy(g_hostips[0], "auto"); g_hostip_n = 1; g_hostip_sel = 0;
    char line[128]; int n = 0;
    for (int k = 0; n < HOSTIP_MAX && setting_get("hostip", L"hostip.txt", k, line, sizeof line, path); k++) {
        char clean[64];
        if (!hostip_clean(line, clean)) { logf_("host address: ignoring '%.40s' (not an IPv4 address or auto)", line); continue; }
        strcpy(g_hostips[n++], clean);
    }
    if (!n) { swprintf(path, MAX_PATH, L"%sconfig.txt", g_dir); logf_("host address: auto - every adapter (default; no hostip set). Add hostip = <Radmin/VPN address> to %ls to bind only it.", path); return; }
    g_hostip_n = n;
    logf_("host address: %d choice(s) from %ls; a host binds '%s'%s", g_hostip_n, path, g_hostips[0], strcmp(g_hostips[0], "auto") ? " (that address only - joiners must use it)" : " (every adapter)");
}

// The DLL is loaded before the engine parses the command line (FCommandLine::bIsInitialized is still 0 then, as the M4.2 logs showed), so the
// original command line is saved from the first game ticks instead, once the flag is set. Quiet until then; tries every tick for ~50 s at most.
static int g_cmd_tries;
static void cmdline_try_save(void) {
    if (g_cmd_saved || g_cmd_tries > 3000) return;
    g_cmd_tries++;
    __try {
        if (!*(uint8_t *)(g_base + RVA_CMDLINE_INIT)) {
            if (g_cmd_tries == 3000) logf_("host address: the command line never became initialized - host address choice is unavailable");
            return;
        }
        const wchar_t *buf = (const wchar_t *)(g_base + RVA_CMDLINE_BUF);
        size_t n = 0;
        while (n < CMDLINE_CAP && buf[n]) n++;
        memcpy(g_cmd_orig, buf, n * sizeof(wchar_t));
        g_cmd_orig[n] = 0;
        g_cmd_saved = 1;
        logf_("host address: original command line saved (%u characters, tick %d) - host address choice is available", (unsigned)n, g_cmd_tries);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cmd_tries = 3001; logf_("host address: EXCEPTION 0x%08lX saving the command line - host address choice is unavailable", GetExceptionCode()); }
}

// ip = "auto"/NULL puts the original command line back. Called from the SetClientTravel hook (game thread), under its SEH.
static void cmdline_apply(const char *ip) {
    if (!g_cmd_saved || g_hostip_faults >= 2) return;
    static wchar_t tmp[CMDLINE_CAP + 1];
    if (!hostip_compose(tmp, CMDLINE_CAP + 1, g_cmd_orig, ip)) { logf_("host address: the command line has no room for -MULTIHOME - not applied"); return; }
    wchar_t *buf = (wchar_t *)(g_base + RVA_CMDLINE_BUF);
    size_t n = wcslen(tmp);
    memcpy(buf, tmp, (n + 1) * sizeof(wchar_t));
    strncpy(g_cmd_applied, ip ? ip : "auto", HOSTIP_LEN - 1); g_cmd_applied[HOSTIP_LEN - 1] = 0;
}

static void hostip_on_travel(const wchar_t *url) {
    if (!g_cmd_saved) return;
    const char *want = hostip_url_is_listen(url) ? g_hostips[g_hostip_sel] : "auto";
    int listen = hostip_url_is_listen(url);
    if (strcmp(want, g_cmd_applied) != 0) {
        __try { cmdline_apply(want); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_hostip_faults++;
            logf_("host address: EXCEPTION 0x%08lX writing the command line (fault %d of 2)", GetExceptionCode(), g_hostip_faults);
        }
    }
    if (listen) logf_("host address: this listen server binds %s%s", strcmp(g_cmd_applied, "auto") ? g_cmd_applied : "every adapter (auto)", strcmp(g_cmd_applied, "auto") ? " only (-MULTIHOME)" : "");
}

// ---------------------------------------------------------------------------------------------------------------------
// M4.0 in-game menu (state machine and layout: menu.h; this gathers key edges, runs the chosen action and draws the text)
static int g_menu_on = 1, g_menu_faults, g_draw_faults, g_perteam_cfg = 1, g_autoleave_on = 1;
static Menu g_menu;
static char g_maps[MENU_LIST_MAX][MENU_URL_MAX], g_altar_label[MENU_LIST_MAX][MENU_LINE_MAX];
static float g_altar_pos[MENU_LIST_MAX][3];
static int g_maps_n, g_altars_n;
#define ALTAR_LIFT 150.f   // altar position + this height, so the character lands on the ground instead of inside it
static char g_menu_lines[MENU_LINES_MAX][MENU_LINE_MAX];
static int g_menu_nlines, g_menu_sel = -1;
static uint64_t g_armed6, g_armed7, g_armed_menu;

static void menu_load_list(const wchar_t *name, char (*out)[MENU_URL_MAX], int *count) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s%s", g_dir, name);
    *count = 0;
    FILE *f = _wfopen(path, L"r");
    if (!f) { logf_("menu: %ls not found - that list is empty", path); return; }
    char line[1024];
    while (*count < MENU_LIST_MAX && fgets(line, sizeof line, f)) if (menu_parse_open_line(line, out[*count], MENU_URL_MAX)) (*count)++;
    fclose(f);
    logf_("menu: %d travel entries from %ls", *count, path);
}

// altars.tsv (shipped; made from the game's own MainMap_Main.umap): label, class, zone, subzone, x, y, z, root
static void menu_load_altars(const wchar_t *name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s%s", g_dir, name);
    g_altars_n = 0;
    FILE *f = _wfopen(path, L"r");
    if (!f) { logf_("menu: %ls not found - the altar list is empty", path); return; }
    char line[512];
    while (g_altars_n < MENU_LIST_MAX && fgets(line, sizeof line, f))
        if (menu_parse_altar_line(line, g_altar_label[g_altars_n], MENU_LINE_MAX, g_altar_pos[g_altars_n])) g_altars_n++;
    fclose(f);
    logf_("menu: %d altar positions from %ls", g_altars_n, path);
}

// M4.2/M4.3 NPCs: npcs.txt, one entry per line, TAB separated: group, label, archetype asset path[, character class path] (menu.h has the parser). Entries are
// kept grouped (each group is a contiguous range) so the F1 menu can show a group list first. Without the file there is one group with one entry.
#define NPC_MAX       512
#define NPC_LABEL_MAX 48
#define NPC_PATH_MAX  200
#define NPC_CLASS_MAX 140
static char g_npc_label[NPC_MAX][NPC_LABEL_MAX], g_npc_path[NPC_MAX][NPC_PATH_MAX], g_npc_class[NPC_MAX][NPC_CLASS_MAX];
static char g_npc_group_name[MENU_GROUPS_MAX][NPC_LABEL_MAX];
static int g_npc_group_count[MENU_GROUPS_MAX], g_npc_ngroups, g_npcs_n, g_npc_on = 1, g_npc_faults;

static int npc_group_index(const char *name) {
    for (int g = 0; g < g_npc_ngroups; g++) if (strcmp(g_npc_group_name[g], name) == 0) return g;
    if (g_npc_ngroups >= MENU_GROUPS_MAX) return -1;
    snprintf(g_npc_group_name[g_npc_ngroups], NPC_LABEL_MAX, "%s", name);
    return g_npc_ngroups++;
}

static void npc_load_list(const wchar_t *name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s%s", g_dir, name);
    g_npcs_n = 0; g_npc_ngroups = 0; memset(g_npc_group_count, 0, sizeof g_npc_group_count);
    FILE *f = _wfopen(path, L"r");
    if (!f) {
        npc_group_index("Open world");
        snprintf(g_npc_label[0], NPC_LABEL_MAX, "Trickster"); snprintf(g_npc_path[0], NPC_PATH_MAX, "/Game/DB/AI/NPCs/Trickster/Trickster.Trickster"); g_npc_class[0][0] = 0;
        g_npc_group_count[0] = 1; g_npcs_n = 1;
        logf_("menu: %ls not found - the NPC list has only the built-in Trickster", path);
        return;
    }
    char line[1024], grp[NPC_LABEL_MAX], lab[NPC_LABEL_MAX], pth[NPC_PATH_MAX], cls[NPC_CLASS_MAX];
    int dropped = 0, start[MENU_GROUPS_MAX] = { 0 }, fill[MENU_GROUPS_MAX] = { 0 };
    while (fgets(line, sizeof line, f))                                           // pass 1: groups and how many entries each has
        if (menu_parse_npc_line(line, grp, sizeof grp, lab, sizeof lab, pth, sizeof pth, cls, sizeof cls)) {
            int g = npc_group_index(grp);
            if (g < 0 || g_npc_group_count[g] >= MENU_LIST_MAX || g_npcs_n >= NPC_MAX) { dropped++; continue; }
            g_npc_group_count[g]++; g_npcs_n++;
        }
    for (int g = 1; g < g_npc_ngroups; g++) start[g] = start[g - 1] + g_npc_group_count[g - 1];
    rewind(f);
    while (fgets(line, sizeof line, f))                                           // pass 2: put every entry into its group's range
        if (menu_parse_npc_line(line, grp, sizeof grp, lab, sizeof lab, pth, sizeof pth, cls, sizeof cls)) {
            int g = npc_group_index(grp);
            if (g < 0 || fill[g] >= g_npc_group_count[g]) continue;               // exactly the entries pass 1 kept
            int at = start[g] + fill[g]++;
            snprintf(g_npc_label[at], NPC_LABEL_MAX, "%s", lab); snprintf(g_npc_path[at], NPC_PATH_MAX, "%s", pth); snprintf(g_npc_class[at], NPC_CLASS_MAX, "%s", cls);
        }
    fclose(f);
    logf_("menu: %d NPC types in %d groups from %ls%s", g_npcs_n, g_npc_ngroups, path, dropped ? " (some entries were dropped: too many groups, more than 96 in a group, or more than 512 in total)" : "");
}

// npc = on / off (default on); npcrepl = on / off (default off): also replicate the spawner actor to the joiner
static void read_npc_cfg(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (setting_get("npc", L"npc.txt", 0, value, sizeof value, path) && _strnicmp(value, "off", 3) == 0) { g_npc_on = 0; logf_("npc: off (%ls)", path); }
    else logf_("npc: on (the F1 menu can spawn NPCs when you are the host)");
    value[0] = 0;
    int repl = setting_get("npcrepl", L"npcrepl.txt", 0, value, sizeof value, path) && _strnicmp(value, "on", 2) == 0;
    L_npc_set_replicated(repl);
    logf_("npc: spawner actor %s to the joiner (npcrepl = %s)", repl ? "is replicated" : "stays local", repl ? "on" : "off");
}

static void npc_spawn_action(int index) {
    if (!g_npc_on) { logf_("npc: switched off (npc = off in config.txt)"); return; }
    if (g_npc_faults >= 2) { logf_("npc: off for this run after 2 faults"); return; }
    if (index < 0 || index >= g_npcs_n) return;
    if (!npc_verify()) return;
    __try { L_npc_spawn(g_npc_path[index], g_npc_label[index], g_npc_class[index]); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_npc_faults++;
        logf_("npc: EXCEPTION 0x%08lX while spawning (fault %d of 2)%s", GetExceptionCode(), g_npc_faults, g_npc_faults >= 2 ? " - NPC spawning is off for this run" : "");
    }
}

static void npc_remove_action(void) {
    if (g_npc_faults >= 2) { logf_("npc: off for this run after 2 faults"); return; }
    if (g_npc_verified <= 0) { L_npc_remove_all(); return; }                  // nothing could have been spawned without the engine calls
    __try { L_npc_remove_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_npc_faults++;
        logf_("npc: EXCEPTION 0x%08lX while removing (fault %d of 2)", GetExceptionCode(), g_npc_faults);
    }
}

static int read_menu_cfg(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("menu", L"menu.txt", 0, value, sizeof value, path)) { logf_("menu: on (default; not set in %ls). F1 opens it.", path); return 1; }
    if (_strnicmp(value, "off", 3) == 0) { logf_("menu: off (%ls)", path); return 0; }
    logf_("menu: on (%ls). F1 opens it.", path);
    return 1;
}

static const char *menu_list_text(int page, int i) {
    static char label[MENU_LINE_MAX];
    if (page == MP_ALTARS) return g_altar_label[i];
    if (page == MP_NPCGROUPS) { snprintf(label, sizeof label, "%s (%d)", g_npc_group_name[i], g_npc_group_count[i]); return label; }
    if (page == MP_NPCS) return g_npc_label[(g_menu.ngroups > 0 ? g_menu.gstart[g_menu.group] : 0) + i];
    snprintf(label, sizeof label, "%s", g_maps[i]);
    return label;
}

static void menu_do(MenuAct a, uint64_t now) {
    switch (a.action) {
    case MA_HOST: if (L_guard_leave(&g_armed6, now, "menu Host")) do_travel(MAP_URL); else g_menu.open = 1; break;
    case MA_JOIN: if (L_guard_leave(&g_armed7, now, "menu Join")) { wchar_t url[900]; if (read_join_url(url, 900)) do_travel(url); } else g_menu.open = 1; break;
    case MA_HOSTIP:
        g_hostip_sel = (g_hostip_sel + 1) % g_hostip_n;
        logf_("menu: host address = %s (used the next time you host with F6 / the menu%s)", g_hostips[g_hostip_sel], strcmp(g_hostips[g_hostip_sel], "auto") ? "; joiners must connect to exactly this address" : "");
        break;
    case MA_SUMMON: L_summon(); break;
    case MA_STATE: L_state(); break;
    case MA_MARK: if (!g_tel_off) T_mark(); else logf_("menu: log marker unavailable (telemetry is off)"); break;
    case MA_AUTOLEAVE: g_autoleave_on = !g_autoleave_on; logf_("menu: auto-leave is now %s", g_autoleave_on ? "on" : "off"); break;
    case MA_PERTEAM:
        g_perteam_cfg = g_perteam_cfg == 1 ? 2 : (g_perteam_cfg == 2 ? 0 : 1);
        L_set_perteam(g_perteam_cfg);
        logf_("menu: 3v3 players per team = %d (%s; takes effect the next time a 3v3 map is hosted)", g_perteam_cfg, g_perteam_cfg ? "lowered" : "the game's own 3");
        break;
    case MA_SPAWN_NPC: npc_spawn_action(a.index); break;
    case MA_REMOVE_NPCS: npc_remove_action(); break;
    case MA_TELEPORT: {
        if (a.index < 0 || a.index >= g_altars_n) break;
        float to[3] = { g_altar_pos[a.index][0], g_altar_pos[a.index][1], g_altar_pos[a.index][2] + ALTAR_LIFT };
        logf_("menu: teleport to altar %s", g_altar_label[a.index]);
        L_teleport_local(to);
        break; }
    case MA_TRAVEL: {
        if (a.index < 0 || a.index >= g_maps_n) break;
        const char *url = g_maps[a.index];
        if (!L_guard_leave(&g_armed_menu, now, "menu travel")) { g_menu.open = 1; break; }
        wchar_t wide[MENU_URL_MAX];
        int n = MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, MENU_URL_MAX);
        if (n > 0) do_travel(wide);
        break; }
    default: break;
    }
}

static void menu_tick(int focus, uint64_t now) {
    static int kf1, kup, kdn, ken, krt, klf, kbs, loaded;
    if (!g_menu_on || g_menu_faults >= 2) { g_menu.open = 0; g_menu_nlines = 0; return; }
    if (!loaded) {
        loaded = 1;
        menu_load_list(L"maps.txt", g_maps, &g_maps_n);
        menu_load_altars(L"altars.tsv");
        npc_load_list(L"npcs.txt");
        menu_init_n(&g_menu, g_maps_n, g_altars_n, g_npcs_n);
        menu_set_npc_groups(&g_menu, g_npc_ngroups, g_npc_group_count);
    }
    if (!focus) { kf1 = kup = kdn = ken = krt = klf = kbs = 0; return; }
    int f1 = key_edge(VK_F1, &kf1), up = key_edge(VK_UP, &kup), dn = key_edge(VK_DOWN, &kdn), en = key_edge(VK_RETURN, &ken);
    int rt = key_edge(VK_RIGHT, &krt), lf = key_edge(VK_LEFT, &klf), bs = key_edge(VK_BACK, &kbs);
    int key = f1 ? MK_TOGGLE : !g_menu.open ? 0 : up ? MK_UP : dn ? MK_DOWN : (en || rt) ? MK_SELECT : (lf || bs) ? MK_BACK : 0;
    __try {
        if (key) {
            int was = g_menu.open;
            MenuAct a = menu_key(&g_menu, key);
            if (g_menu.open != was) logf_("menu: %s", g_menu.open ? "opened" : "closed");
            if (a.action != MA_NONE) { logf_("menu: action %d%s", a.action, a.action == MA_TRAVEL ? " (travel)" : a.action == MA_TELEPORT ? " (teleport)" : a.action == MA_SPAWN_NPC ? " (spawn npc)" : a.action == MA_REMOVE_NPCS ? " (remove npcs)" : ""); menu_do(a, now); }
        }
        if (g_menu.open) {
            MenuStatus st = { g_autoleave_on, g_perteam_cfg, !L_is_joiner(), g_maps_n, g_altars_n, g_hostips[g_hostip_sel], g_npc_on ? g_npcs_n : 0, L_npc_live() };
            g_menu_nlines = menu_layout(&g_menu, &st, menu_list_text, g_menu_lines, &g_menu_sel);
        } else g_menu_nlines = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_menu_faults++; g_menu.open = 0; g_menu_nlines = 0;
        logf_("menu: EXCEPTION 0x%08lX (fault %d of 2)%s", GetExceptionCode(), g_menu_faults, g_menu_faults >= 2 ? " - the menu is off for this run; everything else keeps working" : "");
    }
}

typedef float (*CanvasDrawText_t)(void *canvas, void *font, const void *fstring, float x, float y, float xs, float ys, const void *render_info);
typedef struct { wchar_t *data; int32_t num, max; } FStringView;
typedef void (*PostRender_t)(void *viewport, void *canvas);
static PostRender_t g_orig_postrender;

static void draw_menu(void *canvas) {
    static const uint8_t expect[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x70 };   // UCanvas::DrawText prologue (verified once)
    static int checked, usable;
    if (!checked) {
        checked = 1;
        usable = memcmp(g_base + RVA_CANVAS_DRAWTEXT, expect, sizeof expect) == 0;
        logf_("menu: UCanvas::DrawText at base+0x%X %s", RVA_CANVAS_DRAWTEXT, usable ? "verified - the menu can draw" : "has different bytes - the menu will not draw");
    }
    if (!usable) return;
    void *e = engine();
    void *font = e ? *(void **)((uint8_t *)e + OFF_ENGINE_MEDIUMFONT) : NULL;
    if (!font) return;
    CanvasDrawText_t draw = (CanvasDrawText_t)(g_base + RVA_CANVAS_DRAWTEXT);
    uint8_t *color = (uint8_t *)canvas + OFF_CANVAS_DRAWCOLOR, saved[4];
    memcpy(saved, color, 4);
    float y = 120.f;
    for (int i = 0; i < g_menu_nlines; i++, y += 24.f) {
        wchar_t wide[MENU_LINE_MAX];
        int n = 0;
        while (g_menu_lines[i][n] && n < MENU_LINE_MAX - 1) { wide[n] = (wchar_t)(unsigned char)g_menu_lines[i][n]; n++; }
        wide[n] = 0;
        FStringView text = { wide, n + 1, n + 1 };
        uint8_t info[64]; memset(info, 0, sizeof info);                // default FFontRenderInfo: no clipping, no shadow, no glow
        static const uint8_t black[4] = { 0, 0, 0, 255 }, white[4] = { 255, 255, 255, 255 }, yellow[4] = { 40, 220, 255, 255 }, cyan[4] = { 255, 220, 120, 255 };
        memcpy(color, black, 4); draw(canvas, font, &text, 62.f, y + 2.f, 1.f, 1.f, info);
        memcpy(color, i == 0 ? cyan : (i == g_menu_sel ? yellow : white), 4); draw(canvas, font, &text, 60.f, y, 1.f, 1.f, info);
    }
    memcpy(color, saved, 4);
}

static void hk_postrender(void *viewport, void *canvas) {
    g_orig_postrender(viewport, canvas);
    if (g_disabled || !g_menu.open || !g_menu_nlines || !canvas || g_draw_faults >= 2) return;
    __try { draw_menu(canvas); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_draw_faults++;
        logf_("menu: EXCEPTION 0x%08lX while drawing (fault %d of 2)%s", GetExceptionCode(), g_draw_faults, g_draw_faults >= 2 ? " - the menu text is off for this run" : "");
    }
}

// M3.9 auto-leave (decision in pvpgate.h's autoleave_step; this only gathers the inputs and performs the travel)
static int g_al_faults, g_al_duel = -1;
static void *g_al_world;
static AutoLeave g_al;
static void autoleave_tick(uint64_t now) {
    void *w = world();
    if (w != g_al_world) { g_al_world = w; g_al_duel = -1; autoleave_reset(&g_al); }     // new world: start over, look at its path again
    if (!w) return;
    if (g_al_duel < 0) {
        char path[200];
        if (api_object_path(w, path, (int)sizeof path)) g_al_duel = pvp_path_is_duel(path);
        else return;                                                                       // not readable yet: try next frame
    }
    int peers = T_peers_now();
    if (peers > 0 && T_quiet_ms(now) >= AUTOLEAVE_QUIET_MS) peers = 0;       // connected on paper, but nothing has arrived for 10 s: the other side is gone
    int act = autoleave_step(&g_al, now, peers, g_al_duel, g_last_travel_ms);
    if (act == AL_ANNOUNCE) logf_("autoleave: the other player's connection is gone in this duel map - returning to your own game in %llu s unless something else is already travelling", AUTOLEAVE_GRACE_MS / 1000);
    else if (act == AL_LEAVE) { logf_("autoleave: leaving the duel map now (same travel as F6)"); do_travel(MAP_URL); }
}

static void on_tick(float dt) {
    static int first = 1, k6, k7, k5, k3, k8;
    static unsigned frame;
    if (first) {
        first = 0;
        logf_("first tick: GEngine=%p GWorld=%p base=%p", engine(), world(), g_base);
    }
    ensure_console();
    cmdline_try_save();
    if (g_pvpfix_on && g_orig_returnprev) {
        __try { note_peers(GetTickCount64()); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_peers_seen = -1; }     // a transient fault only forgets the last count
    }
    if (g_pvpflow_on && g_flow_faults < 2) {
        __try { L_pvp_flow(1); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_flow_faults++;
            logf_("pvp flow: EXCEPTION 0x%08lX (fault %d of 2)%s", GetExceptionCode(), g_flow_faults, g_flow_faults >= 2 ? " - duel flow is off for this run" : "");
        }
    }
    if (g_autoleave_on && g_pvpfix_on && g_al_faults < 2) {
        __try { autoleave_tick(GetTickCount64()); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_al_faults++;
            logf_("autoleave: EXCEPTION 0x%08lX (fault %d of 2)%s", GetExceptionCode(), g_al_faults, g_al_faults >= 2 ? " - autoleave is off for this run" : "");
        }
    }
    if (g_pvpflow_on && g_result_faults < 2) {
        __try { L_pvp_result(1); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_result_faults++;
            logf_("pvp result: EXCEPTION 0x%08lX (fault %d of 2)%s", GetExceptionCode(), g_result_faults, g_result_faults >= 2 ? " - duel result is off for this run" : "");
        }
    }
    if (!g_tel_off && g_tel_cfg.level != TEL_OFF) {
        __try { T_frame(dt); T_tick(GetTickCount64()); }
        __except (EXCEPTION_EXECUTE_HANDLER) { tel_fault("telemetry tick", GetExceptionCode()); }
    }
    if ((++frame % 30) == 0) L_periodic();
    if (g_clone_sweep && GetTickCount64() >= g_clone_sweep_due) {
        int n = L_sweep_joiner_clones();
        logf_("clone sweep %d/3: removed %d", g_clone_sweep, n);
        g_clone_sweep_due = GetTickCount64() + (g_clone_sweep == 1 ? 3000 : 5000);
        if (++g_clone_sweep > 3) g_clone_sweep = 0;
    }
    int focus = game_has_focus();
    menu_tick(focus, GetTickCount64());
    if (!focus) { k3 = k5 = k6 = k7 = k8 = 0; return; }
    uint64_t now = GetTickCount64();
    if (key_edge(VK_F8, &k8) && !g_tel_off) {
        __try { T_mark(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { tel_fault("mark", GetExceptionCode()); }
    }
    if (key_edge(VK_F6, &k6) && L_guard_leave(&g_armed6, now, "F6")) do_travel(MAP_URL);
    if (key_edge(VK_F7, &k7) && L_guard_leave(&g_armed7, now, "F7")) { wchar_t url[900]; if (read_join_url(url, 900)) do_travel(url); }
    if (key_edge(VK_F3, &k3)) L_summon();
    if (key_edge(VK_F5, &k5)) L_state();
}

#define GUARDED(label, body) __try { body } __except (EXCEPTION_EXECUTE_HANDLER) { \
    g_disabled = 1; logf_("EXCEPTION 0x%08lX in " label " - LanNative is now switched off for this run", GetExceptionCode()); }

static void hk_clientrestart(void *pc, void *new_pawn) {
    WeakPtr old = { -1, 0 };
    if (!g_disabled) { GUARDED("ClientRestart(pre)", L_clientrestart_pre(pc, &old);) }
    g_orig_clientrestart(pc, new_pawn);
    if (!g_disabled) { GUARDED("ClientRestart(post)", if (L_clientrestart_post(pc, new_pawn, &old)) { g_clone_sweep = 1; g_clone_sweep_due = GetTickCount64() + 2000; logf_("clone sweeps scheduled for 2, 5 and 10 seconds"); }) }
}

static void hk_serverack(void *pc, void *pawn) {
    g_orig_serverack(pc, pawn);
    if (!g_disabled) { GUARDED("ServerAcknowledgePossession", L_serverack_post(pc, pawn);) }
}

static void hk_netcleanup(void *pc, void *conn) {
    CleanupPair leaving = { { -1, 0 }, { -1, 0 } };
    if (!g_disabled) { GUARDED("OnNetCleanup(pre)", L_netcleanup_pre(pc, &leaving);) }
    g_orig_netcleanup(pc, conn);
    if (!g_disabled) { GUARDED("OnNetCleanup(post)", L_netcleanup_post(&leaving);) }
}

// "<label>: call chain (innermost first): exe+0x... < exe+0x..." - only frames inside the game image, so the RVAs can be named offline.
static void log_call_chain(const char *label) {
    void *frames[16];
    USHORT nframes = RtlCaptureStackBackTrace(0, 16, frames, NULL);
    char chain[600] = ""; size_t len = 0; int shown = 0;
    for (USHORT i = 0; i < nframes && shown < 12; i++) {
        char one[48];
        tel_describe_caller((uint64_t)(uintptr_t)frames[i], (uint64_t)(uintptr_t)g_base, g_exe_size, g_self_base, g_self_size, one, (int)sizeof one);
        if (strncmp(one, "exe+", 4) != 0) continue;
        len += (size_t)snprintf(chain + len, sizeof chain - len, "%s%s", shown ? " < " : "", one);
        shown++;
    }
    logf_("%s: call chain (innermost first): %s", label, shown ? chain : "(no frames in the game image)");
}

// Logs every client-travel request (F6/F7, the console's open/travel, and the game's own travels such as returning to a previous map)
// with the URL and where it was called from, then lets it proceed unchanged. It also timestamps the request for the LAN-PvP guard.
static void hk_settravel(void *eng, void *wld, const wchar_t *url, int type) {
    void *ret = _ReturnAddress();
    g_last_travel_ms = GetTickCount64();
    if (!g_disabled) {
        __try {
            wchar_t clipped[301]; int n = 0;
            while (url && url[n] && n < 300) { clipped[n] = url[n]; n++; }
            clipped[n] = 0;
            int was_clipped = (url && n == 300 && url[300] != 0);     // only look past the end when 300 characters were really there
            char narrow[1024] = "(null)";
            if (url && WideCharToMultiByte(CP_UTF8, 0, clipped, -1, narrow, (int)sizeof narrow, NULL, NULL) <= 0) strcpy(narrow, "(unprintable)");
            char who[48];
            tel_describe_caller((uint64_t)(uintptr_t)ret, (uint64_t)(uintptr_t)g_base, g_exe_size, g_self_base, g_self_size, who, (int)sizeof who);
            logf_("travel request: url=%s%s type=%d from=%s", narrow, was_clipped ? "..." : "", type, who);
            if (strcmp(who, "LanNative") != 0) log_call_chain("travel request");   // the game asked: record who, so it can be named offline
            hostip_on_travel(url);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logf_("travel request: could not read the URL (0x%08lX)", GetExceptionCode());
        }
    }
    g_orig_settravel(eng, wld, url, type);
}

// LAN-PvP guard (M3.5/M3.6). The PvP player-controller Blueprint calls this on BeginPlay when the game is offline and sends everyone back to the
// main map; it runs on the server for EVERY controller, so it also fires when a joiner connects. While a duel map is loaded and a client travel
// was requested or the number of connected peers changed within PVP_WINDOW_MS, skip that one call (return false = "did nothing", the function's
// own early-out value) and log it. Every other call, including the quit menu later on, passes through unchanged.
static uint8_t hk_returnprev(void *gi, void *arg) {
    if (!g_disabled && g_pvpfix_on) {
        int suppress = 0;
        __try {
            char path[200] = "?";
            uint64_t now = GetTickCount64();
            note_peers(now);                                         // fresh look: the peer may have connected since the last frame
            uint64_t travel = g_last_travel_ms, peers = g_last_peer_change_ms, last = pvp_latest(travel, peers);
            int have_path = api_object_path(world(), path, (int)sizeof path);
            int duel = have_path && pvp_path_is_duel(path);
            suppress = pvp_should_suppress(1, duel, now, last, PVP_WINDOW_MS);
            logf_("pvp guard: BPF_ReturnToPreviousMap called (world=%s duel_map=%d, last travel request %lld ms ago, last peer-count change %lld ms ago, peers now %d) -> %s",
                  have_path ? path : "(unreadable)", duel, travel ? (long long)(now - travel) : -1LL, peers ? (long long)(now - peers) : -1LL, g_peers_seen,
                  suppress ? "SKIPPED, you stay in this world" : "allowed");
            log_call_chain("pvp guard");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            suppress = 0;                                           // when in doubt, do what the game would have done
            logf_("pvp guard: EXCEPTION 0x%08lX while deciding - letting the game's call through", GetExceptionCode());
        }
        if (suppress) return 0;
    }
    return g_orig_returnprev(gi, arg);
}

// Parry observer (read-only). NotifyParrySuccessful(this = the parried ATTACKER's defense component, delay, actor = the PARRIER, ...) reads the
// ParryPropertyDB weak pointer from the PARRIER's defense component (parrier+0x1BE8, then +0x1FC or, with the fix, +0x1F4). M3.5 mistakenly
// looked at `this`, so its lines all said NULL; M3.6 inspects the parrier's component (L_parry_inspect, tested against fake memory).
// M4.3 duel score. AThePlainesGameMode::Killed calls killerPS->ScoreKill(victimPS, 1) for every kill by a player; when the victim is an NPC its AI controller
// has no PlayerState, so the victim is NULL and AAdversarialPlayerState::ScoreKill still records a kill in m_Scores - which is what the 1v1 / 3v3 match
// score, the round result and the recap count. This drops every kill whose victim is not a real player's PlayerState.
typedef void (*ScoreKill_t)(void *ps, void *victim, int points);
static ScoreKill_t g_orig_scorekill;
static int g_scorekill_faults;
static unsigned g_scorekill_skips;
static void hk_scorekill(void *ps, void *victim, int points) {
    int skip = 0;
    if (!g_disabled && g_scorekill_faults < 2) {
        __try {
            skip = !L_ps_is_player(victim);
            if (skip) logf_("pvp score: ignored kill #%u by player state %p (victim %p is not a player - an NPC died, it does not count for the match)", ++g_scorekill_skips, ps, victim);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_scorekill_faults++; skip = 0;
            logf_("pvp score: EXCEPTION 0x%08lX (fault %d of 2)%s - the kill is counted as the game does", GetExceptionCode(), g_scorekill_faults, g_scorekill_faults >= 2 ? "; the NPC score filter is off for this run" : "");
        }
    }
    if (skip) return;
    g_orig_scorekill(ps, victim, points);
}

typedef void (*NotifyParry_t)(void *comp, float delay, void *actor, uint64_t name, uint8_t flag);
static NotifyParry_t g_orig_notifyparry;
static void hk_notifyparry(void *comp, float delay, void *actor, uint64_t name, uint8_t flag) {
    if (!g_disabled && g_parrylog_faults < 3 && g_tel_cfg.level != TEL_OFF) {
        __try {
            char text[360];
            int given = L_parry_inspect(actor, g_parry_fix_applied, text, (int)sizeof text);
            if (given >= 0) {
                g_parry_seen++; if (given) g_parry_ok++;
                logf_("parry #%u: delay=%.3f attacker-component=%p %s [%u/%u so far with the rewards pointer valid]", g_parry_seen, delay, comp, text, g_parry_ok, g_parry_seen);
            } else {
                logf_("parry: not inspected (%s)", text);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (++g_parrylog_faults >= 3) logf_("parry: EXCEPTION 0x%08lX (fault %d) - parry logging is off for this run", GetExceptionCode(), g_parrylog_faults);
            else logf_("parry: EXCEPTION 0x%08lX (fault %d of 3) - skipped", GetExceptionCode(), g_parrylog_faults);
        }
    }
    g_orig_notifyparry(comp, delay, actor, name, flag);
}

static void hk_tick(void *self, float dt, uint8_t idle) {
    if (!g_disabled) {
        __try { on_tick(dt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_disabled = 1;
            logf_("EXCEPTION 0x%08lX inside our tick work - LanNative is now switched off for this run", GetExceptionCode());
        }
    }
    g_orig_tick(self, dt, idle);
}

// ---------------------------------------------------------------------------------------------------------------------
static uint64_t image_size(const uint8_t *base) {          // PE SizeOfImage of a loaded module, 0 if the header looks wrong
    __try {
        if (base[0] != 'M' || base[1] != 'Z') return 0;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void init(HMODULE self) {
    wchar_t mod[MAX_PATH];
    GetModuleFileNameW(self, mod, MAX_PATH);
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash) *slash = 0;
    swprintf(g_dir, MAX_PATH, L"%s\\LanNative\\", mod);
    CreateDirectoryW(g_dir, NULL);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    g_base = (uint8_t *)GetModuleHandleW(NULL);
    logf_("LanNative M4.3.1 loaded in %ls (base %p)", wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe, g_base);
    g_self_base = (uint64_t)(uintptr_t)self;
    g_self_size = image_size((const uint8_t *)self);
    g_exe_size = image_size(g_base);
    if (!wcsstr(exe, L"Absolver-Win64-Shipping")) { logf_("not the game executable - hooks skipped"); return; }

    if (!cmdline_has_noeac(GetCommandLineW())) {
        logf_("the game was NOT started with -NoEAC, so EasyAntiCheat may be active: LanNative stays completely off. "
              "Add -NoEAC to the game's Steam launch options (offline / LAN play only) and start it again.");
        g_disabled = 1;
        return;
    }
    config_load();
    if (parryfix_enabled()) apply_parryfix();
    L_init(&g_api);
    L_npc_init(&g_npcapi);
    read_telemetry_cfg();
    T_init(&g_api, &g_tel_cfg, GetCurrentProcessId());
    g_orig_tick = (Tick_t)hook(IDX_TICK, (void *)hk_tick);
    if (!g_orig_tick) { logf_("tick hook NOT installed - LanNative does nothing (game runs untouched)"); g_disabled = 1; return; }
    g_orig_clientrestart = (ClientRestart_t)hook(IDX_CLIENTRESTART, (void *)hk_clientrestart);
    g_orig_serverack = (ServerAck_t)hook(IDX_SERVERACK, (void *)hk_serverack);
    g_orig_netcleanup = (NetCleanup_t)hook(IDX_NETCLEANUP, (void *)hk_netcleanup);
    g_orig_settravel = (SetClientTravel_t)hook(IDX_SETCLIENTTRAVEL, (void *)hk_settravel);
    if (!g_orig_settravel) logf_("travel logging is unavailable (SetClientTravel hook not installed); F6/F7 still work");
    { int mode = pvpfix_mode(); g_pvpfix_on = mode >= 1; g_pvpflow_on = mode >= 2; }
    if (g_pvpflow_on) { g_perteam_cfg = read_perteam(); L_set_perteam(g_perteam_cfg); }
    g_autoleave_on = read_autoleave();
    hostip_load();
    read_npc_cfg();
    g_menu_on = read_menu_cfg();
    if (g_menu_on) {
        g_orig_postrender = (PostRender_t)hook(IDX_POSTRENDER, (void *)hk_postrender);
        if (!g_orig_postrender) { g_menu_on = 0; logf_("menu: PostRender hook NOT installed - the menu is off"); }
    }
    if (g_pvpfix_on) {
        g_orig_returnprev = (ReturnPrev_t)hook(IDX_RETURNPREV, (void *)hk_returnprev);
        if (!g_orig_returnprev) logf_("pvp guard: hook NOT installed - duel maps will still send players back to the main map");
        else if (!g_orig_settravel) logf_("pvp guard: no travel timestamps without the SetClientTravel hook - the guard will never fire");
    }
    if (g_tel_cfg.level != TEL_OFF) {
        g_orig_notifyparry = (NotifyParry_t)hook(IDX_NOTIFYPARRY, (void *)hk_notifyparry);
        if (!g_orig_notifyparry) logf_("parry log: hook NOT installed (parry fixes are unaffected)");
    }
    if (g_pvpflow_on) {
        g_orig_scorekill = (ScoreKill_t)hook(IDX_SCOREKILL, (void *)hk_scorekill);
        if (!g_orig_scorekill) logf_("pvp score: hook NOT installed - NPC kills will count in duels");
    }
    logf_("ready (%s%s%s%s%s%s%s): F6 host, F7 join (LanNative\\ip.txt), F3 bring joiners (host), F5 state, F8 log marker, F1 menu; console initializes after the viewport exists",
          g_orig_clientrestart ? "joiner fixes " : "", g_orig_serverack ? "host tracking " : "", g_orig_netcleanup ? "body cleanup " : "",
          g_orig_settravel ? "travel log " : "", g_orig_returnprev ? "pvp guard " : "", g_pvpflow_on ? "pvp flow " : "", g_orig_notifyparry ? "parry log" : "");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        init(inst);
    }
    return TRUE;
}

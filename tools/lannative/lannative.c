#define _CRT_RAND_S
// Loader core for Absolver: mod loading, the script extender and LAN host/join (offline / LAN play only; no EAC or online-service involvement).
//
// This DLL is installed AS version.dll next to Absolver-Win64-Shipping.exe. The game imports version.dll, Windows loads this copy
// (version.dll is not a KnownDLL), every export is forwarded to the real System32 copy (version_exports.h), and the code below
// patches the game's own functions with inline hooks.
//
// A hook on UGameEngine::Tick runs the loader's per-frame work on the game thread. Host ("<map>?listen") and join
// (<ip>[:port][options] from LanNative\ip.txt) are F1 menu items.
// The per-frame work runs inside a __try block, so it needs no timers and no callbacks from other threads.
//
// Offsets/RVAs are for Absolver 1.31 b1.25_575 (the Steam build); the loader verifies the original bytes before patching.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <intrin.h>
#include "version_exports.h"
#include "gleam_version.h"
#include "lannative_targets.h"
#include "lan_logic.h"
#include "lan_telemetry.h"
#include "noeac.h"
#include "parryfix.h"
#include "patches.h"
#include "objectscan.h"
#include "console.h"
#include "joinurl.h"
#include "pvpgate.h"
#include "parrylog.h"
#include "menu.h"
#include "textinput.h"
#include "hostip.h"
#include "config.h"
#ifdef LN_OVERLAY
#include "overlay.h"
#endif
#include "matchredir.h"
#include "modconf.h"
#include "lanmods.h"
#include "rowsjson.h"
#include "utf8text.h"
#include "joinguard.h"
#include "hookreg.h"

#define RVA_GENGINE         0x38A2E68u   // UEngine *GEngine
#define RVA_GWORLD          0x38A5890u   // UWorldProxy GWorld (first 8 bytes = UWorld *)
#define IDX_TICK            0
#define IDX_PROCESSINTERNAL 18           // UObject::ProcessInternal(this, FFrame &, void *result): Blueprint script functions (secore.inc)
#define IDX_PROCESSEVENT    17           // UObject::ProcessEvent(this, UFunction *, void *params): script extender (secore.inc)
#define IDX_CLIENTRESTART   1
#define IDX_SERVERACK       2
#define IDX_SETCLIENTTRAVEL 4
#define IDX_NETCLEANUP      6
#define IDX_RETURNPREV      7            // UThePlainesGameInstance::BPF_ReturnToPreviousMap (bool, this, one pointer argument)
#define IDX_POSTRENDER      9            // UGameViewportClient::PostRender (this, UCanvas *): menu text
#define IDX_SCOREKILL       10           // AAdversarialPlayerState::ScoreKill (this, victim PlayerState, points): NPC kills do not score in duels
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
#define OFF_ENGINE_MEDIUMFONT   0x70u     // UEngine::MediumFont (PDB)
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
#define RVA_ZEN_CLS_STARTSCENE   0x55BCE0u  // AAdversarialStartGameScene::GetPrivateStaticClass
#define RVA_ZEN_CLS_PLAYERSTART  0x18F4F60u // APlayerStart::GetPrivateStaticClass
#define RVA_ZEN_RESTARTGAME      0x3E12C0u  // AAdversarialGameMode::RestartGame
#define RVA_ZEN_CLS_TEAMSTART    0x633BD0u  // ATeamPlayerStart::GetPrivateStaticClass
#define RVA_ZEN_CLS_ACTOR        0x17FCB20u // AActor::GetPrivateStaticClass
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
static DWORD g_game_tid;                  // thread that runs UGameEngine::Tick (set by the tick hook)
static void rows_drain_objects(void);
static int g_safe_mode;                  // safe mode (crashrep.inc): no mods, no plugins, no extender hooks this run
static void crashguard_ready(void);      // crashrep.inc
static ObjOps g_objops;                  // data asset edits (object / ops entries of rows.json), applied when the object finishes loading
static RowOps g_rowops;                  // rows.json edits of all enabled mods in load order (filled by the mod loader, applied by rowedit.inc)
static char g_posname[MODS_MAX + 1][MOD_NAME_MAX];   // name of the mod at each load position (mounted mods and rows-only mods)
static LmInfo g_lm_me;                   // this PC's gameplay-mod fingerprint and names (filled by the mod loader); present = 0 until the mod check is installed
static int g_lm_mode = 1;                // modmismatch: 0 off, 1 warn, 2 block
static void lm_serverack_post(void *pc);
static void lm_selftest(void);
static void jg_announce_host(void);
static JoinGuard g_jg;
static void toast_show(char lines[][LM_LINE_MAX], int n, unsigned seconds);
static int jg_key_for_url(wchar_t *out, size_t cap);
static void toast_draw(void *canvas);
static int toast_active(void);
static void md_draw(void *canvas); static int md_draw_active(void);                    // modes.inc
static wchar_t g_dir[MAX_PATH];          // the LanNative folder next to the DLL, with a trailing backslash
static CRITICAL_SECTION g_lock;
static HookReg g_hreg;                  // which functions are patched and how their detours stack (hookreg.h); the loader's own hooks are registered here too
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
static int g_pvpfix_on = 1;                              // pvpfix setting: send-back guard (on by default)
static int g_pvpflow_on = 1;                             // pvpfix setting: duel debug flow (on by default, off with "guard")
static int g_flow_faults;                                // the duel flow switches itself off after 2 faults
static int g_result_faults;                              // the duel result writer switches itself off after 2 faults
static int g_parry_fix_applied;                          // the one-byte parry fix is in memory (for the parry observer's wording)
static int g_parrylog_faults;                            // the parry observer switches itself off after 3 faults
static unsigned g_parry_seen, g_parry_ok;                // parries observed / with a valid pointer

// logging (plain Win32 so it is safe from DllMain too)
static void logf_(const char *fmt, ...) {
    char msg[1024], line[1100];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    static HANDLE s_log = INVALID_HANDLE_VALUE;                 // open for the whole run; rotated at 4 MB when opened
    EnterCriticalSection(&g_lock);
    if (s_log == INVALID_HANDLE_VALUE) {
        wchar_t path[MAX_PATH], old[MAX_PATH];
        swprintf(path, MAX_PATH, L"%slannative.log", g_dir); swprintf(old, MAX_PATH, L"%slannative.log.1", g_dir);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && ((((uint64_t)fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > 4u * 1024 * 1024) MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        s_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (s_log != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(s_log, line, (DWORD)n, &w, NULL); }
    LeaveCriticalSection(&g_lock);
}

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
    hr_add(&g_hreg, t->rva, t->stolen, t->bytes, tramp, replacement, t->name);
    logf_("hook %s: installed at base+0x%X", t->name, t->rva);
    return tramp;
}

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
typedef uint8_t (*ReturnPrev_t)(void *gi, void *arg);
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

// NPC spawning: the engine calls (the logic is in lan_logic.c). Every address is byte-checked once before the first use.
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
static void *npc_spawn_actor_rot(void *w, void *cls, const float loc[3], const float rot[3]) {   // rot = FRotator (pitch, yaw, roll) in degrees
    uint8_t params[0x30];
    ((void *(*)(void *))(g_base + RVA_NPC_PARAMS_CTOR))(params);
    params[0x28] = 1;                                    // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    *(uint16_t *)(params + 0x2A) |= 2;                   // bNoFail (the game's own AAISpawner::SpawnAI sets the same)
    return ((void *(*)(void *, void *, const float *, const float *, void *))(g_base + RVA_NPC_SPAWNACTOR))(w, cls, loc, rot, params);
}
static void *npc_spawn_actor(void *w, void *cls, const float loc[3]) {
    const float rot[3] = { 0.f, 0.f, 0.f };
    return npc_spawn_actor_rot(w, cls, loc, rot);
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
static const NpcApi g_npcapi = { npc_cls_spawner, npc_cls_archetype, npc_spawn_actor, npc_can_spawn, npc_wants_spawn, npc_unspawn, npc_set_replicates, npc_set_archetype, npc_load_object, npc_spawn_actor_rot };

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

// ZenArena was authored with ordinary UE PlayerStart actors. Absolver's duel mode only selects its native
// AAdversarialStartGameScene class, so add one at each authored location once, then let RestartGame run its normal flow.
// This is intentionally exact-map only: it must never affect a stock or another downloaded duel map.
#define OFF_ADVSTART_TEAM 0x468u
#define OFF_TEAMSTART_TEAM 0x3F0u   // ATeamPlayerStart team: UTeamSpawningComponent::IsSpawnValid compares it with AFightingPlayerState+0x45C
#define ZEN_MAX_CAND 24
typedef struct { float pos[3], rot[3]; int in_level; char path[260]; } ZenCand;
typedef struct { void *cls; const char *level_prefix; ZenCand c[ZEN_MAX_CAND]; int count, total; } ZenStarts;
typedef void (*RestartGame_t)(void *gm);
static void *g_zen_world;
static int g_zen_done, g_zen_verified, g_zen_logged_total = -1;

// Every live non-template APlayerStart is recorded and logged; only those whose object path sits in the map's own
// PersistentLevel (<world path>:PersistentLevel.<name>) are used, so Zen streaming sublevels/templates are ignored.
static void zen_find_start(void *object, void *context) {
    ZenStarts *starts = (ZenStarts *)context;
    if ((*(uint32_t *)((uint8_t *)object + OFF_OBJ_FLAGS) & 0x30u) || !object_is_a(object, starts->cls)) return;
    starts->total++;
    if (starts->count == ZEN_MAX_CAND) return;
    void *root = *(void **)((uint8_t *)object + OFF_ACTOR_ROOTCOMP);
    if (!root) return;
    float *p = (float *)((uint8_t *)root + OFF_ROOT_C2W_POS);
    for (int i = 0; i < 3; i++) if (!(p[i] == p[i]) || p[i] < -10000000.f || p[i] > 10000000.f) return;
    ZenCand *c = &starts->c[starts->count++];
    memcpy(c->pos, p, sizeof c->pos);
    const float *q = p - 4;                                  // FTransform::Rotation quaternion (x y z w) sits just before the translation
    float x = q[0], y = q[1], z = q[2], w = q[3];
    double sp = 2.0 * (w * y - z * x);
    if (sp > 1.0) sp = 1.0; else if (sp < -1.0) sp = -1.0;
    const double rad = 57.29577951308232;
    c->rot[0] = (float)(asin(sp) * rad);
    c->rot[1] = (float)(atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z)) * rad);
    c->rot[2] = (float)(atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y)) * rad);
    c->path[0] = 0;
    api_object_path(object, c->path, (int)sizeof c->path);
    c->in_level = strncmp(c->path, starts->level_prefix, strlen(starts->level_prefix)) == 0;
}

static int zen_verify(void) {
    static const uint8_t class_bytes[] = { 0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0x05 };
    static const uint8_t spawn_bytes[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x70 };
    static const uint8_t ctor_bytes[] = { 0x33, 0xC0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08 };
    static const uint8_t restart_bytes[] = { 0x40, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x28 };
    if (g_zen_verified) return g_zen_verified > 0;
    g_zen_verified = memcmp(g_base + RVA_ZEN_CLS_STARTSCENE, class_bytes, sizeof class_bytes) == 0 &&
                     memcmp(g_base + RVA_ZEN_CLS_PLAYERSTART, class_bytes, sizeof class_bytes) == 0 &&
                     memcmp(g_base + RVA_ZEN_CLS_TEAMSTART, class_bytes, sizeof class_bytes) == 0 &&
                     memcmp(g_base + RVA_NPC_SPAWNACTOR, spawn_bytes, sizeof spawn_bytes) == 0 &&
                     memcmp(g_base + RVA_NPC_PARAMS_CTOR, ctor_bytes, sizeof ctor_bytes) == 0 &&
                     memcmp(g_base + RVA_ZEN_RESTARTGAME, restart_bytes, sizeof restart_bytes) == 0 ? 1 : -1;
    logf_("zenarena: %s", g_zen_verified > 0 ? "engine functions verified" : "engine bytes differ; start bridge is off for this run");
    return g_zen_verified > 0;
}


// ZenArena streams its scenery (Zen_Vis, backgrounds, FX, gameplay) as LevelStreamingAlwaysLoaded sublevels. Read-only diagnostic for a
// joiner's pawn that falls through the map: log every streaming level's class, request bits and whether its
// loaded level is visible, once a second for the first 30 s, on both PCs (ULevelStreaming bitfield +0xB0: bit3 ShouldBeVisible,
// bit5 ShouldBeLoaded; UWorld::StreamingLevels at +0x88; loaded level at +0x140 with its visible flag in bit3 of the byte at +0x20C).
#define OFF_W_STREAMLEVELS 0x88u
#define OFF_LS_FLAGS 0xB0u
#define OFF_LS_LOADEDLEVEL 0x140u
#define OFF_LEVEL_VISFLAGS 0x20Cu
static void *g_zs_world;
static uint64_t g_zs_next_ms, g_zs_first_ms;
static int g_zs_dumped;


// Read-only comparison aid: list every live actor whose XY is within ZEN_NEAR_R of either duel start, so the host and joiner logs can be
// diffed to find the floor/collision actor one of the worlds is missing.
#define ZEN_NEAR_R 700.f
#define ZEN_NEAR_MAX 80
typedef struct { void *cls; int found, logged; } ZenNear;
static void zen_near_visit(void *object, void *context) {
    static const float centers[2][2] = { { -268.1f, -2820.f }, { -538.1f, -1350.f } };
    ZenNear *zn = (ZenNear *)context;
    if ((*(uint32_t *)((uint8_t *)object + OFF_OBJ_FLAGS) & 0x30u) || !object_is_a(object, zn->cls)) return;
    void *root = *(void **)((uint8_t *)object + OFF_ACTOR_ROOTCOMP);
    if (!root) return;
    const float *p = (const float *)((uint8_t *)root + OFF_ROOT_C2W_POS);
    int which = -1;
    for (int i = 0; i < 2; i++) {
        float dx = p[0] - centers[i][0], dy = p[1] - centers[i][1];
        if (dx == dx && dy == dy && dx * dx + dy * dy <= ZEN_NEAR_R * ZEN_NEAR_R) { which = i; break; }
    }
    if (which < 0) return;
    zn->found++;
    if (zn->logged >= ZEN_NEAR_MAX) return;
    char nm[260] = "?", cn[160] = "?";
    api_object_path(object, nm, (int)sizeof nm);
    api_object_path(*(void **)((uint8_t *)object + 0x10), cn, (int)sizeof cn);
    zn->logged++;
    logf_("zenarena: near start%d: %s [%s] at (%.0f, %.0f, %.0f)", which + 1, nm, cn, p[0], p[1], p[2]);
}
static void zen_near_dump(int tag) {
    static const uint8_t class_bytes[] = { 0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0x05 };
    if (memcmp(g_base + RVA_ZEN_CLS_ACTOR, class_bytes, sizeof class_bytes) != 0) { logf_("zenarena: AActor class getter bytes differ; actor dump skipped"); return; }
    ZenNear zn = { ((void *(*)(void))(g_base + RVA_ZEN_CLS_ACTOR))(), 0, 0 };
    logf_("zenarena: --- actors within %.0f units of the two starts (dump %d) ---", ZEN_NEAR_R, tag);
    api_scan_objects(zen_near_visit, &zn);
    logf_("zenarena: --- %d actor(s) found, %d listed ---", zn.found, zn.logged);
}

static void zen_stream_watch(void) {
    void *w = world();
    if (!w) return;
    if (w != g_zs_world) { g_zs_world = w; g_zs_next_ms = 0; g_zs_first_ms = 0; g_zs_dumped = 0; }
    char path[200];
    if (!api_object_path(w, path, (int)sizeof path) || !pvp_path_is_zenarena(path)) return;
    uint64_t now = GetTickCount64();
    if (g_zs_first_ms && now - g_zs_first_ms >= 30000) return;
    if (now < g_zs_next_ms) return;
    g_zs_next_ms = now + 1000;
    if (!g_zs_first_ms) g_zs_first_ms = now;
    if (g_zs_dumped < 1 && now - g_zs_first_ms >= 8000) { g_zs_dumped = 1; zen_near_dump(1); }
    else if (g_zs_dumped < 2 && now - g_zs_first_ms >= 25000) { g_zs_dumped = 2; zen_near_dump(2); }
    uint8_t *arr = *(uint8_t **)((uint8_t *)w + OFF_W_STREAMLEVELS);
    int n = *(int32_t *)((uint8_t *)w + OFF_W_STREAMLEVELS + 8);
    if (!arr || n < 0 || n > 64) { logf_("zenarena: streaming levels array looks wrong (%p, %d)", arr, n); return; }
    for (int i = 0; i < n; i++) {
        uint8_t *ls = ((uint8_t **)arr)[i];
        if (!ls) continue;
        uint32_t fl = *(uint32_t *)(ls + OFF_LS_FLAGS);
        uint8_t *lvl = *(uint8_t **)(ls + OFF_LS_LOADEDLEVEL);
        int vis = lvl ? ((*(uint8_t *)(lvl + OFF_LEVEL_VISFLAGS) >> 3) & 1) : 0;
        char nm[200] = "?";
        api_object_path(ls, nm, (int)sizeof nm);
        logf_("zenarena: t+%llus stream[%d] %s flags=0x%08X shouldLoad=%d shouldVis=%d loaded=%s levelVisible=%d", (unsigned long long)((now - g_zs_first_ms) / 1000), i, nm, fl, (fl >> 5) & 1, (fl >> 3) & 1, lvl ? "yes" : "no", vis);
    }
}

static void zen_bootstrap(void) {
    void *w = world();
    if (w != g_zen_world) { g_zen_world = w; g_zen_done = 0; }
    if (!w || g_zen_done) return;
    char path[200];
    if (!api_object_path(w, path, (int)sizeof path) || !pvp_path_is_zenarena(path)) return;
    if (!zen_verify()) { g_zen_done = -1; return; }
    void *gm = *(void **)((uint8_t *)w + OFF_W_AUTHGM);
    if (!gm || L_is_joiner() || !object_is_a(gm, api_cls_advgm())) return;
    char prefix[260];
    snprintf(prefix, sizeof prefix, "%s:PersistentLevel.", path);
    ZenStarts starts;
    memset(&starts, 0, sizeof starts);
    starts.cls = ((void *(*)(void))(g_base + RVA_ZEN_CLS_PLAYERSTART))();
    starts.level_prefix = prefix;
    api_scan_objects(zen_find_start, &starts);
    int use[2], nuse = 0, nlevel = 0, lastidx[2] = { -1, -1 };
    for (int i = 0; i < starts.count; i++) if (starts.c[i].in_level) { lastidx[0] = lastidx[1]; lastidx[1] = i; nlevel++; }
    // With more than two starts in the persistent level the map author added extra ones last; the two newest are the ones meant for the duel.
    if (nlevel >= 2) { use[0] = lastidx[0]; use[1] = lastidx[1]; nuse = 2; }
    else nuse = nlevel;
    if (nlevel > 2 && starts.total != g_zen_logged_total) logf_("zenarena: %d starts in the persistent level; using the last two (indexes %d and %d)", nlevel, use[0], use[1]);
    if (starts.total != g_zen_logged_total) {
        g_zen_logged_total = starts.total;
        logf_("zenarena: %d live APlayerStart object(s), %d recorded, %d in this map's PersistentLevel (prefix %s)", starts.total, starts.count, nuse, prefix);
        for (int i = 0; i < starts.count; i++) {
            const ZenCand *c = &starts.c[i];
            logf_("zenarena:   [%d] %s pos=(%.1f, %.1f, %.1f) rot(p,y,r)=(%.1f, %.1f, %.1f)%s", i, c->path, c->pos[0], c->pos[1], c->pos[2], c->rot[0], c->rot[1], c->rot[2], c->in_level ? " <- PersistentLevel" : "");
        }
    }
    if (nuse < 2) return;                                   // not loaded yet (or the map has fewer than two starts); retry next tick
    void *scene_cls = ((void *(*)(void))(g_base + RVA_ZEN_CLS_STARTSCENE))();
    const ZenCand *ca = &starts.c[use[0]], *cb = &starts.c[use[1]];
    void *a = scene_cls ? npc_spawn_actor_rot(w, scene_cls, ca->pos, ca->rot) : NULL;
    void *b = scene_cls ? npc_spawn_actor_rot(w, scene_cls, cb->pos, cb->rot) : NULL;
    if (!a || !b || !object_is_a(a, scene_cls) || !object_is_a(b, scene_cls)) {
        g_zen_done = -1;
        logf_("zenarena: could not spawn both duel start scenes (%p, %p); bridge is off for this world", a, b);
        return;
    }
    *(int32_t *)((uint8_t *)a + OFF_ADVSTART_TEAM) = 0;
    *(int32_t *)((uint8_t *)b + OFF_ADVSTART_TEAM) = 1;
    // The actual spawn choice (UTeamSpawningComponent::GetSpawnPoint) only accepts ATeamPlayerStart actors whose team matches the
    // player's team, so a start scene alone leaves both players on the fallback spot. Add one team start per location too.
    void *team_cls = ((void *(*)(void))(g_base + RVA_ZEN_CLS_TEAMSTART))();
    void *ta = team_cls ? npc_spawn_actor_rot(w, team_cls, ca->pos, ca->rot) : NULL;
    void *tb = team_cls ? npc_spawn_actor_rot(w, team_cls, cb->pos, cb->rot) : NULL;
    if (!ta || !tb || !object_is_a(ta, team_cls) || !object_is_a(tb, team_cls)) {
        g_zen_done = -1;
        logf_("zenarena: could not spawn both team player starts (%p, %p); bridge is off for this world", ta, tb);
        return;
    }
    *(int32_t *)((uint8_t *)ta + OFF_TEAMSTART_TEAM) = 0;
    *(int32_t *)((uint8_t *)tb + OFF_TEAMSTART_TEAM) = 1;
    logf_("zenarena: team starts %p (team 0) and %p (team 1), scenes %p and %p", ta, tb, a, b);
    ((RestartGame_t)(g_base + RVA_ZEN_RESTARTGAME))(gm);
    g_zen_done = 1;
    logf_("zenarena: added duel starts from [%d] %s at (%.0f, %.0f, %.0f) yaw %.0f and [%d] %s at (%.0f, %.0f, %.0f) yaw %.0f; restarted 1v1",
          use[0], ca->path, ca->pos[0], ca->pos[1], ca->pos[2], ca->rot[1], use[1], cb->path, cb->pos[0], cb->pos[1], cb->pos[2], cb->rot[1]);
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

// F10 = teleport your own character to the first line of LanNative\goto.txt ("x y z", UE cm, capsule centre). For map testing: rewrite the file and press F10.
static void goto_file(void) {
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sgoto.txt", g_dir);
    FILE *f = _wfopen(path, L"r"); if (!f) { logf_("goto: %ls not found", path); return; }
    float xyz[3]; int n = fscanf(f, "%f %f %f", &xyz[0], &xyz[1], &xyz[2]); fclose(f);
    if (n != 3) { logf_("goto: %ls must hold three numbers: x y z", path); return; }
    L_teleport_local(xyz);
}


// F9 = free camera. Spawns an engine ACameraActor at a pose from LanNative\cam.txt ("x y z pitch yaw [roll]", UE cm and degrees; or "off") and points this PC's view
// at it with APlayerController::SetViewTarget, so the camera passes through walls and ceilings and the character stays where it is (the shipping build strips the
// engine's own ToggleDebugCamera). Every press re-reads the file, so a script can fly the camera through a map: write the file, press F9. "off" gives the view back
// to the character. Host / single player only (the camera is local, nothing is replicated).
#define RVA_CLS_CAMERAACTOR  0x1828F10u  // ACameraActor::GetPrivateStaticClass
#define RVA_PC_SETVIEWTARGET 0x167D310u  // APlayerController::SetViewTarget(this, AActor *, const FViewTargetTransitionParams *)
static WeakPtr g_cam = { -1, 0 };
static int g_cam_verified;               // 0 = not checked, 1 = ok, -1 = bytes differ (free camera stays off)
static int freecam_verify(void) {
    static const uint8_t cls[] = { 0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x78, 0x48, 0x8B, 0x05 };
    static const uint8_t svt[] = { 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x81, 0x20, 0x04, 0x00, 0x00 };
    if (g_cam_verified) return g_cam_verified > 0;
    g_cam_verified = (!memcmp(g_base + RVA_CLS_CAMERAACTOR, cls, sizeof cls) && !memcmp(g_base + RVA_PC_SETVIEWTARGET, svt, sizeof svt)) ? 1 : -1;
    if (g_cam_verified < 0) logf_("freecam: ACameraActor / SetViewTarget bytes differ from the expected build - the free camera is off for this run");
    return g_cam_verified > 0;
}
static void freecam_set_view(void *pc, void *target) {
    uint8_t blend[16] = { 0 };                                   // FViewTargetTransitionParams: BlendTime 0 = instant
    ((void (*)(void *, void *, const void *))(g_base + RVA_PC_SETVIEWTARGET))(pc, target, blend);
}
// move the free camera to pose v = { x, y, z, pitch, yaw, roll } (spawns the camera actor on first use, replaces it afterwards)
static int freecam_apply(const float v[6]) {
    void *w = world(), *pc = L_local_pc();
    if (!w || !pc) { logf_("freecam: no world or no local player controller"); return 0; }
    void *old = api_weak_get(&g_cam);
    const float loc[3] = { v[0], v[1], v[2] }, rot[3] = { v[3], v[4], v[5] };   // FRotator = (pitch, yaw, roll)
    if (old) { api_destroy(old); api_weak_set(&g_cam, NULL); }
    void *cam = npc_spawn_actor_rot(w, ((void *(*)(void))(g_base + RVA_CLS_CAMERAACTOR))(), loc, rot);
    if (!cam) { logf_("freecam: could not spawn the camera actor"); return 0; }
    api_weak_set(&g_cam, cam);
    freecam_set_view(pc, cam);
    logf_("freecam: camera at %.0f,%.0f,%.0f pitch %.0f yaw %.0f roll %.0f (ok)", loc[0], loc[1], loc[2], rot[0], rot[1], rot[2]);
    return 1;
}
static void freecam_file(void) {
    __try {
        if (!freecam_verify()) return;
        wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%scam.txt", g_dir);
        FILE *f = _wfopen(path, L"r"); if (!f) { logf_("freecam: %ls not found", path); return; }
        char line[256] = ""; if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f);
        void *w = world(), *pc = L_local_pc();
        if (!w || !pc) { logf_("freecam: no world or no local player controller"); return; }
        void *old = api_weak_get(&g_cam);
        if (!_strnicmp(line, "off", 3)) {
            void *pawn = *(void **)((uint8_t *)pc + OFF_CTRL_PAWN);
            if (pawn) freecam_set_view(pc, pawn);
            if (old) api_destroy(old);
            api_weak_set(&g_cam, NULL);
            logf_("freecam: off, the view is back on the character");
            return;
        }
        float v[6] = { 0, 0, 0, 0, 0, 0 };
        int n = sscanf(line, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        if (n < 5) { logf_("freecam: %ls must hold: x y z pitch yaw [roll]   (or: off)", path); return; }
        freecam_apply(v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("freecam: EXCEPTION 0x%08lX - ignored (view target left as it was)", GetExceptionCode());
    }
}


// Room spawns. A Downfall room opened from the F1 map list has no PlayerStart, so the character appears at the world origin, which is over
// a pit in some rooms. LanNative\room_spawns.tsv ("<map name> x y z", a spot just inside the room's first door) gives a spot per map name; every
// new pawn of the local player in such a map (arrival and each respawn) is moved there on the next poll after it appears (a later move lets it
// fall into the pit first). Maps without a line keep the game's spawn.
static void se_path_of(const void *obj, char *out, int cap);                    // secore.inc
#define ROOM_MAX 160
static struct { char map[80]; float xyz[3]; } g_room[ROOM_MAX];
static int g_room_n = -1;
// Inner Palace in rooms: meditation moves the player to the Training level's spot (its level script sets the game state's
// m_TrainingLocation; about z -20000, far below every room). MainMap streams that level, a room opened from the F1 list does not, so without it
// meditation leaves the player in the room. Load it once per room world (/PVE/Theme_* = the single-room maps; Downfall's own PVE_Mine is left alone).
static void room_palace(void);                                                 // below the includes (uses the secore / plugins helpers)
static void room_spawn_tick(void) {
    static void *last_pawn, *last_world; static uint64_t due, next_poll; static int idx = -1;
    uint64_t now = GetTickCount64(); if (now < next_poll) return; next_poll = now + 100;
    room_palace();
    if (g_room_n < 0) {
        g_room_n = 0; wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sroom_spawns.tsv", g_dir); FILE *f = _wfopen(p, L"r"); char line[256];
        while (f && g_room_n < ROOM_MAX && fgets(line, sizeof line, f)) { if (line[0] == '#') continue; float x, y, z; char m[80];
            if (sscanf(line, "%79s %f %f %f", m, &x, &y, &z) == 4) { snprintf(g_room[g_room_n].map, sizeof g_room[0].map, "%s", m); g_room[g_room_n].xyz[0] = x; g_room[g_room_n].xyz[1] = y; g_room[g_room_n].xyz[2] = z; g_room_n++; } }
        if (f) fclose(f); logf_("rooms: %d room spawn(s) from room_spawns.tsv", g_room_n);
    }
    if (!g_room_n) return;
    void *w = world(), *pc = L_local_pc(), *pawn = pc ? *(void **)((uint8_t *)pc + OFF_CTRL_PAWN) : NULL;
    if (w != last_world) {
        last_world = w; idx = -1; last_pawn = NULL; char path[300] = ""; if (w) __try { se_path_of(w, path, sizeof path); } __except (EXCEPTION_EXECUTE_HANDLER) { path[0] = 0; }
        const char *nm = strrchr(path, '.'); nm = nm ? nm + 1 : path;
        for (int i = 0; i < g_room_n; i++) if (!_stricmp(nm, g_room[i].map)) { idx = i; logf_("rooms: %s has a room spawn (%.0f, %.0f, %.0f)", nm, g_room[i].xyz[0], g_room[i].xyz[1], g_room[i].xyz[2]); break; }
    }
    if (idx < 0) return;
    if (pawn && pawn != last_pawn) { last_pawn = pawn; due = now; }
    if (due && now >= due) { due = 0; __try { logf_("rooms: moving the character to the room spawn of %s -> %d", g_room[idx].map, L_teleport_local(g_room[idx].xyz)); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("rooms: teleport EXCEPTION 0x%08lX", GetExceptionCode()); } }
}

#ifdef LANNATIVE_SHOTRUNNER   // dev build only; never ship the screenshot/probe runner
// Shot runner (map testing without keyboard input). When LanNative\shots.run exists, the loader reads LanNative\shots.txt (and deletes shots.run) and executes one
// command per game frame, in the game thread, so it needs neither window focus nor the console:
//   cam x y z pitch yaw [roll]   free camera pose (as F9)       exec <console command>   e.g. exec r.EyeAdaptationQuality 0
//   shot <name>                  HighResShot 1 (the engine names the file HighresScreenshotNNNNN.png; the order is logged "shots: shot N <name>")
//   wait <frames>                pause                           done                      view back to the character, writes LanNative\shots.done
// After a cam line the runner waits SHOT_SETTLE frames, after a shot SHOT_AFTER frames. Lines starting with # are ignored. Everything is logged ("shots: ...").
#define RVA_PC_CONSOLECMD 0x16651C0u  // APlayerController::ConsoleCommand(this, FString *ret, const FString &cmd, bool bWriteToLog) -> returns ret
#define SHOT_SETTLE 4
#define SHOT_AFTER  5
typedef struct { wchar_t *data; int32_t num, max; } FStr_;
static char **g_sr_lines; static int g_sr_n, g_sr_pos, g_sr_wait, g_sr_count; static int g_sr_active;
static int shotrunner_exec(void *pc, const char *cmd) {
    static const uint8_t sig[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x89, 0x08, 0x04, 0x00, 0x00, 0x48, 0x8B, 0xDA };
    if (memcmp(g_base + RVA_PC_CONSOLECMD, sig, sizeof sig) != 0) { logf_("shots: ConsoleCommand bytes differ from the expected build - cannot run console commands"); return 0; }
    wchar_t buf[320]; size_t n = 0; while (cmd[n] && n < 318) { buf[n] = (wchar_t)(unsigned char)cmd[n]; n++; } buf[n] = 0;
    FStr_ cs = { buf, (int32_t)n + 1, 320 }, ret = { 0, 0, 0 };
    ((void *(*)(void *, FStr_ *, FStr_ *, int))(g_base + RVA_PC_CONSOLECMD))(pc, &ret, &cs, 0);
    if (ret.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(ret.data);
    return 1;
}

// ---- collision probe (dev build): ask the ENGINE whether a ray is blocked, so the collision that was actually cooked and loaded can be compared with what was intended
//   trace x0 y0 z0 x1 y1 z1 [channel]   logs "shots: trace hit=H t=T"      (channel: 0 WorldStatic, 1 WorldDynamic, 2 Pawn (default), 3 Visibility, 4 Camera)
//   tracedown x y z0 z1                 logs the hit buffer as floats once (shows the FHitResult layout)
//   tracefile                           reads LanNative\trace_in.csv ("x0,y0,z0,x1,y1,z1[,channel]" per line) and writes trace_out.csv ("hit,time") for every line, all in one frame
#define RVA_LINETRACE 0x1362CE0u   // UWorld::LineTraceSingleByChannel(this, FHitResult *out, const FVector *start, const FVector *end, ECollisionChannel, const FCollisionQueryParams *, const FCollisionResponseParams *) -> bool in AL
#define RVA_DEFQUERY  0x387DD20u   // FCollisionQueryParams::DefaultQueryParam (global object)
#define RVA_DEFRESP   0x387DC50u   // FCollisionResponseParams::DefaultResponseParam (global object)
#define HIT_TIME_OFF  4            // FHitResult::Time (float 0..1 along the trace); confirmed by tracedown
static int trace_verify(void) {
    static const uint8_t sig[] = { 0x48, 0x83, 0xEC, 0x68, 0x41, 0x8B, 0x41, 0x08, 0xF2, 0x41, 0x0F, 0x10, 0x01, 0x4C, 0x8D, 0x4C, 0x24, 0x40 };
    return memcmp(g_base + RVA_LINETRACE, sig, sizeof sig) == 0;
}
static int trace_line(const float a_[3], const float b_[3], int chan, uint8_t *hit, float *t) {
    void *w = world(); if (!w) return -1;
    memset(hit, 0, 0x180);
    uint8_t qp[160]; const void *qparams = g_base + RVA_DEFQUERY;                       // channel >= 100: same trace against COMPLEX collision (FCollisionQueryParams::bTraceComplex, byte 0x10 of the default object)
    if (chan >= 100) { memcpy(qp, g_base + RVA_DEFQUERY, 112); qp[0x10] = 1; qparams = qp; chan -= 100; }
    uint8_t r = ((uint8_t (*)(void *, void *, const float *, const float *, int, const void *, const void *))(g_base + RVA_LINETRACE))(w, hit, a_, b_, chan, qparams, g_base + RVA_DEFRESP);
    if (t) *t = *(float *)(hit + HIT_TIME_OFF);
    return r != 0;
}
static void trace_file(void) {
    wchar_t pin[MAX_PATH], pout[MAX_PATH]; swprintf(pin, MAX_PATH, L"%strace_in.csv", g_dir); swprintf(pout, MAX_PATH, L"%strace_out.csv", g_dir);
    FILE *fi = _wfopen(pin, L"r"); if (!fi) { logf_("shots: %ls not found", pin); return; }
    FILE *fo = _wfopen(pout, L"w"); if (!fo) { fclose(fi); logf_("shots: cannot write %ls", pout); return; }
    char line[256]; int n = 0, hits = 0; uint8_t hit[0x180];
    while (fgets(line, sizeof line, fi)) {
        float v[6]; int ch = 2;
        int k = sscanf(line, "%f,%f,%f,%f,%f,%f,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &ch);
        if (k < 6) { fprintf(fo, "-1,0\n"); continue; }
        float t = 0; int r = trace_line(v, v + 3, ch, hit, &t);
        fprintf(fo, "%d,%.4f\n", r, t); n++; hits += r > 0;
    }
    fclose(fi); fclose(fo);
    logf_("shots: tracefile done, %d rays, %d blocked", n, hits);
}


// ---- capsule sweep (dev build): the query character movement really uses. sweep x0 y0 z0 x1 y1 z1 radius halfheight [channel]  /  sweepfile (sweep_in.csv "x0,y0,z0,x1,y1,z1,r,hh[,ch]" -> sweep_out.csv "hit,time")
#define RVA_SWEEP  0x136A8B0u   // UWorld::SweepSingleByChannel(this, FHitResult *, const FVector *start, const FVector *end, const FQuat *rot, ECollisionChannel, const FCollisionShape *, const FCollisionQueryParams *, const FCollisionResponseParams *) -> bool in AL
#define RVA_QUAT_ID 0x339B790u  // FQuat::Identity (16 bytes)
static int sweep_verify(void) {
    static const uint8_t sig[] = { 0x48, 0x83, 0xEC, 0x78, 0x4C, 0x8B, 0xDA, 0x48, 0x8B, 0x94, 0x24, 0xB0 };
    return memcmp(g_base + RVA_SWEEP, sig, sizeof sig) == 0;
}
static int sweep_capsule(const float a_[3], const float b_[3], float radius, float halfheight, int chan, uint8_t *hit, float *t) {
    void *w = world(); if (!w) return -1;
    memset(hit, 0, 0x180);
    struct { int32_t type; float radius, halfheight, pad; } shape = { 3, radius, halfheight, 0 };      // ECollisionShape::Capsule = 3
    uint8_t r = ((uint8_t (*)(void *, void *, const float *, const float *, const void *, int, const void *, const void *, const void *))(g_base + RVA_SWEEP))(w, hit, a_, b_, g_base + RVA_QUAT_ID, chan, &shape, g_base + RVA_DEFQUERY, g_base + RVA_DEFRESP);
    if (t) *t = *(float *)(hit + HIT_TIME_OFF);
    return r != 0;
}
static void sweep_file(void) {
    wchar_t pin[MAX_PATH], pout[MAX_PATH]; swprintf(pin, MAX_PATH, L"%ssweep_in.csv", g_dir); swprintf(pout, MAX_PATH, L"%ssweep_out.csv", g_dir);
    FILE *fi = _wfopen(pin, L"r"); if (!fi) { logf_("shots: %ls not found", pin); return; }
    FILE *fo = _wfopen(pout, L"w"); if (!fo) { fclose(fi); logf_("shots: cannot write %ls", pout); return; }
    char line[256]; int n = 0, hits = 0; uint8_t hit[0x180];
    while (fgets(line, sizeof line, fi)) {
        float v[8]; int ch = 2;
        int k = sscanf(line, "%f,%f,%f,%f,%f,%f,%f,%f,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &ch);
        if (k < 8) { fprintf(fo, "-1,0\n"); continue; }
        float t = 0; int r = sweep_capsule(v, v + 3, v[6], v[7], ch, hit, &t);
        fprintf(fo, "%d,%.4f\n", r, t); n++; hits += r > 0;
    }
    fclose(fi); fclose(fo);
    logf_("shots: sweepfile done, %d sweeps, %d blocked", n, hits);
}


// ---- real walking (dev build): walk dx dy frames = APawn::AddMovementInput(pawn, dir, 1.0, force) once per frame for N frames; read the result with `where` (the movement component does the collision, exactly as in play)
#define RVA_ADDMOVE 0x16161F0u   // APawn::AddMovementInput(this, const FVector *worldDirection, float scale (xmm2), bool force (r9b))
static int g_walk_left; static float g_walk_dir[3];
static int walk_verify(void) {
    static const uint8_t sig[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x01 };
    static const uint8_t character_ctor[]={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C};
    static const uint8_t stop_move[]={0xF2,0x0F,0x10,0x05,0x68,0xF3,0x23,0x03};
    return !memcmp(g_base + RVA_ADDMOVE, sig, sizeof sig) && !memcmp(g_base+0x132F040,character_ctor,sizeof character_ctor) && !memcmp(g_base+0x44E0F0,stop_move,sizeof stop_move);
}
static void walk_step(void *pc) {
    if (g_walk_left <= 0) return;
    void *pawn = *(void **)((uint8_t *)pc + OFF_CTRL_PAWN); g_walk_left--;
    if (pawn) {
        const float *input=(const float *)((uint8_t *)pawn+0x3e8);
        void *movement=*(void **)((uint8_t *)pawn+0x408);
        const float *velocity=movement ? (const float *)((uint8_t *)movement+0x104) : NULL;
        logf_("shots: walk frame remaining=%d input=%.2f,%.2f,%.2f velocity=%.1f,%.1f,%.1f",g_walk_left,input[0],input[1],input[2],velocity ? velocity[0] : 0,velocity ? velocity[1] : 0,velocity ? velocity[2] : 0);
        // Absolver's generic forced-input path reaches 10000 cm/s at scale 1; probe at walking speed.
        ((void (*)(void *, const float *, float, int))(g_base + RVA_ADDMOVE))(pawn, g_walk_dir, .04f, 1);
    }
}

static void shotrunner_stop(const char *why) {
    for (int i = 0; i < g_sr_n; i++) free(g_sr_lines[i]);
    free(g_sr_lines); g_sr_lines = NULL; g_sr_n = g_sr_pos = 0; g_sr_active = 0;
    logf_("shots: %s (%d shots taken)", why, g_sr_count);
}
static void shotrunner_tick(void) {
    static unsigned frame_poll;
    __try {
        if (!g_sr_active) {
            if ((++frame_poll % 20) != 0) return;
            wchar_t run[MAX_PATH]; swprintf(run, MAX_PATH, L"%sshots.run", g_dir);
            if (GetFileAttributesW(run) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileW(run);
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sshots.txt", g_dir);
            FILE *f = _wfopen(path, L"r"); if (!f) { logf_("shots: %ls not found", path); return; }
            char line[512]; int cap = 64; g_sr_lines = (char **)malloc(sizeof(char *) * cap); g_sr_n = 0;
            while (fgets(line, sizeof line, f)) {
                char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
                if (!line[0] || line[0] == '#') continue;
                if (g_sr_n == cap) { cap *= 2; g_sr_lines = (char **)realloc(g_sr_lines, sizeof(char *) * cap); }
                g_sr_lines[g_sr_n++] = _strdup(line);
            }
            fclose(f); g_sr_pos = 0; g_sr_wait = 0; g_sr_count = 0; g_sr_active = 1;
            logf_("shots: running %d commands from %ls", g_sr_n, path);
            return;
        }
        if (g_walk_left > 0) { void *pc0 = L_local_pc(); if (pc0) walk_step(pc0); }
        if (g_sr_wait > 0) { g_sr_wait--; return; }
        if (g_sr_pos >= g_sr_n) { shotrunner_stop("script ended without done"); return; }
        const char *cmd = g_sr_lines[g_sr_pos++]; void *pc = L_local_pc();
        if (!pc) { shotrunner_stop("no local player controller - stopped"); return; }
        if (!strncmp(cmd, "cam ", 4)) {
            float v[6] = { 0, 0, 0, 0, 0, 0 };
            if (sscanf(cmd + 4, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 5) { logf_("shots: bad cam line %s", cmd); return; }
            if (!freecam_apply(v)) { shotrunner_stop("camera could not be moved - stopped"); return; }
            g_sr_wait = SHOT_SETTLE;
        } else if (!strncmp(cmd, "shot ", 5)) {
            if (!shotrunner_exec(pc, "HighResShot 1")) { shotrunner_stop("console command failed - stopped"); return; }
            g_sr_count++; logf_("shots: shot %d %s", g_sr_count, cmd + 5); g_sr_wait = SHOT_AFTER;
        } else if (!strncmp(cmd, "trace ", 6)) {
            float v[6] = { 0, 0, 0, 0, 0, 0 }; int ch = 2; uint8_t hit[0x180]; float t = 0;
            if (sscanf(cmd + 6, "%f %f %f %f %f %f %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &ch) < 6) { logf_("shots: bad trace line %s", cmd); return; }
            if (!trace_verify()) { logf_("shots: LineTrace bytes differ from the expected build - traces disabled"); return; }
            int r = trace_line(v, v + 3, ch, hit, &t); logf_("shots: trace hit=%d t=%.4f from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f ch=%d", r, t, v[0], v[1], v[2], v[3], v[4], v[5], ch);
        } else if (!strncmp(cmd, "tracedown ", 10)) {
            float x = 0, y = 0, z0 = 0, z1 = 0; uint8_t hit[0x180]; float t = 0;
            if (sscanf(cmd + 10, "%f %f %f %f", &x, &y, &z0, &z1) < 4) { logf_("shots: bad tracedown line %s", cmd); return; }
            if (!trace_verify()) { logf_("shots: LineTrace bytes differ from the expected build - traces disabled"); return; }
            const float a_[3] = { x, y, z0 }, b_[3] = { x, y, z1 }; int r = trace_line(a_, b_, 2, hit, &t);
            logf_("shots: tracedown hit=%d t=%.4f (expected hit z = %.1f if t is the fraction)", r, t, z0 + (z1 - z0) * t);
            char buf[1100]; int o = 0; for (int i = 0; i < 40; i++) o += snprintf(buf + o, sizeof buf - o, "%d:%.2f ", i * 4, *(float *)(hit + i * 4));
            logf_("shots: hitbuffer floats %s", buf);
        } else if (!strcmp(cmd, "tracefile")) {
            if (!trace_verify()) { logf_("shots: LineTrace bytes differ from the expected build - traces disabled"); return; }
            trace_file();
        } else if (!strncmp(cmd, "tp ", 3)) {
            float v[3] = { 0, 0, 0 }; if (sscanf(cmd + 3, "%f %f %f", &v[0], &v[1], &v[2]) < 3) { logf_("shots: bad tp line %s", cmd); return; }
            if (!walk_verify()) { shotrunner_stop("movement functions differ from expected build"); return; }
            if (L_teleport_local(v)) {
                void *pawn=*(void **)((uint8_t *)pc+OFF_CTRL_PAWN);
                if (pawn) {
                    // Dev probes must not carry velocity between distant test positions.
                    ((uint8_t (*)(void *,const float *,int,void *,int))(g_base+RVA_SET_LOCATION))(pawn,v,0,NULL,1);
                    void *movement=*(void **)((uint8_t *)pawn+0x408);
                    if (movement) ((void (*)(void *))(g_base+0x44E0F0))(movement);
                }
            }
        } else if (!strcmp(cmd, "where")) {
            void *pawn = *(void **)((uint8_t *)pc + OFF_CTRL_PAWN), *root = pawn ? *(void **)((uint8_t *)pawn + OFF_ACTOR_ROOTCOMP) : NULL; float p3[3] = { 0, 0, 0 };
            if (root) memcpy(p3, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof p3);
            logf_("shots: where %.0f,%.0f,%.0f", p3[0], p3[1], p3[2]);
            // Offsets confirmed by this build's UCapsuleComponent constructor; diagnostics only.
            char cls_path[512];
            static const uint8_t capsule_ctor[]={0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B};
            if (root && !memcmp(g_base+0x136DA00,capsule_ctor,sizeof capsule_ctor) && api_object_path(*(void **)((uint8_t *)root+0x10),cls_path,sizeof cls_path) && strstr(cls_path,"CapsuleComponent"))
                logf_("shots: capsule unscaled halfheight=%.1f radius=%.1f",*(float *)((uint8_t *)root+0x6c0),*(float *)((uint8_t *)root+0x6c4));
        } else if (!strncmp(cmd, "sweep ", 6)) {
            float v[8] = { 0, 0, 0, 0, 0, 0, 34, 92 }; int ch = 2; uint8_t hit[0x180]; float t = 0;
            if (sscanf(cmd + 6, "%f %f %f %f %f %f %f %f %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &ch) < 6) { logf_("shots: bad sweep line %s", cmd); return; }
            if (!sweep_verify()) { logf_("shots: Sweep bytes differ from the expected build - sweeps disabled"); return; }
            int r = sweep_capsule(v, v + 3, v[6], v[7], ch, hit, &t); logf_("shots: sweep hit=%d t=%.4f from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f capsule r=%.0f hh=%.0f ch=%d", r, t, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], ch);
        } else if (!strcmp(cmd, "sweepfile")) {
            if (!sweep_verify()) { logf_("shots: Sweep bytes differ from the expected build - sweeps disabled"); return; }
            sweep_file();
        } else if (!strncmp(cmd, "walk ", 5)) {
            float dx = 0, dy = 0; int fr = 0;
            if (sscanf(cmd + 5, "%f %f %d", &dx, &dy, &fr) < 3) { logf_("shots: bad walk line %s", cmd); return; }
            if (!walk_verify()) { logf_("shots: AddMovementInput bytes differ from the expected build - walking disabled"); return; }
            float l = sqrtf(dx * dx + dy * dy); if (l < 1e-6f) l = 1.f; g_walk_dir[0] = dx / l; g_walk_dir[1] = dy / l; g_walk_dir[2] = 0; g_walk_left = fr; g_sr_wait = fr + 2;
        } else if (!strncmp(cmd, "exec ", 5)) { shotrunner_exec(pc, cmd + 5); logf_("shots: exec %s", cmd + 5); g_sr_wait = 2;
        } else if (!strncmp(cmd, "wait ", 5)) { g_sr_wait = atoi(cmd + 5);
        } else if (!strcmp(cmd, "done")) {
            void *pawn = *(void **)((uint8_t *)pc + OFF_CTRL_PAWN), *cam = api_weak_get(&g_cam);
            if (pawn) freecam_set_view(pc, pawn);
            if (cam) { api_destroy(cam); api_weak_set(&g_cam, NULL); }
            wchar_t d[MAX_PATH]; swprintf(d, MAX_PATH, L"%sshots.done", g_dir); FILE *f = _wfopen(d, L"w"); if (f) { fprintf(f, "%d\n", g_sr_count); fclose(f); }
            shotrunner_stop("done, the view is back on the character");
        } else logf_("shots: unknown command %s ignored", cmd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("shots: EXCEPTION 0x%08lX - runner stopped", GetExceptionCode());
        g_sr_active = 0;
    }
}
#else
static void shotrunner_tick(void) {}
#endif

// first line of ip.txt = host ip or ip:port, optional second line = extra URL options (e.g. ?CheckPoint=...?zone=3)
static int read_join_url(wchar_t *out, int cap) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%sip.txt", g_dir);
    FILE *f = _wfopen(path, L"r");
    if (!f) { logf_("join: %ls not found", path); return 0; }
    char l1[256] = "", l2[512] = "", raw[512];
    while (fgets(raw, sizeof raw, f)) {                                   // lines starting with # are comments (the installer writes a note there)
        const char *r = raw; while (*r == ' ' || *r == '\t') r++;
        if (*r == '#' || *r == '\r' || *r == '\n' || !*r) { if (*r == '#' || !l1[0]) continue; }
        if (!l1[0]) snprintf(l1, sizeof l1, "%s", raw); else { snprintf(l2, sizeof l2, "%s", raw); break; }
    }
    fclose(f);
    char *e;
    for (e = l1 + strlen(l1); e > l1 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
    for (e = l2 + strlen(l2); e > l2 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
    if (!l1[0]) { logf_("join: ip.txt has no address yet - put the host's address in it (or paste it from the F1 menu)"); return 0; }
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
    if (g_lm_mode && g_lm_me.present) {                                   // tell the host which gameplay mods this PC runs
        wchar_t suffix[LM_NAMES_MAX + 60];
        if (lm_join_suffix(&g_lm_me, suffix, sizeof suffix / sizeof suffix[0]) && wcslen(out) + wcslen(suffix) + 1 < (size_t)cap) { wcscat(out, suffix); logf_("join: added the mod check (%ls)", suffix); }
        else logf_("join: the mod check did not fit in the URL and was left out");
    }
    { wchar_t ks[60]; if (jg_key_for_url(ks, 60) && wcslen(out) + wcslen(ks) + 1 < (size_t)cap) wcscat(out, ks); }       // join protection: the host's key
    return 1;
}

// Settings: LanNative\config.txt ("key = value" lines, see config.h). When config.txt exists it is the only source (a missing key = that
// setting's default); when it does not, the one-file-per-setting files (parryfix.txt, pvpfix.txt, ...) are read.
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

// pvpfix (config.txt key, or LanNative\pvpfix.txt): "on" (default) = send-back guard + duel debug flow, "guard" = send-back guard only, "off" = neither.
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
// perteam (config.txt key, or LanNative\perteam.txt): players per team for 3v3 maps (default 1 = a 3v3 map starts with 2 players; 2 = starts with 4; 3 or "off" = the game's own 6).
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

// config "soloduel = on": a 1v1 map this PC hosts starts with only the host in it (map/spawn/collision testing without a second PC). Default off.
static int read_soloduel(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("soloduel", L"soloduel.txt", 0, value, sizeof value, path)) { logf_("solo duel: off (default; not set in %ls)", path); return 0; }
    if (_strnicmp(value, "on", 2) == 0) { logf_("solo duel: ON (%ls) - a hosted 1v1 / 3v3 map starts with only you in it", path); return 1; }
    logf_("solo duel: off (%ls)", path);
    return 0;
}

// config "lan = on": install the LAN networking hooks (joiner fixes, host tracking, body cleanup, travel log). Default OFF; the pvp guard /
// solo duel / verdict byte do not need them (SetClientTravel stays for the guard).
static int read_lan(void) {
    wchar_t path[MAX_PATH];
    char value[32] = "";
    if (!setting_get("lan", L"lan.txt", 0, value, sizeof value, path)) { logf_("lan: OFF (default; set 'lan = on' in %ls for LAN play) - LAN hooks not installed", path); return 0; }
    if (_strnicmp(value, "on", 2) == 0) { logf_("lan: ON (%ls) - installing the LAN hooks", path); return 1; }
    logf_("lan: off (%ls) - LAN hooks not installed", path);
    return 0;
}

// autoleave (config.txt key, or LanNative\autoleave.txt): "on" (default) or "off". In a duel map, when the other player's connection is gone, travel to MAP_URL (as menu Host does).
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
    { char hx[40]; patch_hex(code, PARRYFIX_SIZE, hx, sizeof hx); logf_("parry fix: applied at base+0x%X (FC -> F4), memory now: %s", RVA_PARRY_FIX, hx); }
}

// ---- generic runtime patch table (patches.h). config "patches = on" (default OFF) + LanNative\patches.txt. Memory only, never the exe file.
static int se_sym_lookup(const char *name, uint32_t *rva, uint32_t *stolen, uint8_t *bytes);   // secore.inc (included below)
static uint32_t patch_target_rva(PatchRow *r) {
    if (!r->sym[0]) return r->off;
    uint32_t rva = 0, st; uint8_t b[24];
    if (se_sym_lookup(r->sym, &rva, &st, b)) return rva + r->off;
    return 0;
}
// op: 1 apply, 0 revert. Returns 1 on success (apply: applied or already in place).
static int patch_write(PatchRow *r, int op) {
    uint8_t *code = g_base + r->rva; DWORD old; char hx[3 * PATCH_MAX + 4];
    if (!VirtualProtect(code, (SIZE_T)r->n, PAGE_EXECUTE_READWRITE, &old)) { logf_("patch %s: VirtualProtect failed at base+0x%X - skipped", r->name, r->rva); return 0; }
    int res;
    if (op) { res = patch_apply_bytes(code, r); if (res == PATCH_MISMATCH) { patch_hex(code, r->n, hx, sizeof hx); logf_("patch %s: bytes at base+0x%X are %s, not the expected ones - REFUSED, nothing written", r->name, r->rva, hx); } }
    else { res = patch_revert_bytes(code, r); if (!res) logf_("patch %s: revert refused, memory at base+0x%X does not hold the new bytes", r->name, r->rva); }
    VirtualProtect(code, (SIZE_T)r->n, old, &old);
    if (res) FlushInstructionCache(GetCurrentProcess(), code, (SIZE_T)r->n);
    return res != 0;
}
static PatchRow g_patch_rows[PATCH_ROWS];
static int g_patch_n;
static void apply_patch_table(void) {
    char v[16] = ""; wchar_t path[MAX_PATH];
    if (!setting_get("patches", L"patches.flag", 0, v, sizeof v, path) || _strnicmp(v, "on", 2) != 0) { logf_("patches: OFF (default; set 'patches = on' in config.txt to apply LanNative\\patches.txt)"); return; }
    static char text[16384]; wchar_t pf[MAX_PATH]; swprintf(pf, MAX_PATH, L"%spatches.txt", g_dir);
    if (!read_text_file(pf, text, sizeof text)) { logf_("patches: ON but %ls is missing - nothing to apply", pf); return; }
    int bad = 0; char e1[128];
    g_patch_n = patch_parse_file(text, g_patch_rows, PATCH_ROWS, &bad, e1, sizeof e1);
    logf_("patches: ON, %d row(s) from %ls, %d bad line(s)%s%s", g_patch_n, pf, bad, bad ? ": " : "", bad ? e1 : "");
    int selftest = (setting_get("patches_selftest", L"patches_selftest.flag", 0, v, sizeof v, path) && _strnicmp(v, "on", 2) == 0);
    for (int i = 0; i < g_patch_n; i++) {
        PatchRow *r = &g_patch_rows[i]; char hx[3 * PATCH_MAX + 4];
        if (!r->enabled) { logf_("patch %s: off (row disabled)", r->name); continue; }
        r->rva = patch_target_rva(r);
        if (!r->rva || r->rva + (uint32_t)r->n > g_exe_size) { logf_("patch %s: target '%s' (+0x%X) does not resolve inside the exe - skipped", r->name, r->sym[0] ? r->sym : "rva", r->off); continue; }
        int clash = 0; for (int j = 0; j < i; j++) if (g_patch_rows[j].state && patch_overlap(&g_patch_rows[j], r)) clash = 1;
        if (clash) { logf_("patch %s: overlaps an applied patch - skipped", r->name); continue; }
        if (!patch_write(r, 1)) continue;
        r->state = 1;
        patch_hex(g_base + r->rva, r->n, hx, sizeof hx);
        logf_("patch %s: applied at base+0x%X (%d byte(s)), memory now: %s", r->name, r->rva, r->n, hx);
        if (r->rva == RVA_PARRY_FIX) g_parry_fix_applied = 1;
        if (selftest) {      // reversibility proof: revert, read back, re-apply, read back
            int ok = patch_write(r, 0); patch_hex(g_base + r->rva, r->n, hx, sizeof hx);
            logf_("patch %s: selftest revert %s, memory: %s", r->name, ok ? "ok" : "FAILED", hx);
            ok = patch_write(r, 1); patch_hex(g_base + r->rva, r->n, hx, sizeof hx);
            logf_("patch %s: selftest re-apply %s, memory: %s", r->name, ok ? "ok" : "FAILED", hx);
        }
    }
}
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

// Host address. The game's socket code reads "-MULTIHOME=<ip>" from the command line buffer when a listen server picks the address it binds
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

// The DLL is loaded before the engine parses the command line (FCommandLine::bIsInitialized is still 0 then), so the
// original command line is saved from the first game ticks instead, once the flag is set. Quiet until then; tries every tick for 3000 ticks at most.
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
    if (listen) jg_announce_host();
    if (listen) logf_("host address: this listen server binds %s%s", strcmp(g_cmd_applied, "auto") ? g_cmd_applied : "every adapter (auto)", strcmp(g_cmd_applied, "auto") ? " only (-MULTIHOME)" : "");
}

// In-game menu (state machine and layout: menu.h; this gathers key edges, runs the chosen action and draws the text)
static int g_menu_on = 1, g_menu_faults, g_draw_faults, g_perteam_cfg = 1, g_autoleave_on = 1, g_solo_cfg;
static Menu g_menu;
static char g_maps[MENU_LIST_MAX][MENU_URL_MAX], g_altar_label[MENU_LIST_MAX][MENU_LINE_MAX];
static float g_altar_pos[MENU_LIST_MAX][3];
static int g_maps_n, g_altars_n;
static char g_maps_new[MENU_LIST_MAX];            // 1 = found in a pak the game has not mounted yet (needs a restart)
#include "mapscan.h"
typedef struct { char paths[2048][MENU_URL_MAX], urls[MENU_LIST_MAX][MENU_URL_MAX]; int count, shown, paks, skipped, dropped, mounted_ok, mounted_count; char urlnew[MENU_LIST_MAX]; } MapDiscovery;
static MapDiscovery *g_discovered;
static volatile LONG g_discovery_state;
static void map_discovered(const char *path, void *context) {
    MapDiscovery *d = context;
    if (strlen(path)+8 >= MENU_URL_MAX) { d->dropped++; return; }
    for (int i = 0; i < d->count; i++) if (!_stricmp(d->paths[i], path)) return;
    if (d->count == 2048) { d->dropped++; return; }
    strcpy(d->paths[d->count++], path);
}
static void discover_directory(const wchar_t *dir, const char *relative, int depth, MapDiscovery *d) {
    wchar_t pattern[MAX_PATH];
    if (depth > 12 || swprintf(pattern, MAX_PATH, L"%s\\*", dir) < 0) { d->dropped++; return; }
    WIN32_FIND_DATAW item; HANDLE search = FindFirstFileW(pattern, &item); if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (item.cFileName[0] == '.' || (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t file[MAX_PATH]; char name[512], rel[1024];
        if (swprintf(file, MAX_PATH, L"%s\\%s", dir, item.cFileName) < 0 || !WideCharToMultiByte(CP_UTF8, 0, item.cFileName, -1, name, sizeof name, NULL, NULL) || snprintf(rel, sizeof rel, "%s%s", relative, name) >= sizeof rel) { d->dropped++; continue; }
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            size_t n = strlen(rel); if (n+2 >= sizeof rel) { d->dropped++; continue; }
            rel[n] = '/'; rel[n+1] = 0; discover_directory(file, rel, depth+1, d);
        } else {
            const char *ext = strrchr(name, '.'); if (!ext) continue;
            if (!_stricmp(ext, ".pak")) {
                FILE *f = _wfopen(file, L"rb"); int n = f ? mapscan_pak(f, map_discovered, d) : -1; if (f) fclose(f);
                d->paks++; if (n < 0) d->skipped++;
                logf_("discovery: package %ls (%s, %d maps)", file, n < 0 ? "unsupported/unreadable index" : "indexed", n < 0 ? 0 : n);
            } else if (!_stricmp(ext, ".umap")) {
                char path[MENU_URL_MAX]; if (mapscan_path("../../../Absolver/Content/", rel, path, sizeof path)) map_discovered(path, d);
            }
        }
    } while (FindNextFileW(search, &item));
    FindClose(search);
}
static int discover_mounted_maps(MapDiscovery *d) {
    // Stock indexes are encrypted; ask the already-mounted engine filesystem, never embed a key.
    static int disabled;
    const unsigned char get_bytes[] = {0x48,0x8d,0x05,0x89,0xf5,0xfe,0x02,0xc3};
    const unsigned char find_bytes[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18};
    const unsigned char files_bytes[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10};
    const unsigned char free_bytes[] = {0x48,0x85,0xc9,0x74,0x1d};
    if (disabled) return 0;
    if (memcmp(g_base+0x67CEE0,get_bytes,sizeof get_bytes) || memcmp(g_base+0x67BAF0,find_bytes,sizeof find_bytes) || memcmp(g_base+0x193ABC0,files_bytes,sizeof files_bytes) || memcmp(g_base+RVA_FMEMORY_FREE,free_bytes,sizeof free_bytes)) {
        disabled=1; logf_("discovery: mounted filesystem byte guard failed; native enumeration disabled"); return 0;
    }
    typedef struct { FStringRaw *data; int num, max; } StringArray;
    StringArray files={0}; int ok=0;
    __try {
        void *manager=((void *(*)(void))(g_base+0x67CEE0))();
        void *pak=((void *(*)(void *,const wchar_t *))(g_base+0x67BAF0))(manager,L"PakFile");
        if (pak) {
            ((void (*)(void *,StringArray *,const wchar_t *,const wchar_t *))(g_base+0x193ABC0))(pak,&files,L"../../../Absolver/Content/",L".umap");
            if (files.num>=0 && files.num<=100000 && files.max>=files.num && (!files.num || files.data)) {
                for (int i=0; i<files.num; i++) {
                    FStringRaw *s=&files.data[i]; char entry[1024],path[MENU_URL_MAX];
                    if (s->data && s->num>1 && s->num<=1024 && s->max>=s->num && s->data[s->num-1]==0 && WideCharToMultiByte(CP_UTF8,0,s->data,-1,entry,sizeof entry,NULL,NULL) && mapscan_path("",entry,path,sizeof path)) map_discovered(path,d);
                }
                ok=1;
                for (int i=0; i<files.num; i++) if (files.data[i].data) ((void (*)(void *))(g_base+RVA_FMEMORY_FREE))(files.data[i].data);
                if (files.data) ((void (*)(void *))(g_base+RVA_FMEMORY_FREE))(files.data);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { disabled=1; logf_("discovery: mounted filesystem exception 0x%08lX; disabled for this run",GetExceptionCode()); }
    logf_("discovery: mounted filesystem %s (%d filenames)",ok ? "enumerated" : "unavailable",files.num);
    return ok;
}
static DWORD WINAPI discover_maps_worker(void *unused) {
    (void)unused;
    MapDiscovery *d = calloc(1, sizeof *d);
    if (d) {
        d->mounted_ok = discover_mounted_maps(d); d->mounted_count = d->count;       // the first mounted_count paths are the maps the running game already has
        wchar_t exe[MAX_PATH], content[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *slash = wcsrchr(exe, '\\'); if (slash) *slash = 0;
        if (slash && swprintf(content, MAX_PATH, L"%s\\..\\..\\Content", exe) >= 0) discover_directory(content, "", 0, d);
        // Custom URL options survive only when the referenced map is actually installed.
        wchar_t config[MAX_PATH]; swprintf(config, MAX_PATH, L"%smaps.txt", g_dir);
        FILE *f = _wfopen(config, L"r"); char line[1024], url[MENU_URL_MAX];
        while (f && fgets(line, sizeof line, f)) if (menu_parse_open_line(line, url, sizeof url)) {
            char path[MENU_URL_MAX]; strcpy(path, url); char *options = strchr(path, '?'); if (options) *options = 0;
            for (int i = 0; i < d->count && d->shown < MENU_LIST_MAX; i++) if (!_stricmp(path, d->paths[i])) {
                int duplicate = 0; for (int j = 0; j < d->shown; j++) { size_t n = strcspn(d->urls[j], "?"); if (strlen(path) == n && !_strnicmp(path, d->urls[j], n)) duplicate = 1; }
                if (!duplicate) strcpy(d->urls[d->shown++], url);
                break;
            }
        }
        if (f) fclose(f);
        for (int i = 0; i < d->count; i++) if (mapscan_root(d->paths[i])) {
            int duplicate = 0; for (int j = 0; j < d->shown; j++) { size_t n = strcspn(d->urls[j], "?"); if (strlen(d->paths[i]) == n && !_strnicmp(d->paths[i], d->urls[j], n)) duplicate = 1; }
            if (!duplicate) { if (d->shown < MENU_LIST_MAX) snprintf(d->urls[d->shown++], MENU_URL_MAX, "%s?listen", d->paths[i]); else d->dropped++; }
        }
        qsort(d->urls, d->shown, sizeof d->urls[0], map_menu_compare);   // by group, then label (mapscan.h)
        for (int i = 0; i < d->shown; i++) {                                   // a map that is on disk but not mounted: its pak was added after the game started
            char base[MENU_URL_MAX]; snprintf(base, sizeof base, "%.*s", (int)strcspn(d->urls[i], "?"), d->urls[i]);
            int idx = -1; for (int k = 0; k < d->count; k++) if (!_stricmp(d->paths[k], base)) { idx = k; break; }
            d->urlnew[i] = d->mounted_ok && idx >= d->mounted_count;
        }
        logf_("discovery: %d packages, %d skipped indexes, %d installed maps, %d travel entries, %d capacity/path drops", d->paks, d->skipped, d->count, d->shown, d->dropped);
    }
    g_discovered = d; InterlockedExchange(&g_discovery_state, 2); return 0;
}
static void request_map_discovery(void) {
    if (InterlockedCompareExchange(&g_discovery_state, 1, 0) != 0) return;
    HANDLE thread = CreateThread(NULL, 0, discover_maps_worker, NULL, 0, NULL);
    if (thread) CloseHandle(thread); else { InterlockedExchange(&g_discovery_state, 0); logf_("discovery: could not start worker (%lu)", GetLastError()); }
}
#define ALTAR_LIFT 150.f   // altar position + this height, so the character lands on the ground instead of inside it
static char g_menu_lines[MENU_LINES_MAX][MENU_LINE_MAX];
static int g_menu_nlines, g_menu_sel = -1;
static uint64_t g_armed6, g_armed7, g_armed_menu;

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

// NPCs: npcs.txt, one entry per line, TAB separated: group, label, archetype asset path[, character class path] (menu.h has the parser). Entries are
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

// Mods\*\content entries of the kinds npcs / bots (content.inc; defined after the includes at the end of this file): entry i -> its id and label;
// returns -1 past the last entry, 0 = skip (not built), 1 = an npcs entry, 2 = a bots entry. Spawned by id (npc_spawn_entry) so their reactions / brain bind.
static int npc_mod_entry(int i, char *id, int idcap, char *label, int lcap); static void npc_spawn_entry(const char *id);
#define NPC_ENTRY_MARK '@'                                                          // g_npc_path of a content entry: "@<id>" (stock entries start with /Game/)
static const char *npc_sort_label(int i) { return g_npc_label[i]; }
static void npc_sort_swap(int a, int b) {
    char l[NPC_LABEL_MAX], p[NPC_PATH_MAX], c[NPC_CLASS_MAX];
    memcpy(l, g_npc_label[a], sizeof l); memcpy(g_npc_label[a], g_npc_label[b], sizeof l); memcpy(g_npc_label[b], l, sizeof l);
    memcpy(p, g_npc_path[a], sizeof p); memcpy(g_npc_path[a], g_npc_path[b], sizeof p); memcpy(g_npc_path[b], p, sizeof p);
    memcpy(c, g_npc_class[a], sizeof c); memcpy(g_npc_class[a], g_npc_class[b], sizeof c); memcpy(g_npc_class[b], c, sizeof c);
}
// appends the content npcs / bots entries as the groups "Mod NPCs" and "Bots" (after the file's groups), then sorts every group by label
static void npc_finish_list(void) {
    char id[NPC_PATH_MAX - 1], lab[NPC_LABEL_MAX]; int added[3] = { 0 }, dropped = 0;
    for (int kind = 1; kind <= 2; kind++) {
        int g = -1;
        for (int i = 0, k; (k = npc_mod_entry(i, id, sizeof id, lab, sizeof lab)) >= 0; i++) {
            if (k != kind) continue;
            if (g < 0) g = npc_group_index(kind == 1 ? "Mod NPCs" : "Bots");
            if (g < 0 || g != g_npc_ngroups - 1 || g_npc_group_count[g] >= MENU_LIST_MAX || g_npcs_n >= NPC_MAX) { dropped++; continue; }   // only a new last group keeps the ranges contiguous
            int at = g_npcs_n++; g_npc_group_count[g]++; added[kind]++;
            snprintf(g_npc_label[at], NPC_LABEL_MAX, "%s", lab); snprintf(g_npc_path[at], NPC_PATH_MAX, "%c%s", NPC_ENTRY_MARK, id); g_npc_class[at][0] = 0;
        }
    }
    for (int g = 0, start = 0; g < g_npc_ngroups; start += g_npc_group_count[g], g++) menu_sort_range(start, g_npc_group_count[g], npc_sort_label, npc_sort_swap);
    logf_("menu: NPC list: %d entries in %d groups (%d content npcs, %d bots entries added%s), every group sorted by label", g_npcs_n, g_npc_ngroups, added[1], added[2],
          dropped ? "; some content entries did not fit (a group named Mod NPCs / Bots in npcs.txt, 16 groups or 512 entries)" : "");
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
        logf_("menu: %ls not found - the NPC list has only the built-in Trickster (plus the Mods content entries)", path);
        npc_finish_list();
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
    logf_("menu: %d NPC types in %d groups from %ls%s", g_npcs_n, g_npc_ngroups, path, dropped ? " (some entries were dropped: too many groups, more than 192 in a group, or more than 512 in total)" : "");
    npc_finish_list();
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
    logf_("npc: menu spawn '%s' (%s%s%s)", g_npc_label[index], g_npc_path[index], g_npc_class[index][0] ? ", class " : "", g_npc_class[index]);
    __try { if (g_npc_path[index][0] == NPC_ENTRY_MARK) npc_spawn_entry(g_npc_path[index] + 1); else L_npc_spawn(g_npc_path[index], g_npc_label[index], g_npc_class[index]); }
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

static int modsui_count(void); static const char *modsui_line(int i); static void modsui_action(int index); static void modsui_tick(int focus);   // modsui.inc
static int modsui_back(int sel); static int modsui_open_section(void); static void modsui_collapse(void);
static const char *menu_list_text(int page, int i) {
    static char label[MENU_LINE_MAX];
    if (page == MP_MODS) return modsui_line(i);
    if (page == MP_ALTARS) return g_altar_label[i];
    if (page == MP_NPCGROUPS) { snprintf(label, sizeof label, "%s (%d)", g_npc_group_name[i], g_npc_group_count[i]); return label; }
    if (page == MP_NPCS) return g_npc_label[(g_menu.ngroups > 0 ? g_menu.gstart[g_menu.group] : 0) + i];
    char short_[MENU_LINE_MAX]; map_menu_label(g_maps[i], short_, sizeof short_);
    snprintf(label, sizeof label, "%s%s", short_, g_maps_new[i] ? "   (new: restart the game first)" : "");
    return label;
}

// ---- join settings from the menu: the address and the key come from the clipboard (nothing to type in the game)
static char g_joinaddr_text[64];
static int clipboard_text(char *out, size_t cap) {                          // first line of the clipboard text, trimmed, ASCII only
    out[0] = 0; if (!OpenClipboard(NULL)) return 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT); const wchar_t *w = h ? GlobalLock(h) : NULL; size_t n = 0;
    if (w) { while (*w == L' ' || *w == L'\t' || *w == L'\r' || *w == L'\n') w++; for (; *w && *w != L'\r' && *w != L'\n' && n + 1 < cap; w++) out[n++] = *w < 128 ? (char)*w : '?'; GlobalUnlock(h); }
    CloseClipboard(); while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--; out[n] = 0; return n > 0;
}
static void joinaddr_refresh(void) {                                        // the address on the first usable line of ip.txt, for the menu label
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sip.txt", g_dir); g_joinaddr_text[0] = 0;
    FILE *f = _wfopen(path, L"r"); if (!f) return; char raw[256];
    while (fgets(raw, sizeof raw, f)) { const char *r = raw; while (*r == ' ' || *r == '\t') r++; if (*r == '#' || *r == '\r' || *r == '\n' || !*r) continue; snprintf(g_joinaddr_text, sizeof g_joinaddr_text, "%s", r); break; }
    fclose(f); size_t n = strlen(g_joinaddr_text); while (n && (g_joinaddr_text[n - 1] == '\r' || g_joinaddr_text[n - 1] == '\n' || g_joinaddr_text[n - 1] == ' ')) g_joinaddr_text[--n] = 0;
}
static int joinaddr_ok(const char *s) {                                      // host, IPv4 or name, optional :port; nothing that could break a URL
    size_t n = strlen(s); if (n < 3 || n > 100) return 0; int colons = 0;
    for (size_t i = 0; i < n; i++) { char c = s[i]; if (c == ':') { if (++colons > 1) return 0; } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-')) return 0; }
    return 1;
}
static void menu_set_ip(const char *t) {                                    // writes the address to ip.txt (line 1), keeping the options on line 2
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sip.txt", g_dir);
    char old[1024] = "", second[512] = ""; read_text_file(path, old, sizeof old);
    { int seen = 0; for (const char *p = old; *p;) { const char *e = strpbrk(p, "\r\n"); size_t n = e ? (size_t)(e - p) : strlen(p); const char *q = p; while (q < p + n && (*q == ' ' || *q == '\t')) q++;
        if (q < p + n && *q != '#') { if (seen == 1) { snprintf(second, sizeof second, "%.*s", (int)n, p); break; } seen++; } p += n; while (*p == '\r' || *p == '\n') p++; } }
    FILE *f = _wfopen(path, L"wb"); if (!f) { logf_("menu: cannot write ip.txt"); return; }
    fprintf(f, "%s\r\n%s%s", t, second, second[0] ? "\r\n" : ""); fclose(f); joinaddr_refresh();
    logf_("menu: join address set to %s (ip.txt)", t); char l[1][LM_LINE_MAX]; snprintf(l[0], LM_LINE_MAX, "Join address set: %s", t); toast_show(l, 1, 6);
}
// The "type the join address" box: Enter on the Join address entry opens it; characters come from key edges (textinput.h), Ctrl+V pastes, Enter saves, F1 cancels.
static TextInput g_ti; static int g_ti_active, g_ti_prev[TI_KEY_COUNT];
static void ti_prime(void) { for (int i = 0; i < TI_KEY_COUNT; i++) g_ti_prev[i] = (GetAsyncKeyState(ti_keys[i]) & 0x8000) != 0; }
// Returns 1 while the box is open (the menu must not handle keys then). en / bs / f1 are this frame's edges of Enter, Backspace and F1.
static int ti_step(int en, int bs, int f1) {
    if (!g_ti_active) return 0;
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    for (int i = 0; i < TI_KEY_COUNT; i++) {
        if (!key_edge(ti_keys[i], &g_ti_prev[i])) continue;
        if (ctrl) { if (ti_keys[i] == 0x56) { char t[128]; if (clipboard_text(t, sizeof t)) ti_set(&g_ti, t); } }
        else { char c = ti_vk_char(ti_keys[i], shift); if (c) ti_add(&g_ti, c); }
    }
    if (bs) ti_backspace(&g_ti);
    if (f1) { g_ti_active = 0; logf_("menu: join address box cancelled"); return 1; }
    if (en) {
        if (!g_ti.len) g_ti_active = 0;
        else if (joinaddr_ok(g_ti.buf)) { menu_set_ip(g_ti.buf); g_ti_active = 0; }
        else { char l[1][LM_LINE_MAX]; snprintf(l[0], LM_LINE_MAX, "Not a valid address (example 192.168.1.20 or 26.1.2.3:7777)"); toast_show(l, 1, 5); }
    }
    return 1;
}
static int ti_layout(char (*lines)[MENU_LINE_MAX], int *sel_line) {
    snprintf(lines[0], MENU_LINE_MAX, "Join address");
    snprintf(lines[1], MENU_LINE_MAX, "> %s_", g_ti.buf);
    snprintf(lines[2], MENU_LINE_MAX, "Now: %.60s", g_joinaddr_text[0] ? g_joinaddr_text : "not set");
    snprintf(lines[3], MENU_LINE_MAX, "Type the host address (192.168.1.20 or 26.1.2.3:7777)");
    snprintf(lines[4], MENU_LINE_MAX, "Enter = save   Ctrl+V = paste   Backspace = delete   F1 = cancel");
    *sel_line = 1; return 5;
}
static void menu_do(MenuAct a, uint64_t now) {
    switch (a.action) {
    case MA_MODS_ACT: modsui_action(a.index); break;
    case MA_MODS_BACK: g_menu.sel = modsui_back(a.index); break;
    case MA_EDIT_IP: ti_clear(&g_ti); ti_prime(); g_ti_active = 1; logf_("menu: join address box opened"); break;
    case MA_HOST: if (L_guard_leave(&g_armed6, now, "menu Host")) do_travel(MAP_URL); else g_menu.open = 1; break;
    // quit without a function key; the same travel the game's own Quit ends in (UGameplayStatics::OpenLevel StartMap)
    case MA_QUIT: if (L_guard_leave(&g_armed_menu, now, "menu Quit")) { g_menu.open = 0; logf_("menu: quit to the main menu"); do_travel(L"/Game/Maps/StartMap/StartMap"); } break;
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
    case MA_SOLODUEL: g_solo_cfg = !g_solo_cfg; L_set_soloduel(g_solo_cfg); logf_("menu: solo duel is now %s (1v1 and 3v3 maps you host, also the one you are in)", g_solo_cfg ? "ON - the duel starts with only you in it" : "off"); break;
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

// Watches the game's Content folder for pak files that were added, removed or changed while the game runs (checked every 2 s while the menu is open);
// a change starts a new map scan, so a pak dropped in shows up in the Map list without reopening anything.
static uint64_t pak_sig_dir(const wchar_t *dir, int depth, uint64_t h) {
    wchar_t pattern[MAX_PATH]; if (depth > 12 || swprintf(pattern, MAX_PATH, L"%s\\*", dir) < 0) return h;
    WIN32_FIND_DATAW it; HANDLE f = FindFirstFileW(pattern, &it); if (f == INVALID_HANDLE_VALUE) return h;
    do {
        if (it.cFileName[0] == '.' || (it.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t sub[MAX_PATH];
        if (it.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (swprintf(sub, MAX_PATH, L"%s\\%s", dir, it.cFileName) >= 0) h = pak_sig_dir(sub, depth + 1, h); continue; }
        size_t n = wcslen(it.cFileName); if (n < 5 || _wcsicmp(it.cFileName + n - 4, L".pak") != 0) continue;
        uint64_t v[3] = { 0, ((uint64_t)it.nFileSizeHigh << 32) | it.nFileSizeLow, ((uint64_t)it.ftLastWriteTime.dwHighDateTime << 32) | it.ftLastWriteTime.dwLowDateTime };
        for (const wchar_t *c = it.cFileName; *c; c++) v[0] = v[0] * 131 + (uint64_t)towlower(*c);
        h ^= v[0] * 0x9E3779B97F4A7C15ull; h *= 0x100000001B3ull; h ^= v[1]; h *= 0x100000001B3ull; h ^= v[2]; h *= 0x100000001B3ull;
    } while (FindNextFileW(f, &it));
    FindClose(f); return h;
}
static uint64_t paks_signature(void) {
    wchar_t exe[MAX_PATH], content[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, '\\'); if (!slash) return 1; *slash = 0;
    if (swprintf(content, MAX_PATH, L"%s\\..\\..\\Content", exe) < 0) return 1;
    return pak_sig_dir(content, 0, 0xCBF29CE484222325ull) | 1;
}
static void paks_watch(uint64_t now) {
    static uint64_t last_at, sig;
    if (now - last_at < 2000) return;
    last_at = now; uint64_t n = paks_signature();
    if (sig && n != sig) {
        if (InterlockedCompareExchange(&g_discovery_state, 0, 0) != 0) return;                      // a scan is running: look again in 2 s
        logf_("discovery: the pak files in Content changed - scanning for maps again"); request_map_discovery();
    }
    sig = n;
}

#ifdef LN_OVERLAY
// ---- Dear ImGui rendering of the menus (overlay.cpp). The state machine above stays the single source of truth: the overlay shows a snapshot of it and
// hands back clicks, which are executed as the same keys. The text menu is drawn whenever the overlay is not usable (menu_style = text, start failure, faults).
static int g_ov_on, g_ov_dead_logged; static OvSnap g_ov_snap;
static void modsui_snapshot(OvSnap *o);                                                  // modsui.inc
static int read_menu_style(void) {
    wchar_t path[MAX_PATH]; char value[32] = "";
    if (!setting_get("menu_style", L"menu_style.txt", 0, value, sizeof value, path)) { logf_("menu: style imgui (default; 'menu_style = text' in %ls selects the plain text menu)", path); return 1; }
    int on = _strnicmp(value, "text", 4) != 0;
    logf_("menu: style %s (%ls)", on ? "imgui" : "text", path);
    return on;
}
static int ov_text_hidden(void) { return g_ov_on && ov_shows_menu(); }
static void ov_publish_menu(void) {
    if (!g_ov_on) return;
    if (!g_menu.open || !g_menu_nlines) { ov_publish(NULL); return; }
    if (ov_state() == OV_OFF) { ov_set_log(logf_); ov_request(); }
    if (ov_state() == OV_DEAD) { if (!g_ov_dead_logged) { g_ov_dead_logged = 1; logf_("menu: the overlay is not available - the text menu is used"); } ov_publish(NULL); return; }
    OvSnap *o = &g_ov_snap; memset(o, 0, sizeof *o);
    if (g_ti_active) {
        o->kind = OV_LINES; o->count = g_menu_nlines < OV_ROWS ? g_menu_nlines : OV_ROWS;
        for (int i = 0; i < o->count; i++) snprintf(o->label[i], OV_LINE, "%s", g_menu_lines[i]);
    } else if (g_menu.page == MP_MODS) {
        modsui_snapshot(o); o->sel = g_menu.sel < o->nactions ? g_menu.sel : -1;
    } else {
        o->kind = OV_MENU; o->sel = g_menu.sel; o->has_back = g_menu.page != MP_MAIN;
        snprintf(o->title, OV_LINE, "%s", g_menu_lines[0]);
        if (g_menu.page == MP_MAIN) { o->count = g_menu_nlines - 1 < OV_ROWS ? g_menu_nlines - 1 : OV_ROWS; for (int i = 0; i < o->count; i++) snprintf(o->label[i], OV_LINE, "%s", g_menu_lines[i + 1]); }
        else { o->count = g_menu.count[g_menu.page] < OV_ROWS ? g_menu.count[g_menu.page] : OV_ROWS; for (int i = 0; i < o->count; i++) snprintf(o->label[i], OV_LINE, "%s", menu_list_text(g_menu.page, i)); }
    }
    ov_publish(o);
}
#else
#define ov_text_hidden() 0
#endif


static void menu_tick(int focus, uint64_t now) {
    static int kf1, kf2, kup, kdn, ken, krt, klf, kbs, loaded;
    if (!g_menu_on || g_menu_faults >= 2) { g_menu.open = 0; g_menu_nlines = 0;
#ifdef LN_OVERLAY
        if (g_ov_on) ov_publish(NULL);
#endif
        return; }
    if (!loaded) {
        loaded = 1;
        request_map_discovery();
        menu_load_altars(L"altars.tsv");
        npc_load_list(L"npcs.txt");
        joinaddr_refresh();
        menu_init_n(&g_menu, g_maps_n, g_altars_n, g_npcs_n);
        menu_set_npc_groups(&g_menu, g_npc_ngroups, g_npc_group_count);
    }
    if (!focus) { kf1 = kf2 = kup = kdn = ken = krt = klf = kbs = 0; return; }
    int f2 = key_edge(VK_F2, &kf2);
    int f1 = key_edge(VK_F1, &kf1), up = key_edge(VK_UP, &kup), dn = key_edge(VK_DOWN, &kdn), en = key_edge(VK_RETURN, &ken);
    int rt = key_edge(VK_RIGHT, &krt), lf = key_edge(VK_LEFT, &klf), bs = key_edge(VK_BACK, &kbs);
    if (InterlockedCompareExchange(&g_discovery_state, 2, 2) == 2 && (!g_menu.open || g_menu.page == MP_MAIN || g_menu.page == MP_MAPS)) {
        if (g_discovered) { g_maps_n = g_discovered->shown; memcpy(g_maps, g_discovered->urls, sizeof g_maps); memcpy(g_maps_new, g_discovered->urlnew, sizeof g_maps_new); free(g_discovered); g_discovered = NULL; }
        g_menu.count[MP_MAPS] = g_maps_n; if (g_menu.page == MP_MAPS && g_menu.sel >= g_maps_n) g_menu.sel = g_maps_n ? g_maps_n - 1 : 0; InterlockedExchange(&g_discovery_state, 0);
    }
    if (f1 && !g_menu.open) {
        joinaddr_refresh();
        request_map_discovery(); menu_load_altars(L"altars.tsv"); npc_load_list(L"npcs.txt");
        g_menu.count[MP_ALTARS] = g_altars_n; g_menu.count[MP_NPCS] = menu_clamp_list(g_npcs_n);
        menu_set_npc_groups(&g_menu, g_npc_ngroups, g_npc_group_count);
    }
    if (g_ti_active && !g_menu.open) g_ti_active = 0;
    if (g_menu.open && !g_ti_active) paks_watch(now);
    int in_box = ti_step(en, bs, f1);
    int key = in_box ? 0 : f1 ? MK_TOGGLE : f2 ? MK_MODS : !g_menu.open ? 0 : up ? MK_UP : dn ? MK_DOWN : (en || rt) ? MK_SELECT : (lf || bs) ? MK_BACK : 0;
#ifdef LN_OVERLAY
    { OvCmd oc;                                                                   // a click in the overlay runs as the key that does the same
        if (g_ov_on && ov_poll(&oc) && g_menu.open && !g_ti_active) {
            if (oc.cmd == OVC_TOGGLE) key = MK_TOGGLE;
            else if (oc.cmd == OVC_MENU_BACK) key = MK_BACK;
            else if ((oc.cmd == OVC_MENU_SELECT || oc.cmd == OVC_MODS_ACT) && oc.index >= 0 && oc.index < g_menu.count[g_menu.page]) { g_menu.sel = oc.index; key = MK_SELECT; }
            if (key) logf_("menu: overlay command %d (row %d)", oc.cmd, oc.index);
        }
    }
#endif
    __try {
        if (key) {
            int was = g_menu.open;
            MenuAct a = menu_key(&g_menu, key);
            if (g_menu.open != was) logf_("menu: %s", g_menu.open ? "opened" : "closed");
            if (a.action != MA_NONE) { logf_("menu: action %d%s", a.action, a.action == MA_TRAVEL ? " (travel)" : a.action == MA_TELEPORT ? " (teleport)" : a.action == MA_SPAWN_NPC ? " (spawn npc)" : a.action == MA_REMOVE_NPCS ? " (remove npcs)" : ""); menu_do(a, now); }
        }
        if (g_menu.open && g_menu.page == MP_MODS) { g_menu.count[MP_MODS] = menu_clamp_list(modsui_count()); if (g_menu.sel >= g_menu.count[MP_MODS]) g_menu.sel = g_menu.count[MP_MODS] ? g_menu.count[MP_MODS] - 1 : 0; }
        g_menu.mods_sub = g_menu.open && g_menu.page == MP_MODS && modsui_open_section();
        if (!g_menu.open) modsui_collapse();                                      // the Mods page always opens with its sections closed
        if (g_menu.open) {
            MenuStatus st = { g_autoleave_on, g_perteam_cfg, !L_is_joiner(), g_maps_n, g_altars_n, g_hostips[g_hostip_sel], g_npc_on ? g_npcs_n : 0, L_npc_live(), g_solo_cfg, g_joinaddr_text };
            g_menu_nlines = g_ti_active ? ti_layout(g_menu_lines, &g_menu_sel) : menu_layout(&g_menu, &st, menu_list_text, g_menu_lines, &g_menu_sel);
        } else g_menu_nlines = 0;
#ifdef LN_OVERLAY
        ov_publish_menu();
#endif
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
    static const uint8_t expect[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x70 };   // UCanvas::DrawText prologue
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
    if (g_disabled || !canvas || g_draw_faults >= 2) return;
    int menu_visible = g_menu.open && g_menu_nlines;
    if (!menu_visible && !toast_active() && !md_draw_active()) return;
    __try { if (menu_visible && !ov_text_hidden()) draw_menu(canvas); toast_draw(canvas); md_draw(canvas); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_draw_faults++;
        logf_("menu: EXCEPTION 0x%08lX while drawing (fault %d of 2)%s", GetExceptionCode(), g_draw_faults, g_draw_faults >= 2 ? " - the menu text is off for this run" : "");
    }
}

// Auto-leave (decision in pvpgate.h's autoleave_step; this only gathers the inputs and performs the travel)
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
    if (peers > 0 && T_quiet_ms(now) >= AUTOLEAVE_QUIET_MS) peers = 0;       // connected on paper, but nothing has arrived for AUTOLEAVE_QUIET_MS: the other side is gone
    int act = autoleave_step(&g_al, now, peers, g_al_duel, g_last_travel_ms);
    if (act == AL_ANNOUNCE) logf_("autoleave: the other player's connection is gone in this duel map - returning to your own game in %llu s unless something else is already travelling", AUTOLEAVE_GRACE_MS / 1000);
    else if (act == AL_LEAVE) { logf_("autoleave: leaving the duel map now (same travel as F6)"); do_travel(MAP_URL); }
}

static void on_tick(float dt) {
    static int first = 1, k10, k9, k11, devkeys = -1;
    static unsigned frame;
    if (first) {
        first = 0;
        logf_("first tick: GEngine=%p GWorld=%p base=%p", engine(), world(), g_base);
        crashguard_ready();                                 // booting worked: the crash-loop counter goes back to 0
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
    zen_bootstrap();
    static int zs_faults;
    if (zs_faults < 2) {
        __try { zen_stream_watch(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { zs_faults++; logf_("zenarena: stream watch EXCEPTION 0x%08lX (fault %d of 2)", GetExceptionCode(), zs_faults); }
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
    room_spawn_tick();
    shotrunner_tick();
    int focus = game_has_focus();
    menu_tick(focus, GetTickCount64());
    modsui_tick(focus);
    if (!focus) { k10 = k9 = k11 = 0; return; }
    // F keys: players get F1 (menu) and F2 (Mods page) only. Host / Join / Bring joiners / State dump / Log marker are F1 menu items,
    // hot reload is an F2 page item, F3 is left free for other mods. F9 cam.txt / F10 goto.txt / F11 lmtest.txt are developer keys, on only when
    // LanNative\devkeys.txt exists.
    if (devkeys < 0) { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sdevkeys.txt", g_dir); devkeys = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; logf_("keys: test keys F9 / F10 / F11 %s", devkeys ? "ON (devkeys.txt)" : "off"); }
    if (!devkeys) return;
    if (key_edge(VK_F10, &k10)) goto_file();
    if (key_edge(VK_F11, &k11)) lm_selftest();
    if (key_edge(VK_F9, &k9)) freecam_file();
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
    if (!g_disabled) { GUARDED("mod check after ServerAcknowledgePossession", lm_serverack_post(pc);) }
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

// Logs every client-travel request (menu Host/Join, the console's open/travel, and the game's own travels such as returning to a previous map)
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

// LAN-PvP guard. The PvP player-controller Blueprint calls this on BeginPlay when the game is offline and sends everyone back to the
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

// Duel score. AThePlainesGameMode::Killed calls killerPS->ScoreKill(victimPS, 1) for every kill by a player; when the victim is an NPC its AI controller
// has no PlayerState, so the victim is NULL and AAdversarialPlayerState::ScoreKill still records a kill in m_Scores - which is what the 1v1 / 3v3 match
// score, the round result and the recap count. This drops every kill whose victim is not a real player's PlayerState.
typedef void (*ScoreKill_t)(void *ps, void *victim, int points);
static ScoreKill_t g_orig_scorekill;
static int g_scorekill_faults;
static unsigned g_scorekill_skips;
static int btr_trial_bot_ps(void *ps);                       // bottrial.inc (included later): the player-like trial bot's own AdversarialPlayerState counts as a player for the score
static void hk_scorekill(void *ps, void *victim, int points) {
    int skip = 0;
    if (!g_disabled && g_scorekill_faults < 2) {
        __try {
            skip = !L_ps_is_player(victim) && !btr_trial_bot_ps(victim);
            if (skip) logf_("pvp score: ignored kill #%u by player state %p (victim %p is not a player - an NPC died, it does not count for the match)", ++g_scorekill_skips, ps, victim);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_scorekill_faults++; skip = 0;
            logf_("pvp score: EXCEPTION 0x%08lX (fault %d of 2)%s - the kill is counted as the game does", GetExceptionCode(), g_scorekill_faults, g_scorekill_faults >= 2 ? "; the NPC score filter is off for this run" : "");
        }
    }
    if (skip) return;
    g_orig_scorekill(ps, victim, points);
}

// Parry observer (read-only). NotifyParrySuccessful(this = the parried ATTACKER's defense component, delay, actor = the PARRIER, ...) reads the
// ParryPropertyDB weak pointer from the PARRIER's defense component (parrier+0x1BE8, then +0x1FC or, with the fix, +0x1F4), so
// L_parry_inspect inspects the parrier's component, not `this`.
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

static void plugins_tick(float dt);

static void plugins_postload(void *obj);
static int plugin_register(const wchar_t *mods_dir, const char *name, const wchar_t *dll, const wchar_t *dirw);
static void plugins_load_all(void);
static void servermode_tick(void);
static void servermode_install(void);
static int g_plugins_n;

static void hk_tick(void *self, float dt, uint8_t idle) {
    g_game_tid = GetCurrentThreadId();
    rows_drain_objects();                                   // data asset edits that arrived on a loader thread are applied here, on the game thread
    servermode_tick();                                      // unattended host steps (servermode.inc)
    plugins_tick(dt);                                       // plugin events and queued jobs (plugins.inc)
    if (!g_disabled) {
        __try { on_tick(dt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_disabled = 1;
            logf_("EXCEPTION 0x%08lX inside our tick work - LanNative is now switched off for this run", GetExceptionCode());
        }
    }
    g_orig_tick(self, dt, idle);
}

static uint64_t image_size(const uint8_t *base) {          // PE SizeOfImage of a loaded module, 0 if the header looks wrong
    __try {
        if (base[0] != 'M' || base[1] != 'Z') return 0;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

#include "modglue.inc"
#include "crashrep.inc"
#include "lanmods.inc"
#include "rowedit.inc"
#include "plugins.inc"
#include "secore.inc"
#include "content.inc"
#include "modsui.inc"
static int npc_mod_entry(int i, char *id, int idcap, char *label, int lcap) {   // the F1 NPC page's "Mod NPCs" / "Bots" groups (npc_finish_list)
    if (i < 0 || i >= g_np_n) return -1;
    const CNpc *n = &g_np[i]; if (!n->obj) return 0;                               // not built (template missing / invalid entry)
    int bot = 0; for (int b = 0; b < g_bt_n; b++) if (!strcmp(g_bt[b].mod, n->mod) && !strcmp(g_bt[b].id, n->id)) bot = 1;
    snprintf(id, (size_t)idcap, "%s", n->id); snprintf(label, (size_t)lcap, "%s", n->name[0] ? n->name : n->id);
    return bot ? 2 : 1;
}
static void npc_spawn_entry(const char *id) { void *ch = api_p_npc_spawn(CA, id, NULL, NULL, NULL); logf_("npc: menu spawn of the content entry %s -> %p", id, ch); }
static void room_palace(void) {
    static void *last_world; void *w = world(); if (!w || w == last_world) return; last_world = w;
    char path[300] = ""; __try { se_path_of(w, path, sizeof path); } __except (EXCEPTION_EXECUTE_HANDLER) { path[0] = 0; }
    if (!strstr(path, "/MainMap_GameModes/PVE/Theme_")) return;
    static wchar_t name[] = L"/Game/Maps/TrainingMap/Training"; struct { wchar_t *d; int n, m; } fs = { name, (int)(sizeof name / sizeof name[0]), (int)(sizeof name / sizeof name[0]) };
    void *cdo = api_p_find_cdo(NULL, "LevelStreamingKismet"), *f = cdo ? api_p_find_function(NULL, cdo, "LoadLevelInstance") : NULL, *ret = NULL;
    float loc[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 }; uint8_t ok = 0; void *args[5] = { &w, &fs, loc, rot, &ok };
    int c = f && api_p_call_params(NULL, cdo, f, args, &ret);
    logf_("rooms: Inner Palace (Training level) for meditation in %s -> call %d, loaded %d", strrchr(path, '.') ? strrchr(path, '.') + 1 : path, c, ok);
}
#if defined(__has_include)
#if __has_include("servermode.inc")
#include "servermode.inc"                    // the optional unattended-host module; without the file the hooks below are empty
#define LN_SERVERMODE 1
#endif
#endif
#ifndef LN_SERVERMODE
static void servermode_tick(void) {}
static void servermode_install(void) {}
#endif

#include "matchredir.inc"

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
    // The installer recognises this dll by the text "Gleam Loader loaded in" (and older builds by "LanNative M"): change both together.
    logf_("Gleam Loader loaded in %ls (version " GLEAM_VERSION ", base %p)", wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe, g_base);
    if (wcsstr(exe, L"Absolver-Win64-Shipping")) { safe_mode_detect(); crashrep_install(); }
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
    matchredir_install();
    if (parryfix_enabled()) apply_parryfix();
    apply_patch_table();
    L_init(&g_api);
    L_npc_init(&g_npcapi);
    read_telemetry_cfg();
    T_init(&g_api, &g_tel_cfg, GetCurrentProcessId());
    g_orig_tick = (Tick_t)hook(IDX_TICK, (void *)hk_tick);
    if (!g_orig_tick) { logf_("tick hook NOT installed - LanNative does nothing (game runs untouched)"); g_disabled = 1; return; }
    if (!g_safe_mode) se_install();                         // script extender: ProcessEvent hook (secore.inc)
    if (!cmdline_has_flag(GetCommandLineW(), L"nomods")) {
        g_orig_pakinit = (PakInit_t)hook(IDX_PAKINIT, (void *)hk_pakinit);
        if (!g_orig_pakinit) logf_("mods: pak hook NOT installed - the mod loader is off (stock game unchanged)");
    } else logf_("mods: -NoMods on the command line - the mod loader is off");
    g_lm_me.present = 1;                                              // until the mod loader fills it in: "no gameplay mods"
    lanmods_install();
    servermode_install();
    if (!cmdline_has_flag(GetCommandLineW(), L"nomods")) rowedit_install();
    int lan_on = read_lan();
    { int mode = pvpfix_mode(); g_pvpfix_on = mode >= 1; g_pvpflow_on = mode >= 2; }
    if (lan_on) {
        g_orig_clientrestart = (ClientRestart_t)hook(IDX_CLIENTRESTART, (void *)hk_clientrestart);
        g_orig_serverack = (ServerAck_t)hook(IDX_SERVERACK, (void *)hk_serverack);
        g_orig_netcleanup = (NetCleanup_t)hook(IDX_NETCLEANUP, (void *)hk_netcleanup);
    }
    // SetClientTravel: LAN travel log, AND the pvp guard's travel timestamp (a bot trial / solo duel travel arms the guard), so it also stays with the guard on
    if (lan_on || g_pvpfix_on) g_orig_settravel = (SetClientTravel_t)hook(IDX_SETCLIENTTRAVEL, (void *)hk_settravel);
    if (!g_orig_settravel) logf_("travel logging is unavailable (SetClientTravel hook not installed); the menu's travel items still work");
    logf_("lan: %d of 3 LAN-only hooks installed (joiner fixes, host tracking, body cleanup); SetClientTravel %s (%s)",
          !!g_orig_clientrestart + !!g_orig_serverack + !!g_orig_netcleanup, g_orig_settravel ? "installed" : "not installed",
          lan_on ? "lan on" : g_pvpfix_on ? "lan off; kept for the pvp guard" : "lan off, pvp guard off");
    if (g_pvpflow_on) { g_perteam_cfg = read_perteam(); L_set_perteam(g_perteam_cfg); g_solo_cfg = read_soloduel(); L_set_soloduel(g_solo_cfg); }
    g_autoleave_on = read_autoleave();
    hostip_load();
    read_npc_cfg();
    g_menu_on = read_menu_cfg();
#ifdef LN_OVERLAY
    if (g_menu_on) g_ov_on = read_menu_style();
#endif
    if (g_menu_on || g_lm_mode) {
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
    logf_("ready (%s%s%s%s%s%s%s): F1 menu, F2 Mods page; console initializes after the viewport exists",
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

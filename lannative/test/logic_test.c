// Offline test for lan_logic.c: fake UE objects in real memory, fake engine functions. Freed objects get PAGE_NOACCESS, so any use of a
// dangling pointer raises an access violation (reported as FAIL) instead of silently reading stale data.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "../lan_logic.h"
#include "../lan_telemetry.h"
#include "../parryfix.h"
#include "../objectscan.h"
#include "../console.h"
#include "../joinurl.h"
#include "../pvpgate.h"
#include "../parrylog.h"
#include "../menu.h"
#include "../hostip.h"
#include "../config.h"

#define OBJ_SIZE 0x7000
static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- fake objects ------------------------------------------------------------------------------------------------------
typedef struct { uint8_t pad[0x30]; void *super; } Cls;
static Cls C_object, C_player, C_localplayer, C_netconn, C_actor, C_char, C_controller, C_ps_base, C_ps, C_gs, C_gi, C_world, C_netdriver, C_netconn_cls, C_defense, C_advgm, C_advgs, C_advps, C_fpc;

static void *mk(Cls *cls) {
    uint8_t *p = VirtualAlloc(NULL, OBJ_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memset(p, 0, OBJ_SIZE);
    *(void **)(p + OFF_OBJ_CLASS) = cls;
    return p;
}
static void rm(void *o) { DWORD old; VirtualProtect(o, OBJ_SIZE, PAGE_NOACCESS, &old); }   // "freed"

// weak registry
typedef struct { void *obj; int serial; int alive; } WEnt;
static WEnt g_w[2048]; static int g_nw;
static void *f_weak_get(WeakPtr *w) { return (w->idx >= 0 && g_w[w->idx].alive && g_w[w->idx].serial == w->serial) ? g_w[w->idx].obj : NULL; }
static void f_weak_set(WeakPtr *w, void *obj) {
    if (!obj) { w->idx = -1; w->serial = 0; return; }
    for (int i = 0; i < g_nw; i++) if (g_w[i].obj == obj && g_w[i].alive) { w->idx = i; w->serial = g_w[i].serial; return; }
    g_w[g_nw] = (WEnt){ obj, 1, 1 }; w->idx = g_nw++; w->serial = 1;
}
static void kill(void *obj) { for (int i = 0; i < g_nw; i++) if (g_w[i].obj == obj) g_w[i].alive = 0; rm(obj); }

static void *g_destroyed[1024]; static int g_ndestroyed;
static int g_destroy_ok = 1;
static void *g_destroy_side_effect_from, *g_destroy_side_effect_target;
static uint8_t f_destroy(void *a) {
    if (!g_destroy_ok) return 0;
    g_destroyed[g_ndestroyed++] = a;
    if (a == g_destroy_side_effect_from && g_destroy_side_effect_target) kill(g_destroy_side_effect_target);
    kill(a);
    return 1;
}
static int was_destroyed(void *a) { for (int i = 0; i < g_ndestroyed; i++) if (g_destroyed[i] == a) return 1; return 0; }

static float g_loc[8][4]; static void *g_locobj[8]; static int g_nloc;
static uint8_t f_setloc(void *a, const float v[3]) { g_locobj[g_nloc] = a; memcpy(g_loc[g_nloc], v, 12); g_nloc++; return 1; }
static void *g_world;
static void *f_world(void) { return g_world; }
static void *f_cls_lp(void) { return &C_localplayer; }
static void *f_cls_gs(void) { return &C_gs; }
static void *f_cls_gi(void) { return &C_gi; }
static void *f_cls_ps(void) { return &C_ps; }
static void *f_cls_fc(void) { return &C_char; }
static void *g_scan[64]; static int g_nscan;
static void f_scan_objects(ObjectVisitor visit, void *context) { for (int i = 0; i < g_nscan; i++) visit(g_scan[i], context); }
static char g_log[200][600]; static int g_nlog;      // the real logger line buffer is 1024 bytes; the world line is ~300
static void f_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_log[g_nlog < 199 ? g_nlog++ : 199], 600, fmt, ap); va_end(ap); }
static int logged(const char *needle) { for (int i = 0; i < g_nlog; i++) if (strstr(g_log[i], needle)) return 1; return 0; }
// telemetry fakes
static struct { void *obj; char path[160]; } g_paths[1024]; static int g_npaths;
static void set_path(void *o, const char *p) { g_paths[g_npaths].obj = o; snprintf(g_paths[g_npaths].path, sizeof g_paths[0].path, "%s", p); g_npaths++; }
static int f_object_path(void *o, char *out, int cap) {
    for (int i = g_npaths - 1; i >= 0; i--) if (g_paths[i].obj == o) { snprintf(out, (size_t)cap, "%s", g_paths[i].path); return 1; }
    return 0;
}
static void *f_cls_nd(void) { return &C_netdriver; }
static void *f_cls_nc(void) { return &C_netconn_cls; }
static void *f_cls_def(void) { return &C_defense; }
static void *f_cls_advgm(void) { return &C_advgm; }
static void *f_cls_advgs(void) { return &C_advgs; }
static void *f_cls_advps(void) { return &C_advps; }
static void *f_cls_fpc(void) { return &C_fpc; }
static uint64_t f_post(void) { return 0xCC; }                       // the fake global FName MatchState::WaitingPostMatch
static struct { uint64_t v; char s[48]; } g_fn[8]; static int g_nfn;
static void set_fn(uint64_t v, const char *s) { g_fn[g_nfn].v = v; snprintf(g_fn[g_nfn].s, sizeof g_fn[0].s, "%s", s); g_nfn++; }
static int f_fname(uint64_t v, char *out, int cap) { for (int i = g_nfn - 1; i >= 0; i--) if (g_fn[i].v == v) { snprintf(out, (size_t)cap, "%s", g_fn[i].s); return 1; } return 0; }
static uint64_t g_now;
static uint64_t f_now(void) { return g_now; }
static char g_csv[3][64][700]; static int g_ncsv[3];
static void f_csv(int file, const char *line) { if (g_ncsv[file] < 64) snprintf(g_csv[file][g_ncsv[file]++], 700, "%s", line); }
static void f_wall(char *out, int cap) { snprintf(out, (size_t)cap, "2000-01-01 00:00:00.000"); }
static int count_logged(const char *needle) { int n = 0; for (int i = 0; i < g_nlog; i++) if (strstr(g_log[i], needle)) n++; return n; }
static const Api API = { f_weak_get, f_weak_set, f_destroy, f_cls_lp, f_cls_gs, f_cls_gi, f_cls_ps, f_cls_fc, f_scan_objects, f_setloc, f_world, f_log,
                         f_object_path, f_cls_nd, f_cls_nc, f_now, f_csv, f_wall, f_cls_def,
                         f_cls_advgm, f_cls_advgs, f_cls_advps, f_cls_fpc, f_fname, f_post };

#define U8(p, off)  (*(uint8_t *)((uint8_t *)(p) + (off)))
#define U32(p, off) (*(uint32_t *)((uint8_t *)(p) + (off)))
#define PTR(p, off) (*(void **)((uint8_t *)(p) + (off)))
#define I32(p, off) (*(int32_t *)((uint8_t *)(p) + (off)))
#define F32(p, off) (*(float *)((uint8_t *)(p) + (off)))
static void reset_tel(void);      // defined with the telemetry helpers below
static void tel_paths(void);

static void init_classes(void) {
    memset(&C_object, 0, sizeof C_object);
    C_actor.super = &C_object; C_char.super = &C_actor; C_controller.super = &C_actor;
    C_player.super = &C_object; C_localplayer.super = &C_player; C_netconn.super = &C_player;
    C_ps_base.super = &C_actor; C_ps.super = &C_ps_base; C_gs.super = &C_actor; C_gi.super = &C_object; C_world.super = &C_object;
    C_netdriver.super = &C_object; C_netconn_cls.super = &C_object; C_defense.super = &C_object;
    C_advgm.super = &C_actor; C_advgs.super = &C_actor; C_advps.super = &C_ps; C_fpc.super = &C_controller;
}

// A world with a local player controller list, game state, game instance (+ host local player in zone `zone`).
static struct { void *world, *gs, *gi, *lp, *localpc, *list; } W;
static void make_world(int server, int zone) {
    W.world = g_world = mk(&C_world);
    W.gs = mk(&C_gs); W.gi = mk(&C_gi); W.lp = mk(&C_localplayer); U8(W.lp, OFF_LP_ZONE) = (uint8_t)zone;
    PTR(W.world, OFF_W_GAMESTATE) = W.gs; PTR(W.world, OFF_W_GAMEINST) = W.gi;
    if (server) PTR(W.world, OFF_W_AUTHGM) = W.gi;                    // any non-null pointer
    void **players = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); players[0] = W.lp;
    PTR(W.gi, OFF_GI_LOCALPLAYERS) = players; I32(W.gi, OFF_GI_LOCALPLAYERS + 8) = 1;
    W.localpc = mk(&C_controller); PTR(W.localpc, OFF_PC_PLAYER) = W.lp; PTR(W.lp, OFF_PLAYER_CONTROLLER) = W.localpc; U8(W.localpc, OFF_ACTOR_ROLE) = ROLE_AUTH;
    W.list = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    PTR(W.world, OFF_W_PCLIST) = W.list; I32(W.world, OFF_W_PCLIST + 8) = 0;
    f_weak_set(&((WeakPtr *)W.list)[0], W.localpc); I32(W.world, OFF_W_PCLIST + 8) = 1;
}
static void add_pc_to_list(void *pc) { int n = I32(W.world, OFF_W_PCLIST + 8); f_weak_set(&((WeakPtr *)W.list)[n], pc); I32(W.world, OFF_W_PCLIST + 8) = n + 1; }
static void *mk_pawn(int role, int remote) { void *p = mk(&C_char); U8(p, OFF_ACTOR_ROLE) = (uint8_t)role; U8(p, OFF_ACTOR_REMOTEROLE) = (uint8_t)remote; return p; }
static void *mk_joiner_pc(void *pawn, void *ps) {   // a remote player's controller on the host
    void *pc = mk(&C_controller); void *conn = mk(&C_netconn);
    PTR(pc, OFF_PC_PLAYER) = conn; PTR(pc, OFF_CTRL_PAWN) = pawn; PTR(pc, OFF_CTRL_PLAYERSTATE) = ps; U8(pc, OFF_ACTOR_ROLE) = ROLE_AUTH;
    return pc;
}

static void reset_log(void) { g_nlog = 0; g_ndestroyed = 0; g_nloc = 0; }

// ---- scenarios ----------------------------------------------------------------------------------------------------------
static void test_joiner(void) {
    printf("-- joiner\n");
    make_world(0, 0);
    void *old = mk_pawn(ROLE_AUTH, 0), *nw = mk_pawn(ROLE_SIM, ROLE_AUTH);
    PTR(W.localpc, OFF_CTRL_PAWN) = old;
    CHECK(!L_is_joiner(), "single-player pawn is not a joiner");
    WeakPtr ow; L_clientrestart_pre(W.localpc, &ow);
    PTR(W.localpc, OFF_CTRL_PAWN) = nw;                              // the original ClientRestart possesses the new pawn
    L_clientrestart_post(W.localpc, nw, &ow);
    CHECK(U8(nw, OFF_ACTOR_ROLE) == ROLE_AUTO, "new pawn Role 1 -> 2, is %d", U8(nw, OFF_ACTOR_ROLE));
    CHECK(was_destroyed(old), "the pre-join character is removed");
    CHECK(!was_destroyed(nw), "the new pawn is kept");
    CHECK(L_is_joiner(), "controlling a pawn whose RemoteRole is Authority = joiner");

    // PlayerControllerList can retain the old local controller after F7. The UPlayer backlink names the live one.
    void *stale = mk(&C_controller), *stale_pawn = mk_pawn(ROLE_AUTH, ROLE_AUTO);
    PTR(stale, OFF_PC_PLAYER) = W.lp; PTR(stale, OFF_CTRL_PAWN) = stale_pawn; add_pc_to_list(stale);
    WeakPtr list_first = ((WeakPtr *)W.list)[0]; ((WeakPtr *)W.list)[0] = ((WeakPtr *)W.list)[1]; ((WeakPtr *)W.list)[1] = list_first;
    CHECK(L_is_joiner(), "the active UPlayer controller wins over a stale local controller after F7");

    // restart with the same pawn again (respawn handler fires repeatedly): nothing destroyed, role stays
    reset_log(); WeakPtr ow2; L_clientrestart_pre(W.localpc, &ow2); L_clientrestart_post(W.localpc, nw, &ow2);
    CHECK(g_ndestroyed == 0, "re-restart with the same pawn destroys nothing");

    // a replicated proxy as the old pawn (Role 1) must not be destroyed by us
    void *proxy = mk_pawn(ROLE_SIM, ROLE_AUTH), *nw2 = mk_pawn(ROLE_SIM, ROLE_AUTH);
    PTR(W.localpc, OFF_CTRL_PAWN) = proxy; WeakPtr ow3; L_clientrestart_pre(W.localpc, &ow3);
    PTR(W.localpc, OFF_CTRL_PAWN) = nw2; L_clientrestart_post(W.localpc, nw2, &ow3);
    CHECK(!was_destroyed(proxy), "an old pawn that is not Role=Authority is left to the engine");

    // The local clone can already be detached before ClientRestart. A bounded object-array sweep removes it while keeping our
    // replicated pawn and other simulated characters untouched.
    void *clone = mk_pawn(ROLE_AUTH, ROLE_SIM), *other = mk_pawn(ROLE_SIM, ROLE_AUTH);
    void *npc = mk_pawn(ROLE_AUTH, ROLE_SIM), *cdo = mk_pawn(ROLE_AUTH, ROLE_SIM);
    PTR(clone, OFF_PAWN_CONTROLLER) = W.localpc;
    PTR(npc, OFF_PAWN_CONTROLLER) = mk(&C_controller);                 // authority character, but not ours
    PTR(cdo, OFF_PAWN_CONTROLLER) = W.localpc; U32(cdo, OFF_OBJ_FLAGS) = 0x10;
    g_scan[0] = nw2; g_scan[1] = clone; g_scan[2] = other; g_scan[3] = npc; g_scan[4] = cdo; g_nscan = 5;
    CHECK(L_sweep_joiner_clones() == 1, "the detached authority clone is found by the bounded sweep");
    CHECK(was_destroyed(clone), "the detached authority clone is removed");
    CHECK(!was_destroyed(nw2) && !was_destroyed(other), "controlled and simulated pawns are kept by the sweep");
    CHECK(!was_destroyed(npc), "an authority character owned by another controller is kept");
    CHECK(!was_destroyed(cdo), "a class-default object is kept");
    void *refused = mk_pawn(ROLE_AUTH, ROLE_SIM); PTR(refused, OFF_PAWN_CONTROLLER) = W.localpc;
    g_scan[0] = refused; g_nscan = 1; g_destroy_ok = 0;
    CHECK(L_sweep_joiner_clones() == 0, "a failed AActor::Destroy is not reported as removed");
    CHECK(!was_destroyed(refused), "a clone remains live when AActor::Destroy refuses it");
    g_destroy_ok = 1;
    void *first = mk_pawn(ROLE_AUTH, ROLE_SIM), *second = mk_pawn(ROLE_AUTH, ROLE_SIM);
    PTR(first, OFF_PAWN_CONTROLLER) = W.localpc; PTR(second, OFF_PAWN_CONTROLLER) = W.localpc;
    g_scan[0] = first; g_scan[1] = second; g_nscan = 2;
    g_destroy_side_effect_from = first; g_destroy_side_effect_target = second;
    CHECK(L_sweep_joiner_clones() == 1, "a victim invalidated by the first EndPlay is not destroyed through a stale pointer");
    CHECK(!was_destroyed(second), "the invalidated second victim was re-resolved and skipped");
    g_destroy_side_effect_from = g_destroy_side_effect_target = NULL;
    g_nscan = 0;

    // old pawn already destroyed by the engine before our post step: no use-after-free
    void *dead = mk_pawn(ROLE_AUTH, 0), *nw3 = mk_pawn(ROLE_SIM, ROLE_AUTH);
    PTR(W.localpc, OFF_CTRL_PAWN) = dead; WeakPtr ow4; L_clientrestart_pre(W.localpc, &ow4);
    kill(dead); PTR(W.localpc, OFF_CTRL_PAWN) = nw3; reset_log();
    L_clientrestart_post(W.localpc, nw3, &ow4);
    CHECK(g_ndestroyed == 0, "an already-destroyed old pawn is not touched");

    // a host pawn arriving (RemoteRole = AutonomousProxy) is left alone
    void *hp = mk_pawn(ROLE_AUTH, ROLE_AUTO); WeakPtr ow5; L_clientrestart_pre(W.localpc, &ow5); L_clientrestart_post(W.localpc, hp, &ow5);
    CHECK(U8(hp, OFF_ACTOR_ROLE) == ROLE_AUTH, "host's own pawn untouched");

    // F6/F7 guard
    PTR(W.localpc, OFF_CTRL_PAWN) = nw3; uint64_t armed = 0;
    CHECK(L_guard_leave(&armed, 1000, "F6") == 0, "joiner: first press refused");
    CHECK(L_guard_leave(&armed, 3000, "F6") == 1, "joiner: second press within 5 s allowed");
    CHECK(L_guard_leave(&armed, 4000, "F6") == 0, "joiner: guard re-arms after use");
    CHECK(L_guard_leave(&armed, 10000, "F6") == 0, "joiner: press after 6 s counts as a first press");
    PTR(W.localpc, OFF_CTRL_PAWN) = hp; armed = 0;
    CHECK(L_guard_leave(&armed, 1000, "F6") == 1, "host: no guard");
}

static void test_host(void) {
    printf("-- host\n");
    make_world(1, 4);
    reset_log();
    void *hostpawn = mk_pawn(ROLE_AUTH, ROLE_AUTO); PTR(W.localpc, OFF_CTRL_PAWN) = hostpawn;
    void *root = mk(&C_object); PTR(hostpawn, OFF_ACTOR_ROOTCOMP) = root;
    float hp[3] = { 10.f, 20.f, 30.f }; memcpy((uint8_t *)root + OFF_ROOT_C2W_POS, hp, 12);

    U8(W.gs, OFF_GS_COOPVOICE) = 1; U8(W.gi, OFF_GI_CANSPAWNALONE) = 1;
    L_periodic();
    CHECK(U8(W.gs, OFF_GS_COOPVOICE) == 0, "coop voice chat flag cleared");
    CHECK(U8(W.gi, OFF_GI_CANSPAWNALONE) == 0, "spawn-alone flag cleared on the host");

    // a joiner connects
    void *jps = mk(&C_ps); U8(jps, OFF_PS_RESPAWNZONE) = 9;
    void *jpawn = mk_pawn(ROLE_AUTH, ROLE_AUTO), *jpc = mk_joiner_pc(jpawn, jps); add_pc_to_list(jpc);
    L_serverack_post(jpc, jpawn);
    CHECK(U8(jps, OFF_PS_RESPAWNZONE) == 4, "joiner respawn zone synced to the host's (4), is %d", U8(jps, OFF_PS_RESPAWNZONE));
    CHECK(logged("tracking joiner"), "joiner is logged as tracked");

    // the host's own controller also acknowledges possession: ignored
    void *hps = mk(&C_ps); U8(hps, OFF_PS_RESPAWNZONE) = 7; PTR(W.localpc, OFF_CTRL_PLAYERSTATE) = hps;
    L_serverack_post(W.localpc, hostpawn); L_periodic();
    CHECK(U8(hps, OFF_PS_RESPAWNZONE) == 7, "the host's own player state is never written");

    // a controller whose PlayerState is only the base class must never be written
    void *bps = mk(&C_ps_base); U8(bps, OFF_PS_RESPAWNZONE) = 7;
    void *bpawn = mk_pawn(ROLE_AUTH, ROLE_AUTO), *bpc = mk_joiner_pc(bpawn, bps);
    L_serverack_post(bpc, bpawn); L_periodic();
    CHECK(U8(bps, OFF_PS_RESPAWNZONE) == 7, "a non-FightingPlayerState is never written (heap-corruption guard)");

    // the host walks into another zone: the respawn zone follows
    U8(W.lp, OFF_LP_ZONE) = 5; L_periodic();
    CHECK(U8(jps, OFF_PS_RESPAWNZONE) == 5, "respawn zone follows the host to zone 5");

    // summon
    reset_log(); L_summon();
    CHECK(g_nloc == 2, "summon moves the two connected joiners (moved %d)", g_nloc);
    CHECK(g_nloc >= 1 && g_loc[0][0] == 160.f && g_loc[0][1] == 20.f && g_loc[0][2] == 80.f, "first joiner next to the host: %.0f,%.0f,%.0f", g_loc[0][0], g_loc[0][1], g_loc[0][2]);

    // the joiner's connection drops: controller lingers with Player = NULL
    reset_log(); PTR(jpc, OFF_PC_PLAYER) = NULL;
    L_periodic();
    CHECK(was_destroyed(jpawn), "a disconnected joiner's character is removed");
    CHECK(!was_destroyed(hostpawn), "the host's character is kept");
    int d = g_ndestroyed; L_periodic(); L_periodic();
    CHECK(g_ndestroyed == d, "nothing is destroyed twice");

    // OnNetCleanup: server-side controller with a pawn
    void *p2 = mk_pawn(ROLE_AUTH, ROLE_AUTO), *pc2 = mk_joiner_pc(p2, mk(&C_ps)); CleanupPair w;
    reset_log(); L_netcleanup_pre(pc2, &w); L_netcleanup_post(&w);
    CHECK(was_destroyed(p2), "OnNetCleanup removes the leaving joiner's character");
    // the engine already destroyed the pawn between pre and post
    void *p3 = mk_pawn(ROLE_AUTH, ROLE_AUTO), *pc3 = mk_joiner_pc(p3, mk(&C_ps)); reset_log();
    L_netcleanup_pre(pc3, &w); kill(p3); L_netcleanup_post(&w);
    CHECK(g_ndestroyed == 0, "an already-destroyed pawn is not touched again");
    // client side: the client's own controller has Role 2 and must keep its pawn
    void *p4 = mk_pawn(ROLE_SIM, ROLE_AUTH), *pc4 = mk_joiner_pc(p4, mk(&C_ps)); U8(pc4, OFF_ACTOR_ROLE) = ROLE_AUTO; reset_log();
    L_netcleanup_pre(pc4, &w); L_netcleanup_post(&w);
    CHECK(g_ndestroyed == 0, "a client's own controller does not destroy its pawn");

    // second joiner whose controller object is freed outright (no OnNetCleanup, no Player=NULL step)
    void *qps = mk(&C_ps); void *qpawn = mk_pawn(ROLE_AUTH, ROLE_AUTO), *qpc = mk_joiner_pc(qpawn, qps); add_pc_to_list(qpc);
    L_serverack_post(qpc, qpawn); reset_log();
    kill(qpc); kill(qps); L_periodic();
    CHECK(was_destroyed(qpawn), "controller freed outright: its character is still removed through the saved weak pointer");

    // Rejoining can transfer a pawn to a new controller before the old controller's delayed cleanup runs.
    void *shared = mk_pawn(ROLE_AUTH, ROLE_AUTO), *oldpc = mk_joiner_pc(shared, mk(&C_ps));
    void *newpc = mk_joiner_pc(shared, mk(&C_ps)); add_pc_to_list(oldpc); add_pc_to_list(newpc);
    L_serverack_post(oldpc, shared); L_serverack_post(newpc, shared); PTR(oldpc, OFF_PC_PLAYER) = NULL; reset_log();
    L_periodic();
    CHECK(!was_destroyed(shared), "a pawn reassigned to a connected rejoin controller is never removed by old cleanup");
    CHECK(logged("another connected controller owns it"), "reassigned pawn skip is logged");
}

static void test_join_url(void) {
    wchar_t url[900];
    printf("-- join URL\n");
    CHECK(join_url_format("192.168.1.20", "", L"/Game/Maps/Main.Altar", 10, url, 900), "saved join URL formats");
    CHECK(wcscmp(url, L"192.168.1.20:7777?CheckPoint=/Game/Maps/Main.Altar?zone=10") == 0, "saved checkpoint and zone are appended");
    CHECK(join_url_format("192.168.1.20:8000", "?zone=3", L"ignored", 9, url, 900), "manual join URL formats");
    CHECK(wcscmp(url, L"192.168.1.20:8000?zone=3") == 0, "manual ip.txt options override the save");
    CHECK(!join_url_format("", "", L"", 0, url, 900), "empty address is rejected");
    CHECK(join_options_is_none("none") && join_options_is_none("  NONE\r\n") && join_options_is_none("None "), "line 2 'none' (any case, trailing space/CRLF) means no options");
    CHECK(!join_options_is_none("") && !join_options_is_none("?zone=3") && !join_options_is_none("nonexistent") && !join_options_is_none(NULL), "other line-2 values are not 'none'");
    CHECK(join_url_format("192.168.1.50", "", L"", 0, url, 900) && wcscmp(url, L"192.168.1.50:7777") == 0, "a bare join (no options) is just ip:port");
}

static void test_pvp_gate(void) {
    printf("-- LAN-PvP guard decision\n");
    const char *coliseum = "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum";
    const char *domination = "/Game/Maps/MainMap_GameModes/3v3/GM_3v3_Domination_Garden.GM_3v3_Domination_Garden";
    CHECK(pvp_path_is_duel(coliseum) && pvp_path_is_duel(domination), "1v1 and 3v3 world paths are duel maps");
    CHECK(pvp_path_is_duel("/GAME/MAPS/MAINMAP_GAMEMODES/1V1/X.X"), "the path match ignores case");
    CHECK(!pvp_path_is_duel("/Game/Maps/MainMap/MainMap_Main.MainMap_Main"), "the main world is not a duel map");
    CHECK(!pvp_path_is_duel("/Game/Maps/MainMap_GameModes/PVE/PVE_Mine.PVE_Mine"), "the PvE mine is not a duel map");
    CHECK(!pvp_path_is_duel("/Game/Maps/StartMap/StartMap.StartMap") && !pvp_path_is_duel("") && !pvp_path_is_duel(NULL) && !pvp_path_is_duel("?"), "title map, empty, null and '?' are not duel maps");
    // the real log: travel requested at t, the game's return call 0.75 s later
    CHECK(pvp_should_suppress(1, 1, 25374, 24623, PVP_WINDOW_MS), "the return call 0.75 s after the open is skipped");
    CHECK(pvp_should_suppress(1, 1, 24623 + PVP_WINDOW_MS, 24623, PVP_WINDOW_MS), "still skipped at exactly the window edge");
    CHECK(!pvp_should_suppress(1, 1, 24623 + PVP_WINDOW_MS + 1, 24623, PVP_WINDOW_MS), "after the window the quit/return call passes through");
    CHECK(!pvp_should_suppress(1, 0, 25374, 24623, PVP_WINDOW_MS), "outside a duel map nothing is skipped");
    CHECK(!pvp_should_suppress(0, 1, 25374, 24623, PVP_WINDOW_MS), "pvpfix off: nothing is skipped");
    CHECK(!pvp_should_suppress(1, 1, 25374, 0, PVP_WINDOW_MS), "no travel request seen yet: nothing is skipped");
    CHECK(!pvp_should_suppress(1, 1, 1000, 5000, PVP_WINDOW_MS), "a clock that went backwards: nothing is skipped");
    CHECK(pvp_latest(0, 0) == 0 && pvp_latest(5, 0) == 5 && pvp_latest(0, 9) == 9 && pvp_latest(5, 9) == 9 && pvp_latest(9, 5) == 9, "the newest of two event times, 0 = no event");
    // the M3.5 two-PC log: open at T0, the joiner connected 77.9 s later and the host's own return call fired 0.3 s after that
    const uint64_t T0 = 24623, join_at = T0 + 77933, call_at = T0 + 78250;
    CHECK(!pvp_should_suppress(1, 1, call_at, pvp_latest(T0, 0), PVP_WINDOW_MS), "travel-only anchor (M3.5): the host was sent home when the joiner arrived (the real failure)");
    CHECK(pvp_should_suppress(1, 1, call_at, pvp_latest(T0, join_at), PVP_WINDOW_MS), "with the peer-count change as an anchor (M3.6) the same call is skipped");
    CHECK(!pvp_should_suppress(1, 1, join_at + PVP_WINDOW_MS + 1, pvp_latest(T0, join_at), PVP_WINDOW_MS), "a later quit/return still passes through");
}

static void test_pvp_flow(void) {
    printf("-- LAN-PvP flow (GameInstance.m_bDebugFlow)\n");
    const char *duel = "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum";
    const char *main_world = "/Game/Maps/MainMap/MainMap_Main.MainMap_Main";
    make_world(1, 0); reset_tel(); set_path(W.world, duel);
    void *gm = mk(&C_advgm); PTR(W.world, OFF_W_AUTHGM) = gm; L_init(&API);
    CHECK(L_pvp_flow(0) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 0, "switched off: nothing is written");
    CHECK(L_pvp_flow(1) == 1 && U8(W.gi, OFF_GI_DEBUGFLOW) == 1, "duel map + duel game mode on this PC: the flag is set");
    CHECK(logged("pvp flow: duel world") && logged("m_bDebugFlow 0 -> 1"), "the decision and the write are logged");
    int logs = g_nlog; L_pvp_flow(1); L_pvp_flow(1); L_pvp_flow(1);
    CHECK(g_nlog == logs && U8(W.gi, OFF_GI_DEBUGFLOW) == 1, "later frames log nothing and keep the flag");
    U8(W.gi, OFF_GI_DEBUGFLOW) = 0; L_pvp_flow(1);
    CHECK(U8(W.gi, OFF_GI_DEBUGFLOW) == 1 && logged("cleared by the game"), "a flag the game clears while we hold it is set again");
    PTR(W.world, OFF_W_AUTHGM) = mk(&C_gi);                                   // the duel is over: the next world has an ordinary game mode
    CHECK(L_pvp_flow(1) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 0 && logged("restored to 0"), "when the duel ends the original value is restored");
    CHECK(L_pvp_flow(1) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 0, "and it is not set again outside a duel");

    // a client (no authority game mode) in a duel map never sets it: only the PC running the duel game mode needs it
    make_world(0, 0); reset_tel(); set_path(W.world, duel); L_init(&API);
    CHECK(L_pvp_flow(1) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 0, "a client in a duel map does not set the flag");
    // the main world with a duel game mode object (path check) and a duel map with a wrong-class game mode (class check)
    make_world(1, 0); reset_tel(); set_path(W.world, main_world); PTR(W.world, OFF_W_AUTHGM) = mk(&C_advgm); L_init(&API);
    CHECK(L_pvp_flow(1) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 0, "a non-duel map never sets the flag");
    make_world(1, 0); reset_tel(); set_path(W.world, duel); PTR(W.world, OFF_W_AUTHGM) = mk(&C_object); L_init(&API);
    CHECK(L_pvp_flow(1) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 0, "a game mode of the wrong class is never treated as the duel game mode");
    make_world(1, 0); reset_tel(); set_path(W.world, duel); PTR(W.world, OFF_W_AUTHGM) = mk(&C_advgm); PTR(W.world, OFF_W_GAMEINST) = mk(&C_object); L_init(&API);
    CHECK(L_pvp_flow(1) == 0, "a game instance of the wrong class is never written");
    // someone else already set it: leave it alone, and never clear it afterwards
    make_world(1, 0); reset_tel(); set_path(W.world, duel); gm = mk(&C_advgm); PTR(W.world, OFF_W_AUTHGM) = gm; U8(W.gi, OFF_GI_DEBUGFLOW) = 1; L_init(&API);
    CHECK(L_pvp_flow(1) == 0 && U8(W.gi, OFF_GI_DEBUGFLOW) == 1 && logged("already 1"), "an existing m_bDebugFlow=1 is left alone");
    PTR(W.world, OFF_W_AUTHGM) = mk(&C_gi); L_pvp_flow(1);
    CHECK(U8(W.gi, OFF_GI_DEBUGFLOW) == 1, "and is not cleared when the duel ends (it was not ours)");
    // no world: no crash, the held state is reported unchanged
    g_world = NULL; CHECK(L_pvp_flow(1) == 0, "no world (loading): nothing happens");
}

static void test_pvp_teams(void) {
    printf("-- 3v3 players per team (M3.9)\n");
    const char *d3 = "/Game/Maps/MainMap_GameModes/3v3/OPTI_GM_3v3_Forest_Village_Domination_1.OPTI_GM_3v3_Forest_Village_Domination_1";
    const char *d1 = "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum";
    make_world(1, 0); reset_tel(); set_path(W.world, d3);
    void *gm = mk(&C_advgm); PTR(W.world, OFF_W_AUTHGM) = gm; I32(gm, OFF_ADVGM_NUMTEAMS) = 2; I32(gm, OFF_ADVGM_PERTEAM) = 3; L_init(&API);
    L_set_perteam(0); L_pvp_flow(1);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 3 && !L_perteam_held(), "setting 0 leaves the game's 3 per team");
    L_set_perteam(1); L_pvp_flow(1);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 1 && L_perteam_held() && logged("pvp teams: 3v3 game mode") && logged("starts with 2 players instead of 6"), "3v3 + duel game mode: 3 -> 1 per team, logged");
    int logs = g_nlog; L_pvp_flow(1); L_pvp_flow(1);
    CHECK(g_nlog == logs, "later frames log nothing");
    I32(gm, OFF_ADVGM_PERTEAM) = 3; L_pvp_flow(1);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 1 && logged("reset by the game"), "a value the game resets is set again");
    L_pvp_flow(0);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 3 && !L_perteam_held() && logged("pvp teams: released"), "switching the flow off restores 3");
    // world change while held: the old game mode is gone, nothing is written to it
    L_pvp_flow(1); CHECK(L_perteam_held(), "held again");
    void *oldgm = gm; I32(oldgm, OFF_ADVGM_PERTEAM) = 1;
    PTR(W.world, OFF_W_AUTHGM) = mk(&C_gi);
    L_pvp_flow(1);
    CHECK(!L_perteam_held() && I32(oldgm, OFF_ADVGM_PERTEAM) == 1 && logged("nothing written back"), "a game mode that disappeared is never written to");
    // a 1v1 map and a non-3v3 duel game mode are never touched; per-team 2 means 4 players
    make_world(1, 0); reset_tel(); set_path(W.world, d1); gm = mk(&C_advgm); PTR(W.world, OFF_W_AUTHGM) = gm; I32(gm, OFF_ADVGM_NUMTEAMS) = 2; I32(gm, OFF_ADVGM_PERTEAM) = 1; L_init(&API);
    L_set_perteam(1); L_pvp_flow(1);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 1 && !L_perteam_held(), "1v1: untouched");
    make_world(1, 0); reset_tel(); set_path(W.world, d3); gm = mk(&C_advgm); PTR(W.world, OFF_W_AUTHGM) = gm; I32(gm, OFF_ADVGM_NUMTEAMS) = 2; I32(gm, OFF_ADVGM_PERTEAM) = 3; L_init(&API);
    L_set_perteam(2); L_pvp_flow(1);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 2 && logged("starts with 4 players instead of 6"), "per-team 2 -> 4 players");
    make_world(1, 0); reset_tel(); set_path(W.world, d3); gm = mk(&C_advgm); PTR(W.world, OFF_W_AUTHGM) = gm; I32(gm, OFF_ADVGM_NUMTEAMS) = 99; I32(gm, OFF_ADVGM_PERTEAM) = 3; L_init(&API);
    L_set_perteam(1); L_pvp_flow(1);
    CHECK(I32(gm, OFF_ADVGM_PERTEAM) == 3, "an implausible team count is never trusted");
    make_world(0, 0); reset_tel(); set_path(W.world, d3); L_init(&API);
    L_set_perteam(1); CHECK(L_pvp_flow(1) == 0 && !L_perteam_held(), "a client in a 3v3 map changes nothing");
    L_set_perteam(0);
}

static void test_autoleave(void) {
    printf("-- auto-leave when the other player is gone (M3.9)\n");
    AutoLeave s; autoleave_reset(&s);
    CHECK(autoleave_step(&s, 1000, 0, 1, 0) == AL_NONE, "no peer yet: nothing");
    CHECK(autoleave_step(&s, 2000, 1, 1, 500) == AL_NONE, "peer connected: nothing");
    CHECK(autoleave_step(&s, 9000, 0, 1, 500) == AL_ANNOUNCE, "peer gone: announced");
    CHECK(autoleave_step(&s, 10000, 0, 1, 500) == AL_NONE && autoleave_step(&s, 11999, 0, 1, 500) == AL_NONE, "within the grace period: nothing");
    CHECK(autoleave_step(&s, 12000, 0, 1, 500) == AL_LEAVE, "after the grace period: leave");
    CHECK(autoleave_step(&s, 13000, 0, 1, 500) == AL_NONE && autoleave_step(&s, 20000, 0, 1, 500) == AL_NONE, "fires once");
    autoleave_reset(&s); autoleave_step(&s, 1000, 1, 1, 0); autoleave_step(&s, 2000, 0, 1, 0);
    CHECK(autoleave_step(&s, 2500, 1, 1, 0) == AL_NONE && autoleave_step(&s, 20000, 1, 1, 0) == AL_NONE, "peer reconnects within the grace period: no leave");
    CHECK(autoleave_step(&s, 21000, 0, 1, 0) == AL_ANNOUNCE, "and the timer restarts the next time it is gone");
    autoleave_reset(&s); autoleave_step(&s, 1000, 1, 1, 0);
    CHECK(autoleave_step(&s, 5000, 0, 1, 4000) == AL_NONE && autoleave_step(&s, 9000, 0, 1, 4000) == AL_NONE, "a travel requested after the peer was seen cancels it (already leaving)");
    autoleave_reset(&s); autoleave_step(&s, 1000, 1, 0, 0);
    CHECK(autoleave_step(&s, 5000, 0, 0, 0) == AL_NONE && autoleave_step(&s, 9000, 0, 0, 0) == AL_NONE, "not a duel map: never");
    autoleave_reset(&s); autoleave_step(&s, 1000, 1, 1, 0); autoleave_step(&s, 2000, 0, 1, 0);
    CHECK(autoleave_step(&s, 3000, 0, 0, 0) == AL_NONE && autoleave_step(&s, 9000, 0, 1, 0) == AL_NONE, "leaving the duel map forgets the peer");
    CHECK(pvp_path_is_3v3("/Game/Maps/MainMap_GameModes/3v3/X.X") && !pvp_path_is_3v3("/Game/Maps/MainMap_GameModes/1v1/X.X") && !pvp_path_is_3v3(NULL), "3v3 path check");
}

static const char *t_list_text(int page, int i) { static char b[64]; snprintf(b, sizeof b, "p%d-item%d", page, i); return b; }

static void test_menu(void) {
    printf("-- M4.0 menu\n");
    Menu m; menu_init(&m, 3, 40);
    MenuStatus st = { 1, 1, 1, 3, 40, "auto" };
    char lines[MENU_LINES_MAX][MENU_LINE_MAX]; int sel;
    CHECK(menu_layout(&m, &st, t_list_text, lines, &sel) == 0 && sel == -1, "closed menu draws nothing");
    CHECK(menu_key(&m, MK_DOWN).action == MA_NONE && menu_key(&m, MK_SELECT).action == MA_NONE && !m.open, "keys other than F1 do nothing while closed");
    menu_key(&m, MK_TOGGLE);
    CHECK(m.open && m.page == MP_MAIN && m.sel == 0, "F1 opens at the first entry");
    int n = menu_layout(&m, &st, t_list_text, lines, &sel);
    CHECK(n == MENU_MAIN_COUNT + 1 && sel == 1 && strstr(lines[0], "LanNative menu") && strstr(lines[1], "Host"), "main page: title + 9 entries, first selected");
    CHECK(strstr(lines[6], "Auto-leave: ON") && strstr(lines[7], "players per team: 1") && strstr(lines[8], "Host address: auto") && strstr(lines[9], "(3)") && strstr(lines[10], "(40)"), "values shown: auto-leave on, per-team 1, host address, list sizes");
    st.autoleave_on = 0; st.perteam = 0; st.is_host_world = 0; menu_layout(&m, &st, t_list_text, lines, &sel);
    CHECK(strstr(lines[6], "off") && strstr(lines[7], "game's 3") && strstr(lines[3], "host only"), "labels follow the status (off, game's 3, host only)");
    menu_key(&m, MK_UP);
    CHECK(m.sel == MENU_MAIN_COUNT - 1, "Up from the first entry wraps to the last");
    menu_key(&m, MK_DOWN);
    CHECK(m.sel == 0, "Down from the last wraps to the first");
    static const int expect[8] = { MA_HOST, MA_JOIN, MA_SUMMON, MA_STATE, MA_MARK, MA_AUTOLEAVE, MA_PERTEAM, MA_HOSTIP };
    for (int i = 0; i < 8; i++) {
        m.sel = i; MenuAct a = menu_key(&m, MK_SELECT);
        CHECK(a.action == expect[i] && m.open, "main entry %d returns its action and keeps the menu open", i);
    }
    m.sel = 8; MenuAct a = menu_key(&m, MK_SELECT);
    CHECK(a.action == MA_NONE && m.page == MP_MAPS && m.sel == 0, "Map list... opens the map page");
    menu_layout(&m, &st, t_list_text, lines, &sel);
    CHECK(strstr(lines[0], "Maps") && strstr(lines[1], "p1-item0") && sel == 1 && strstr(lines[4], "1 of 3"), "map page lists the 3 entries and a position footer");
    menu_key(&m, MK_DOWN); menu_key(&m, MK_DOWN); menu_key(&m, MK_DOWN);
    CHECK(m.sel == 0, "list selection wraps");
    menu_key(&m, MK_UP); a = menu_key(&m, MK_SELECT);
    CHECK(a.action == MA_TRAVEL && a.index == 2 && !m.open, "Enter on a list entry travels to it and closes the menu");
    menu_key(&m, MK_TOGGLE);
    CHECK(m.open && m.page == MP_MAIN && m.sel == 0, "reopening starts at the main page");
    m.sel = 9; menu_key(&m, MK_SELECT);
    CHECK(m.page == MP_ALTARS, "Altar teleport... opens the altar page");
    for (int i = 0; i < 25; i++) menu_key(&m, MK_DOWN);
    n = menu_layout(&m, &st, t_list_text, lines, &sel);
    CHECK(n == MENU_ROWS + 2 && sel >= 1 && strstr(lines[sel], "p2-item25") && strstr(lines[n - 1], "26 of 40"), "long list: a window of %d rows that always contains the selection (n=%d)", MENU_ROWS, n);
    m.sel = 39; n = menu_layout(&m, &st, t_list_text, lines, &sel);
    CHECK(strstr(lines[sel], "p2-item39") && n == MENU_ROWS + 2, "the last entry is shown at the end of the window");
    { Menu t = m; MenuAct ta = menu_key(&t, MK_SELECT); CHECK(ta.action == MA_TELEPORT && ta.index == 39 && !t.open, "Enter on an altar teleports (not travels) and closes the menu"); }
    menu_key(&m, MK_BACK);
    CHECK(m.open && m.page == MP_MAIN && m.sel == 9, "Back from a list returns to the entry that opened it");
    menu_key(&m, MK_BACK);
    CHECK(!m.open, "Back on the main page closes the menu");
    // empty lists cannot be opened
    Menu e; menu_init(&e, 0, 0); menu_key(&e, MK_TOGGLE); e.sel = 8;
    CHECK(menu_key(&e, MK_SELECT).action == MA_NONE && e.page == MP_MAIN, "an empty map list is not opened");
    Menu c; menu_init(&c, 500, -3);
    CHECK(c.count[MP_MAPS] == MENU_LIST_MAX && c.count[MP_ALTARS] == 0, "list sizes are clamped");
    // a long label is cut to the line size, never overflowing
    Menu b; menu_init(&b, 1, 0); menu_key(&b, MK_TOGGLE); b.sel = 8; menu_key(&b, MK_SELECT);
    menu_layout(&b, &st, t_list_text, lines, &sel);
    CHECK(strlen(lines[1]) < MENU_LINE_MAX, "line buffer bound holds");
}

static void test_menu_parse(void) {
    printf("-- M4.0 menu list parsing\n");
    char url[MENU_URL_MAX];
    CHECK(menu_parse_open_line("Open /Game/Maps/MainMap/MainMap_Main?listen\r\n", url, sizeof url) && strcmp(url, "/Game/Maps/MainMap/MainMap_Main?listen") == 0, "maps.txt line: Open <url>");
    CHECK(menu_parse_open_line("  Host console : Open /Game/Maps/MainMap/MainMap_Main?listen?CheckPoint=/Game/X.X:PersistentLevel.Altar_1?zone=0  ", url, sizeof url)
          && strcmp(url, "/Game/Maps/MainMap/MainMap_Main?listen?CheckPoint=/Game/X.X:PersistentLevel.Altar_1?zone=0") == 0, "altars.txt line: indent, Host console prefix, trailing blanks removed");
    CHECK(!menu_parse_open_line("These are root cooked maps found in the installed game's package list.", url, sizeof url), "prose is not a command");
    CHECK(!menu_parse_open_line("  ip.txt line 2: ?CheckPoint=/Game/X", url, sizeof url) && !menu_parse_open_line("Open nothing", url, sizeof url) && !menu_parse_open_line("", url, sizeof url) && !menu_parse_open_line(NULL, url, sizeof url), "other lines, empty and NULL are rejected");
    char tiny[20];
    CHECK(!menu_parse_open_line("Open /Game/Maps/MainMap/MainMap_Main?listen", tiny, sizeof tiny), "a URL that does not fit is rejected, never cut");
    CHECK(!menu_parse_open_line("Open /Game/", url, sizeof url), "a bare /Game/ is rejected");
}

static void *mk_conn(float avg, float best);
static void *mk_driver(void *world, void **conns, int nconn, void *server);
static void tel_paths(void);
static void test_menu_altar_line(void) {
    printf("-- M4.1 altar list, teleport, silent connection\n");
    char label[MENU_LINE_MAX]; float p[3];
    CHECK(menu_parse_altar_line("BP_RespawnAltar_03_E3Demo2_282\tBP_RespawnAltar_03_E3Demo_C\t6\t\t-40525\t-42916\t-7\tAutoSave\n", label, sizeof label, p)
          && strcmp(label, "BP_RespawnAltar_03_E3Demo2_282 (zone 6)") == 0 && p[0] == -40525.f && p[1] == -42916.f && p[2] == -7.f, "altar row with zone: label and coordinates");
    CHECK(menu_parse_altar_line("BP_RespawnAltar_03_E3Demo3\tBP_RespawnAltar_03_E3Demo_C\t\t\t-42901\t-23002\t-584\tAutoSave\r\n", label, sizeof label, p) && strcmp(label, "BP_RespawnAltar_03_E3Demo3") == 0 && p[2] == -584.f, "altar row without zone (empty field) and CRLF");
    CHECK(!menu_parse_altar_line("label\tclass\tzone\tsubzone\tx\ty\tz\troot\n", label, sizeof label, p), "the header row is skipped");
    CHECK(!menu_parse_altar_line("only\ttwo", label, sizeof label, p) && !menu_parse_altar_line("", label, sizeof label, p) && !menu_parse_altar_line(NULL, label, sizeof label, p), "short, empty and NULL lines are rejected");
    CHECK(!menu_parse_altar_line("A\tB\t1\t\t12abc\t5\t6\tR\n", label, sizeof label, p), "a coordinate with trailing junk is rejected");

    // teleport
    make_world(1, 0); reset_tel(); L_init(&API);
    void *pawn = mk_pawn(ROLE_AUTH, ROLE_SIM); void *root = mk(&C_object); PTR(pawn, OFF_ACTOR_ROOTCOMP) = root; PTR(W.localpc, OFF_CTRL_PAWN) = pawn;
    float to[3] = { 100.f, 200.f, 300.f }; g_nloc = 0;
    CHECK(L_teleport_local(to) == 1 && g_nloc == 1 && g_locobj[0] == pawn && g_loc[0][0] == 100.f && g_loc[0][2] == 300.f && logged("teleport:"), "host: the character is moved and it is logged");
    PTR(W.localpc, OFF_CTRL_PAWN) = NULL; g_nloc = 0;
    CHECK(L_teleport_local(to) == 0 && g_nloc == 0 && logged("no controllable character"), "no pawn: nothing is moved");
    PTR(W.localpc, OFF_CTRL_PAWN) = mk(&C_object);
    CHECK(L_teleport_local(to) == 0 && g_nloc == 0, "a pawn of the wrong class is never moved");
    make_world(0, 0); reset_tel(); L_init(&API);
    { void *jp = mk_pawn(ROLE_SIM, ROLE_AUTH); PTR(W.localpc, OFF_CTRL_PAWN) = jp; g_nloc = 0; }
    CHECK(L_is_joiner() && L_teleport_local(to) == 0 && g_nloc == 0 && logged("you are a joiner"), "joiner: refused (the host owns the position)");

    // silent connection
    make_world(1, 0); reset_tel(); tel_paths(); g_now = 1000; T_init(&API, &(TelCfg){ TEL_BASIC, 5000 }, 1);
    CHECK(T_quiet_ms(1000) == 0, "no net driver: 0");
    void *cc = mk_conn(0.05f, 0.03f); void **cl = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); cl[0] = cc;
    mk_driver(W.world, cl, 1, NULL);
    I32(cc, OFF_NC_INPPS) = 300;
    CHECK(T_quiet_ms(2000) == 0 && T_quiet_ms(5000) == 0, "packets arriving: never quiet");
    I32(cc, OFF_NC_INPPS) = 0;
    CHECK(T_quiet_ms(6000) == 1000 && T_quiet_ms(14000) == 9000, "no packets: quiet time grows from the last active moment");
    I32(cc, OFF_NC_INPPS) = 5;
    CHECK(T_quiet_ms(15000) == 0, "a packet resets it");
    void *srv = mk_conn(0.05f, 0.03f); I32(srv, OFF_NC_INPPS) = 0;
    mk_driver(W.world, NULL, 0, srv);
    CHECK(T_quiet_ms(16000) >= 0 && T_quiet_ms(30000) == 15000, "client side: the server connection is the one watched");
}

static void test_hostip(void) {
    printf("-- M4.2 host address\n");
    CHECK(hostip_valid("26.12.0.7") && hostip_valid("192.168.1.50") && hostip_valid("0.0.0.0") && hostip_valid("255.255.255.255"), "valid IPv4 addresses");
    CHECK(!hostip_valid("") && !hostip_valid(NULL) && !hostip_valid("1.2.3") && !hostip_valid("1.2.3.4.5") && !hostip_valid("256.1.1.1") && !hostip_valid("1..2.3") && !hostip_valid("a.b.c.d") && !hostip_valid("1.2.3.4 ") && !hostip_valid("1.2.3.1234") && !hostip_valid(" 1.2.3.4"), "invalid addresses are rejected");
    char o[64];
    CHECK(hostip_clean("  26.12.0.7 \r\n", o) && strcmp(o, "26.12.0.7") == 0, "blanks and CRLF are stripped");
    CHECK(hostip_clean("AUTO", o) && strcmp(o, "auto") == 0 && hostip_clean("auto\n", o), "auto in any case");
    CHECK(hostip_clean("\xEF\xBB\xBF" "26.12.0.7\r\n", o) && strcmp(o, "26.12.0.7") == 0, "UTF-8 BOM before the first line is skipped");
    CHECK(!hostip_clean("my radmin ip", o) && !hostip_clean("", o) && !hostip_clean("999.1.1.1", o) && !hostip_clean("autox", o), "junk lines are rejected");
    wchar_t out[64];
    CHECK(hostip_compose(out, 64, L"-NoEAC", "26.12.0.7") && wcscmp(out, L"-NoEAC -MULTIHOME=26.12.0.7") == 0, "address appended to the original command line");
    CHECK(hostip_compose(out, 64, L"-NoEAC", "auto") && wcscmp(out, L"-NoEAC") == 0 && hostip_compose(out, 64, L"-NoEAC", NULL) && wcscmp(out, L"-NoEAC") == 0, "auto / none = the original command line");
    wchar_t tiny[20];
    CHECK(!hostip_compose(tiny, 20, L"-NoEAC", "26.12.0.7"), "a result that does not fit is refused");
    CHECK(hostip_compose(tiny, 20, L"-NoEAC", "auto"), "the original fits");
    CHECK(hostip_url_is_listen(L"/Game/Maps/MainMap/MainMap_Main?listen") && hostip_url_is_listen(L"/Game/Maps/X?game=Y?LISTEN?z=1") && hostip_url_is_listen(L"/Game/Maps/MainMap/MainMap_Main?listen?CheckPoint=/Game/A?zone=0"), "?listen travels (any case, with options)");
    CHECK(!hostip_url_is_listen(L"192.168.1.50:7777") && !hostip_url_is_listen(L"/Game/Maps/MainMap/MainMap_Main") && !hostip_url_is_listen(NULL) && !hostip_url_is_listen(L"") && !hostip_url_is_listen(L"x?liste"), "joins, plain maps, NULL and cut-off text are not listen travels");
}

// ---- M4.2 NPC spawning -------------------------------------------------------------------------------------------------
static Cls C_aispawner, C_archetype, C_uclass, C_bpclass;
static void *g_np_load_class, *g_np_bad_class; static char g_np_load_path[300];
static int g_np_spawn_calls, g_np_can, g_np_makes, g_np_spawn_fail, g_np_repl_calls, g_np_arch_calls, g_np_unspawn_calls, g_np_wants_calls, g_np_load_calls;
static float g_np_last_loc[3]; static void *g_np_last_sp;
static void *g_np_last_cls, *g_np_load_result, *g_np_last_arch;
static void *f_np_cls_sp(void) { return &C_aispawner; }
static void *f_np_cls_arch(void) { return &C_archetype; }
static void *f_np_spawn(void *w, void *cls, const float loc[3]) {
    (void)w; g_np_spawn_calls++; g_np_last_cls = cls; memcpy(g_np_last_loc, loc, 12);
    if (g_np_spawn_fail) return NULL;
    void *sp = mk(&C_aispawner); U8(sp, OFF_ACTOR_ROLE) = ROLE_AUTH; g_np_last_sp = sp; U8(sp, OFF_SPAWNER_CANRESPAWN) = 1; U8(sp, OFF_SPAWNER_METHOD) = 0;
    ((WeakPtr *)((uint8_t *)sp + OFF_SPAWNER_AISPAWNED))->idx = -1;
    return sp;
}
static uint8_t f_np_can(void *sp) { (void)sp; return (uint8_t)g_np_can; }
static void f_np_wants(void *sp, void *arch) {
    g_np_wants_calls++; g_np_last_arch = arch;
    if (g_np_makes && (!g_np_bad_class || PTR(sp, OFF_SPAWNER_CLASS) != g_np_bad_class)) { void *n = mk(&C_char); f_weak_set((WeakPtr *)((uint8_t *)sp + OFF_SPAWNER_AISPAWNED), n); set_path(n, "/Game/Maps/X.X:PersistentLevel.BP_AICharacter_C_0"); }
}
static void f_np_unspawn(void *sp) {
    g_np_unspawn_calls++;
    void *n = f_weak_get((WeakPtr *)((uint8_t *)sp + OFF_SPAWNER_AISPAWNED));
    if (n) kill(n);
    ((WeakPtr *)((uint8_t *)sp + OFF_SPAWNER_AISPAWNED))->idx = -1;
}
static void f_np_repl(void *a, int on) { (void)a; (void)on; g_np_repl_calls++; }
static void f_np_setarch(void *sp, void *arch) { (void)sp; (void)arch; g_np_arch_calls++; }
static void *f_np_load(void *cls, const char *path) { g_np_load_calls++; snprintf(g_np_load_path, sizeof g_np_load_path, "%s", path); return cls == &C_archetype ? g_np_load_result : g_np_load_class; }
static const NpcApi NPCAPI = { f_np_cls_sp, f_np_cls_arch, f_np_spawn, f_np_can, f_np_wants, f_np_unspawn, f_np_repl, f_np_setarch, f_np_load };

static void *np_make_host(void) {         // a host world with a pawn at (1000, 2000, 300) facing +X, and an AIManager with a character class
    make_world(1, 0);
    void *pawn = mk_pawn(ROLE_AUTH, 0), *root = mk(&C_object);
    PTR(W.localpc, OFF_CTRL_PAWN) = pawn; PTR(pawn, OFF_ACTOR_ROOTCOMP) = root;
    F32(root, OFF_ROOT_C2W_POS - 16 + 12) = 1.f;                         // quaternion (0,0,0,1)
    F32(root, OFF_ROOT_C2W_POS) = 1000.f; F32(root, OFF_ROOT_C2W_POS + 4) = 2000.f; F32(root, OFF_ROOT_C2W_POS + 8) = 300.f;
    void *aim = mk(&C_object); PTR(aim, OFF_AIMGR_CLASSES) = &C_char;
    PTR(W.gi, OFF_GI_AIMANAGER) = aim;
    return pawn;
}
static void np_reset(void) {
    g_np_spawn_calls = g_np_repl_calls = g_np_arch_calls = g_np_unspawn_calls = g_np_wants_calls = g_np_load_calls = 0;
    g_np_can = 1; g_np_makes = 1; g_np_spawn_fail = 0; g_np_load_result = NULL; g_np_last_cls = g_np_last_arch = NULL; g_np_load_class = g_np_bad_class = NULL;
    g_nscan = 0; g_npaths = 0; reset_log(); L_npc_init(&NPCAPI); L_npc_set_replicated(0);
}

static void test_npc(void) {
    printf("-- M4.2 NPC spawning\n");
    L_init(&API);
    void *arch = mk(&C_archetype);
    const char *APATH = "/Game/DB/AI/NPCs/Trickster/Trickster.Trickster";

    // --- the plain case: archetype already in memory, host in an open world
    np_make_host(); np_reset();
    set_path(arch, APATH); g_scan[g_nscan++] = arch;
    CHECK(L_npc_spawn(APATH, "Trickster", NULL) == 1, "host: an NPC is spawned");
    CHECK(g_np_spawn_calls == 1 && g_np_last_cls == &C_aispawner, "one spawner actor of class AAISpawner is created");
    CHECK(g_np_last_loc[0] == 1350.f && g_np_last_loc[1] == 2000.f && g_np_last_loc[2] == 350.f, "first NPC appears 350 uu in front of the character, 50 above: %.1f %.1f %.1f", g_np_last_loc[0], g_np_last_loc[1], g_np_last_loc[2]);
    CHECK(g_np_wants_calls == 1 && g_np_last_arch == arch && g_np_arch_calls == 1 && g_np_load_calls == 0, "the loaded archetype is used, nothing is loaded from disk");
    CHECK(L_npc_live() == 1, "one NPC is tracked");
    CHECK(PTR(g_np_last_sp, OFF_SPAWNER_CLASS) == &C_char && U8(g_np_last_sp, OFF_SPAWNER_CANRESPAWN) == 0 && U8(g_np_last_sp, OFF_SPAWNER_METHOD) == 2, "spawner is set up: character class from the AIManager, no respawn, OnAnEvent");
    CHECK(g_np_repl_calls == 0, "the spawner stays local by default");
    CHECK(logged("npc: spawned") && logged("CanSpawn=1"), "the spawn is logged");

    // --- every spawn adds one more, 45 degrees further round
    CHECK(L_npc_spawn(APATH, "Trickster", NULL) == 1 && L_npc_live() == 2, "a second NPC is allowed (no limit)");
    CHECK(fabsf(g_np_last_loc[0] - (1000.f + 350.f * 0.70710678f)) < 0.5f && fabsf(g_np_last_loc[1] - (2000.f + 350.f * 0.70710678f)) < 0.5f, "the second NPC is 45 degrees round: %.1f %.1f", g_np_last_loc[0], g_np_last_loc[1]);

    // --- remove all: unspawn + destroy our spawners, nothing left
    int d0 = g_ndestroyed;
    CHECK(L_npc_remove_all() == 2 && g_np_unspawn_calls == 2 && g_ndestroyed - d0 == 2 && L_npc_live() == 0, "Remove all unspawns both NPCs and destroys both spawners");
    CHECK(L_npc_remove_all() == 0, "removing again does nothing");

    // --- archetype only on disk (duel maps do not load it)
    np_make_host(); np_reset();
    g_np_load_result = arch;
    CHECK(L_npc_spawn(APATH, "Trickster", NULL) == 1 && g_np_load_calls == 1 && logged("loaded from the game's files"), "an archetype that is not in memory is loaded");
    np_make_host(); np_reset();
    g_np_load_result = mk(&C_char);
    CHECK(L_npc_spawn(APATH, "x", NULL) == 0 && g_np_spawn_calls == 0 && logged("is not an archetype asset"), "a loaded object of the wrong class is refused");
    np_make_host(); np_reset();
    CHECK(L_npc_spawn(APATH, "x", NULL) == 0 && g_np_spawn_calls == 0 && logged("NOT FOUND"), "no archetype anywhere: nothing is spawned");

    // --- gates
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    PTR(W.world, OFF_W_AUTHGM) = NULL;
    CHECK(L_npc_spawn(APATH, "x", NULL) == 0 && g_np_spawn_calls == 0 && logged("only the host"), "a client of someone else's server cannot spawn");
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    PTR(W.localpc, OFF_CTRL_PAWN) = NULL;
    CHECK(L_npc_spawn(APATH, "x", NULL) == 0 && logged("no controllable character"), "no character: nothing is spawned");
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    CHECK(L_npc_spawn("", "x", NULL) == 0 && L_npc_spawn(NULL, "x", NULL) == 0 && g_np_spawn_calls == 0, "an empty archetype path is refused");

    // --- replicated spawner option
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    L_npc_set_replicated(1);
    CHECK(L_npc_spawn(APATH, "x", NULL) == 1 && g_np_repl_calls == 1 && logged("(replicated)"), "npcrepl = on replicates the spawner");

    // --- our spawner cannot be created / produces nothing -> fallback to the level's Calbot spawner
    np_make_host(); np_reset(); set_path(arch, APATH);
    void *cal = mk(&C_aispawner); U8(cal, OFF_ACTOR_ROLE) = ROLE_AUTH; set_path(cal, "/Game/Maps/MainMap/MainMap_HUB_LD.MainMap_HUB_LD:PersistentLevel.BP_AISpawner_Calbot");
    void *calroot = mk(&C_object); PTR(cal, OFF_ACTOR_ROOTCOMP) = calroot;
    F32(calroot, OFF_ROOT_C2W_POS) = -42465.f; F32(calroot, OFF_ROOT_C2W_POS + 4) = -22874.f; F32(calroot, OFF_ROOT_C2W_POS + 8) = -580.f;
    ((WeakPtr *)((uint8_t *)cal + OFF_SPAWNER_AISPAWNED))->idx = -1;
    g_scan[g_nscan++] = arch; g_scan[g_nscan++] = cal;
    g_np_spawn_fail = 1;
    CHECK(L_npc_spawn(APATH, "x", NULL) == 1 && g_np_wants_calls == 1 && g_nloc == 1 && g_locobj[0] == cal && g_loc[0][0] == 1350.f && logged("through the level's spawner"), "spawner creation failed: the level's Calbot spawner is moved next to you and used");
    CHECK(L_npc_live() == 1, "the fallback NPC is tracked");
    g_nloc = 0;
    CHECK(L_npc_remove_all() == 1 && g_np_unspawn_calls == 1 && g_nloc == 1 && g_locobj[0] == cal && g_loc[0][0] == -42465.f && g_loc[0][2] == -580.f, "Remove all unspawns it and puts the level's spawner back where it was");
    CHECK(!was_destroyed(cal), "the level's own spawner is never destroyed");

    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    g_np_makes = 0;
    int c0 = g_ndestroyed;
    CHECK(L_npc_spawn(APATH, "x", NULL) == 0 && g_ndestroyed - c0 == 1 && logged("no fallback spawner"), "no character and no fallback: the spawner we made is removed again, nothing is left behind");
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    g_np_can = 0;
    CHECK(L_npc_spawn(APATH, "x", NULL) == 0 && g_np_wants_calls == 0 && logged("CanSpawn=0"), "CanSpawn false: BPF_WantsSpawn is not called");

    // --- AIManager not there yet -> the level's spawner is used directly
    np_make_host(); np_reset(); set_path(arch, APATH);
    PTR(W.gi, OFF_GI_AIMANAGER) = NULL;
    set_path(cal, "/Game/Maps/MainMap/MainMap_HUB_LD.MainMap_HUB_LD:PersistentLevel.BP_AISpawner_Calbot");
    g_scan[g_nscan++] = arch; g_scan[g_nscan++] = cal;
    ((WeakPtr *)((uint8_t *)cal + OFF_SPAWNER_AISPAWNED))->idx = -1;
    CHECK(L_npc_spawn(APATH, "x", NULL) == 1 && g_np_spawn_calls == 0 && logged("AIManager missing"), "no AIManager: no own spawner is created, the level's one is used");
    L_npc_remove_all();

    // --- a world change invalidates everything: no use of freed objects
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    L_npc_spawn(APATH, "x", NULL);
    for (int i = 0; i < g_nw; i++) { void *o = g_w[i].obj; if (o && g_w[i].alive && PTR(o, OFF_OBJ_CLASS) == &C_aispawner) kill(o); }
    CHECK(L_npc_live() == 0, "after the world is gone the tracked NPC count drops to 0 without touching freed memory");
    CHECK(L_npc_remove_all() == 0, "Remove all after a world change cleans nothing and does not crash");

    // --- very many NPCs: the table of removable ones is bounded, spawning itself is not
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    int ok = 0;
    for (int i = 0; i < 270; i++) { if (i < 256) reset_log(); ok += L_npc_spawn(APATH, "x", NULL); }
    CHECK(ok == 270 && logged("can no longer be removed"), "270 NPCs can be spawned; the log says when the removable table is full");
    CHECK(L_npc_remove_all() == 256, "Remove all cleans the 256 it still tracks");
}

static const char *np_text(int page, int i) { static char b[32]; snprintf(b, sizeof b, "npc %d/%d", page, i); return b; }
static void test_npc_class(void) {
    printf("-- M4.3 NPC character class override\n");
    L_init(&API);
    C_bpclass.super = &C_uclass;
    *(void **)((uint8_t *)&C_char + OFF_OBJ_CLASS) = &C_uclass;           // the character class object's own class = UClass (what the game's m_AiClassToSpawn entry is)
    const char *APATH = "/Game/DB/AI/BOSSES/Kuretz/Level1/Lvl1_Kuretz_Phase1.Lvl1_Kuretz_Phase1";
    const char *CPATH = "/Game/Blueprints/AI/Boss/BP_AICharacter_Boss_Kuretz.BP_AICharacter_Boss_Kuretz_C";
    void *arch = mk(&C_archetype);

    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    void *bp = mk(&C_bpclass); set_path(bp, CPATH); g_scan[g_nscan++] = bp;
    CHECK(L_npc_spawn(APATH, "Kuretz", CPATH) == 1 && PTR(g_np_last_sp, OFF_SPAWNER_CLASS) == bp && g_np_load_calls == 0, "a requested class that is already loaded is used for the spawner");
    CHECK(logged("character class resolved") && logged("(requested)"), "the class choice is logged");

    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    g_np_load_class = bp;
    CHECK(L_npc_spawn(APATH, "Kuretz", CPATH) == 1 && PTR(g_np_last_sp, OFF_SPAWNER_CLASS) == bp && g_np_load_calls == 1 && strcmp(g_np_load_path, CPATH) == 0, "a class that is not in memory is loaded from the game's files");

    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch;
    CHECK(L_npc_spawn(APATH, "Kuretz", CPATH) == 1 && PTR(g_np_last_sp, OFF_SPAWNER_CLASS) == &C_char && logged("NOT FOUND - using the generic AI character"), "a class that does not exist falls back to the game's generic character class");

    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch; set_path(bp, CPATH); g_scan[g_nscan++] = bp;
    g_np_bad_class = bp;
    int d0 = g_ndestroyed;
    CHECK(L_npc_spawn(APATH, "Kuretz", CPATH) == 1 && g_np_spawn_calls == 2 && g_ndestroyed - d0 == 1 && PTR(g_np_last_sp, OFF_SPAWNER_CLASS) == &C_char && logged("with the requested class"), "a requested class that yields no character: its spawner is removed and the generic class is tried");
    CHECK(L_npc_live() == 1, "only the second NPC is tracked");

    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch; g_scan[g_nscan++] = bp;
    CHECK(L_npc_spawn(APATH, "x", "") == 1 && PTR(g_np_last_sp, OFF_SPAWNER_CLASS) == &C_char, "an empty class path = the generic class");
    np_make_host(); np_reset(); set_path(arch, APATH); g_scan[g_nscan++] = arch; g_scan[g_nscan++] = bp;
    PTR(W.gi, OFF_GI_AIMANAGER) = NULL;
    CHECK(L_npc_spawn(APATH, "x", CPATH) == 0 && g_np_spawn_calls == 0 && g_np_load_calls == 0, "without an AIManager (no UClass known) the class is not looked up; no spawner of ours is made");
}

static void test_npc_menu(void) {
    printf("-- M4.2 NPC menu + list\n");
    Menu m; menu_init_n(&m, 3, 40, 5); menu_key(&m, MK_TOGGLE);
    CHECK(m.count[MP_MAIN] == 12 && m.count[MP_NPCS] == 5, "main page has 12 entries, NPC page 5");
    m.sel = 10;
    MenuAct a = menu_key(&m, MK_SELECT);
    CHECK(a.action == MA_NONE && m.page == MP_NPCS && m.sel == 0 && m.open, "Spawn NPC... opens the NPC page");
    m.sel = 3;
    a = menu_key(&m, MK_SELECT);
    CHECK(a.action == MA_SPAWN_NPC && a.index == 3 && m.open && m.page == MP_NPCS, "Enter on an NPC spawns it and the menu STAYS open (spawn several in a row)");
    a = menu_key(&m, MK_BACK);
    CHECK(m.page == MP_MAIN && m.sel == 10 && m.open, "Left returns to the Spawn NPC entry");
    m.sel = 11; a = menu_key(&m, MK_SELECT);
    CHECK(a.action == MA_REMOVE_NPCS && m.open, "Remove all NPCs is an action on the main page");
    Menu e; menu_init(&e, 3, 40); menu_key(&e, MK_TOGGLE); e.sel = 10;
    a = menu_key(&e, MK_SELECT);
    CHECK(a.action == MA_NONE && e.page == MP_MAIN, "with no NPC list the entry does nothing");
    Menu big; menu_init_n(&big, 0, 0, 500);
    CHECK(big.count[MP_NPCS] == MENU_LIST_MAX, "a huge NPC list is clamped");

    char lines[MENU_LINES_MAX][MENU_LINE_MAX]; int sel = -1;
    MenuStatus st = { 1, 1, 1, 3, 40, "auto", 5, 2 };
    int n = menu_layout(&m, &st, NULL, lines, &sel);
    CHECK(n == MENU_MAIN_COUNT + 1 && strstr(lines[11], "Spawn NPC") && strstr(lines[11], "(5)") && strstr(lines[12], "Remove all NPCs") && strstr(lines[12], "2 spawned"), "labels show the type count and the live count: '%s' / '%s'", lines[11], lines[12]);
    st.is_host_world = 0;
    menu_layout(&m, &st, NULL, lines, &sel);
    CHECK(strstr(lines[11], "host only") != NULL, "a joiner sees 'host only'");
    Menu pg; menu_init_n(&pg, 0, 0, 3); menu_key(&pg, MK_TOGGLE); pg.sel = 10; menu_key(&pg, MK_SELECT);
    n = menu_layout(&pg, &st, np_text, lines, &sel);
    CHECK(strstr(lines[0], "NPCs") != NULL, "the NPC page has its own title");

    char grp[32], label[48], path[200], cls[140];
#define PARSE(line) menu_parse_npc_line(line, grp, 32, label, 48, path, 200, cls, 140)
    CHECK(PARSE("Open world\tTrickster\t/Game/DB/AI/NPCs/Trickster/Trickster.Trickster\r\n") && strcmp(grp, "Open world") == 0 && strcmp(label, "Trickster") == 0 && strcmp(path, "/Game/DB/AI/NPCs/Trickster/Trickster.Trickster") == 0 && cls[0] == 0, "npcs.txt: group, label, path");
    CHECK(PARSE("MiniBoss\tCity MiniBoss\t/Game/DB/AI/NPCs/Pier_MiniBoss.Pier_MiniBoss\t/Game/Blueprints/AI/BP_AICharacter_MiniBoss.BP_AICharacter_MiniBoss_C\n") && strcmp(grp, "MiniBoss") == 0 && strcmp(cls, "/Game/Blueprints/AI/BP_AICharacter_MiniBoss.BP_AICharacter_MiniBoss_C") == 0, "npcs.txt: optional character class");
    CHECK(PARSE("Grunt v1  \t  /Game/DB/AI/NPCs/Grunt/Grunt_v1.Grunt_v1  \n") && strcmp(grp, "Other") == 0 && strcmp(label, "Grunt v1") == 0 && strcmp(path, "/Game/DB/AI/NPCs/Grunt/Grunt_v1.Grunt_v1") == 0, "npcs.txt: the old two-column format still works (group Other)");
    CHECK(PARSE("G\tL\t/Game/X.X\tnot-a-class\n") && cls[0] == 0, "npcs.txt: a fourth column that is not a /Game/ path is ignored");
    CHECK(!PARSE("# comment\n") && !PARSE("\n") && !PARSE("no tab here /Game/X.X\n") && !PARSE("Label\tnot-a-game-path\n") && !PARSE("\t/Game/X.X\n") && !PARSE("G\t\t/Game/X.X\n"), "npcs.txt: comments, blank lines and malformed lines are skipped");
    CHECK(!PARSE("G\tA very long label that cannot possibly fit into the forty-eight byte buffer\t/Game/X.X\n") && !PARSE("A group name that is longer than thirty-one characters\tL\t/Game/X.X\n") && !PARSE("G\tL\t/Game/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.X\n"), "npcs.txt: a group, label or path that does not fit is refused, not cut");

    // --- groups: group list -> group page -> spawn index, Left walks back
    Menu gm; menu_init_n(&gm, 0, 0, 7); int gc[3] = { 2, 3, 2 }; menu_set_npc_groups(&gm, 3, gc); menu_key(&gm, MK_TOGGLE);
    CHECK(gm.ngroups == 3 && gm.count[MP_NPCGROUPS] == 3 && gm.gstart[1] == 2 && gm.gstart[2] == 5, "groups: contiguous ranges (starts %d %d %d)", gm.gstart[0], gm.gstart[1], gm.gstart[2]);
    gm.sel = 10; a = menu_key(&gm, MK_SELECT);
    CHECK(a.action == MA_NONE && gm.page == MP_NPCGROUPS && gm.sel == 0, "Spawn NPC... opens the group list when there are groups");
    menu_key(&gm, MK_DOWN); a = menu_key(&gm, MK_SELECT);
    CHECK(a.action == MA_NONE && gm.page == MP_NPCS && gm.count[MP_NPCS] == 3 && gm.sel == 0 && gm.group == 1, "Enter on a group opens its NPC page with that group's count");
    menu_key(&gm, MK_DOWN); a = menu_key(&gm, MK_SELECT);
    CHECK(a.action == MA_SPAWN_NPC && a.index == 3 && gm.open && gm.page == MP_NPCS, "the spawn index is the group's start plus the selection (2 + 1)");
    menu_key(&gm, MK_BACK);
    CHECK(gm.page == MP_NPCGROUPS && gm.sel == 1, "Left from a group's NPC page goes back to the group list on that group");
    menu_key(&gm, MK_BACK);
    CHECK(gm.page == MP_MAIN && gm.sel == 10, "Left from the group list goes back to Spawn NPC...");
    menu_key(&gm, MK_SELECT); menu_key(&gm, MK_UP); menu_key(&gm, MK_SELECT); menu_key(&gm, MK_SELECT);
    CHECK(gm.page == MP_NPCS && gm.group == 2 && gm.count[MP_NPCS] == 2, "Up wraps to the last group");
    Menu gz; menu_init_n(&gz, 0, 0, 3); int none_counts[1] = { 0 }; menu_set_npc_groups(&gz, 0, none_counts);
    CHECK(gz.ngroups == 0 && gz.count[MP_NPCGROUPS] == 0, "no groups: flat list as before");
    Menu gt; menu_init_n(&gt, 0, 0, 30); int many[20]; for (int q = 0; q < 20; q++) many[q] = 1; menu_set_npc_groups(&gt, 20, many);
    CHECK(gt.ngroups == 0, "more groups than the menu has room for: groups are switched off, nothing overflows");
    char gl[MENU_LINES_MAX][MENU_LINE_MAX]; int gsel = -1;
    n = menu_layout(&gm, &st, np_text, gl, &gsel);
    gm.page = MP_NPCS; gm.group = 1; gm.sel = 0; gm.count[MP_NPCS] = 3;
    n = menu_layout(&gm, &st, np_text, gl, &gsel);
    CHECK(strstr(gl[0], "NPCs:") && strstr(gl[0], "npc 4/1"), "the NPC page title names its group: '%s'", gl[0]);
}

static void test_m43(void) {
    printf("-- M4.3 stale pawn sweep + duel score filter\n");
    // --- leave + rejoin without a world reload: the previous session's character is Role=Authority again, but nobody controls it any more
    make_world(0, 0);
    void *nw = mk_pawn(ROLE_SIM, ROLE_AUTH);                            // our current (joined) pawn
    PTR(W.localpc, OFF_CTRL_PAWN) = nw;
    void *stale = mk_pawn(ROLE_AUTH, ROLE_AUTO);                        // previous session: Role 3, RemoteRole 2, controller gone
    void *hostpawn = mk_pawn(ROLE_SIM, ROLE_AUTH);                      // the host's character as the server replicates it: Role 1, no controller here
    void *aipawn = mk_pawn(ROLE_AUTH, ROLE_SIM); PTR(aipawn, OFF_PAWN_CONTROLLER) = mk(&C_controller);   // a local-only AI character with its own controller
    g_scan[0] = nw; g_scan[1] = stale; g_scan[2] = hostpawn; g_scan[3] = aipawn; g_nscan = 4;
    reset_log();
    CHECK(L_sweep_joiner_clones() == 1 && was_destroyed(stale), "the controller-less Role=Authority pawn left from the previous session is removed");
    CHECK(!was_destroyed(nw) && !was_destroyed(hostpawn) && !was_destroyed(aipawn), "our pawn, the host's replicated pawn and a controlled local AI character are kept");
    g_nscan = 0;

    // --- the duel score filter: only a real player's PlayerState is a kill victim
    make_world(1, 0);
    void *ps1 = mk(&C_ps), *ps2 = mk(&C_ps), *aips = mk(&C_ps);
    PTR(W.localpc, OFF_CTRL_PLAYERSTATE) = ps1;
    void *jpc = mk_joiner_pc(mk_pawn(ROLE_AUTH, ROLE_AUTO), ps2); add_pc_to_list(jpc);
    CHECK(L_ps_is_player(ps1) == 1 && L_ps_is_player(ps2) == 1, "the host's and a joiner's PlayerStates are players");
    CHECK(L_ps_is_player(NULL) == 0, "a NULL victim (an NPC died) is not a player");
    CHECK(L_ps_is_player(aips) == 0, "a PlayerState that no player controller owns is not a player");
    kill(jpc);
    CHECK(L_ps_is_player(ps2) == 0 && L_ps_is_player(ps1) == 1, "a disconnected player's PlayerState stops counting, no use of the freed controller");
}

static void test_config(void) {
    printf("-- M4.1.3 config.txt\n");
    char v[64];
    const char *cfg = "\xEF\xBB\xBF# comment\r\nParryFix = off\r\n\r\n  telemetry=verbose 10  \r\nhostip = auto\r\nhostip = 26.12.0.7\r\n#perteam = 2\r\njunk line\r\nmenu\r\n";
    CHECK(cfg_get(cfg, "parryfix", 0, v, sizeof v) && strcmp(v, "off") == 0, "key lookup is case-insensitive and skips a BOM and comments");
    CHECK(cfg_get(cfg, "telemetry", 0, v, sizeof v) && strcmp(v, "verbose 10") == 0, "no spaces around = and trailing blanks are trimmed; values may have several words");
    CHECK(cfg_get(cfg, "hostip", 0, v, sizeof v) && strcmp(v, "auto") == 0 && cfg_get(cfg, "hostip", 1, v, sizeof v) && strcmp(v, "26.12.0.7") == 0 && !cfg_get(cfg, "hostip", 2, v, sizeof v), "a repeated key is read in order");
    CHECK(!cfg_get(cfg, "perteam", 0, v, sizeof v) && !cfg_get(cfg, "menu", 0, v, sizeof v) && !cfg_get(cfg, "parry", 0, v, sizeof v), "commented-out, value-less and partial keys are absent");
    CHECK(cfg_get("autoleave =\n", "autoleave", 0, v, sizeof v) && v[0] == 0, "an empty value is found but empty");
    char cut4[4];
    CHECK(cfg_get("telemetry = verbose\n", "telemetry", 0, cut4, sizeof cut4) && strcmp(cut4, "ver") == 0, "an over-long value is cut to the buffer, NUL-terminated");
    CHECK(cfg_nth_line("# x\r\n\r\non\r\n26.1.1.1\r\n", 0, v, sizeof v) && strcmp(v, "on") == 0 && cfg_nth_line("# x\r\n\r\non\r\n26.1.1.1\r\n", 1, v, sizeof v) && strcmp(v, "26.1.1.1") == 0 && !cfg_nth_line("# x\n", 0, v, sizeof v) && !cfg_nth_line("", 0, v, sizeof v), "old single-setting files: nth non-blank non-comment line");
}

static void test_match_probe(void) {
    printf("-- duel match probe\n");
    TelCfg verbose = { TEL_VERBOSE, 1000 };
    make_world(1, 0); reset_tel(); tel_paths(); g_nfn = 0;
    set_fn(0xAA, "WaitingToStart"); set_fn(0xBB, "InitializingPlayers");
    void *gm = mk(&C_advgm), *gs = mk(&C_advgs);
    PTR(W.world, OFF_W_AUTHGM) = gm; PTR(W.world, OFF_W_GAMESTATE) = gs;
    *(uint64_t *)((uint8_t *)gm + OFF_GM_MATCHSTATE) = 0xAA; *(uint64_t *)((uint8_t *)gs + OFF_ADVGS_CACHESTATE) = 0xAA;
    I32(gm, OFF_GM_NUMPLAYERS) = 2; U8(gm, OFF_ADVGM_NBPLAYERS) = 1; U8(gm, OFF_ADVGM_NBCLIENTS) = 0; U8(gm, OFF_ADVGM_READYWAIT) = 1; F32(gm, OFF_ADVGM_TIMEOUT) = 12.5f;
    U8(gs, OFF_ADVGS_EXPECTED) = 1; U8(gs, OFF_ADVGS_CONNECTED) = 1; I32(gs, OFF_GS_PLAYERARRAY + 8) = 2; U8(gs, OFF_ADVGS_VALID) = 1;
    void *pc1 = mk(&C_fpc), *pc2 = mk(&C_fpc), *pc3 = mk(&C_fpc), *ps1 = mk(&C_advps);
    PTR(pc1, OFF_PC_PLAYER) = W.lp; PTR(pc1, OFF_CTRL_PLAYERSTATE) = ps1; U8(ps1, OFF_ADVPS_ANSWER) = 1;
    U8(pc1, OFF_FPC_PAWNINIT) = 1; U8(pc1, OFF_FPC_GAMEINIT) = 0; U8(pc1, OFF_FPC_LOADINGIDX) = 3; U8(pc1, OFF_FPC_WAITFOR) = 2;
    PTR(pc2, OFF_PC_PLAYER) = mk(&C_netconn);                                   // remote player, no player state yet
    PTR(pc3, OFF_PC_PLAYER) = NULL;                                             // a disconnected controller still in the list
    add_pc_to_list(pc1); add_pc_to_list(pc2); add_pc_to_list(pc3);
    g_now = 1000; T_init(&API, &verbose, 1); T_tick(g_now);
    CHECK(logged("match: server state=WaitingToStart numPlayers=2 nbPlayers=1 nbClients=0 readyWait=1 timeout=12.5s"), "server side: state name and the mode's counters are logged");
    CHECK(logged("cache=WaitingToStart expected=1 connected=1 playerArray=2 valid=1"), "game state side: expected vs connected vs the real player array (the offline mismatch)");
    CHECK(logged("pc1[local pawnInit=1 gameInit=0 loadingIdx=3 waitFor=2 answer=1 team="), "the local controller's loading/answer state and team are logged");
    CHECK(logged("pc2[remote") && logged("answer=-1 team=-1]"), "a remote controller without an adversarial player state is logged with answer -1");
    CHECK(!logged("pc3["), "a disconnected controller (Player null) is never read");
    int c = count_logged("match:");
    g_now = 1600; T_tick(g_now); g_now = 2200; T_tick(g_now);
    CHECK(count_logged("match:") == c, "nothing is logged while the state is unchanged");
    g_now = 12000; T_tick(g_now);
    CHECK(logged("match: unchanged for 10 s"), "a heartbeat shows the state is stuck");
    *(uint64_t *)((uint8_t *)gm + OFF_GM_MATCHSTATE) = 0xBB; g_now = 13000; T_tick(g_now);
    CHECK(logged("state=InitializingPlayers"), "a state change is logged");
    PTR(W.world, OFF_W_AUTHGM) = NULL; g_now = 14000; T_tick(g_now);
    CHECK(logged("match: client state=-"), "a client (no game mode object) logs the replicated game state only");
    PTR(W.world, OFF_W_GAMESTATE) = mk(&C_gs); g_now = 15000; T_tick(g_now);
    CHECK(logged("no duel game state any more"), "leaving the duel is logged once");
    PTR(W.world, OFF_W_GAMESTATE) = mk(&C_object); PTR(W.world, OFF_W_AUTHGM) = NULL; g_now = 16000; T_tick(g_now);
    CHECK(count_logged("no duel game state any more") == 1, "a non-duel game state is never read as a duel one");
}

static void test_pvp_result(void) {
    printf("-- duel result (backend verdict byte)\n");
    const char *duel = "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum";
    // a client (no game mode object) in a duel map: game state, binder and a local player state with team 1
    make_world(0, 0); reset_tel(); set_path(W.world, duel);
    void *gs = mk(&C_advgs), *binder = mk(&C_object), *ps = mk(&C_advps);
    PTR(W.world, OFF_W_GAMESTATE) = gs; PTR(W.gi, OFF_GI_BINDER) = binder;
    I32(ps, OFF_ADVPS_TEAM) = 1; PTR(W.localpc, OFF_CTRL_PLAYERSTATE) = ps;
    uint64_t *cache = (uint64_t *)((uint8_t *)gs + OFF_ADVGS_CACHESTATE_L);
    *cache = 0xAA; I32(gs, OFF_ADVGS_WINNER) = -1; L_init(&API);
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 0, "match in progress: the verdict byte is not touched");
    *cache = 0xCC;
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 0, "post-match but no winner team yet (-1): not touched");
    I32(gs, OFF_ADVGS_WINNER) = 1;
    CHECK(L_pvp_result(1) == 1 && U8(binder, OFF_BINDER_RESULT) == 1 && logged("verdict set to 1 (victory)"), "local team is the winner team: verdict 1 (victory) and it is logged");
    int logs = g_nlog; L_pvp_result(1); L_pvp_result(1); L_pvp_result(1);
    CHECK(g_nlog == logs && U8(binder, OFF_BINDER_RESULT) == 1, "later frames log nothing and keep the byte");
    *cache = 0xAA;
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 0 && logged("put back to 0"), "when the post-match state ends the byte is put back to 0");
    // the other player won: defeat
    I32(ps, OFF_ADVPS_TEAM) = 0; *cache = 0xCC;
    CHECK(L_pvp_result(1) == 1 && U8(binder, OFF_BINDER_RESULT) == 2 && logged("(defeat)"), "local team is not the winner team: verdict 2 (defeat)");
    CHECK(L_pvp_result(0) == 0 && U8(binder, OFF_BINDER_RESULT) == 0, "switched off while holding: the byte is restored");
    // a byte that already has a value (a real verdict, or someone else's) is never overwritten or cleared
    L_init(&API); U8(binder, OFF_BINDER_RESULT) = 3;
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 3, "an existing non-zero verdict is left alone");
    *cache = 0xAA; L_pvp_result(1);
    CHECK(U8(binder, OFF_BINDER_RESULT) == 3, "and is not cleared afterwards (it was not ours)");
    // someone changes it while we hold it: we let go without touching it
    L_init(&API); U8(binder, OFF_BINDER_RESULT) = 0; *cache = 0xCC; L_pvp_result(1); U8(binder, OFF_BINDER_RESULT) = 7;
    L_pvp_result(1); *cache = 0xAA; L_pvp_result(1);
    CHECK(U8(binder, OFF_BINDER_RESULT) == 7 && logged("changed to 7"), "a byte changed by something else is neither overwritten nor cleared");
    // wrong place / wrong objects
    L_init(&API); U8(binder, OFF_BINDER_RESULT) = 0; *cache = 0xCC; set_path(W.world, "/Game/Maps/MainMap/MainMap_Main.MainMap_Main");
    L_init(&API);
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 0, "a non-duel map never gets a verdict");
    reset_tel(); set_path(W.world, duel);
    void *badgs = mk(&C_gs);                                                   // wrong class, but fields that WOULD trigger a write if it were trusted
    *(uint64_t *)((uint8_t *)badgs + OFF_ADVGS_CACHESTATE_L) = 0xCC; I32(badgs, OFF_ADVGS_WINNER) = 1;
    PTR(W.world, OFF_W_GAMESTATE) = badgs; L_init(&API);
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 0, "a game state of the wrong class is never read as a duel game state");
    PTR(W.world, OFF_W_GAMESTATE) = gs; PTR(W.gi, OFF_GI_BINDER) = NULL; L_init(&API);
    CHECK(L_pvp_result(1) == 0, "no binder object: nothing is written");
    PTR(W.gi, OFF_GI_BINDER) = binder; PTR(W.localpc, OFF_CTRL_PLAYERSTATE) = mk(&C_ps); L_init(&API);
    CHECK(L_pvp_result(1) == 0 && U8(binder, OFF_BINDER_RESULT) == 0, "a local player state that is not an adversarial one gives no team: nothing is written");
    g_world = NULL; CHECK(L_pvp_result(1) == 0, "no world (loading): nothing happens");
}
static void test_parry_inspect(void) {
    printf("-- parry observer reads the PARRIER's defense component\n");
    char t[420];
    void *parrier = mk_pawn(ROLE_AUTH, ROLE_AUTO), *def = mk(&C_defense), *db = mk(&C_object);
    PTR(parrier, OFF_CHAR_DEFENSE) = def;
    // NotifyParrySuccessful's `this` is the attacker's component: its pointers are null (this is what the M3.5 log looked at). The parrier holds the value.
    f_weak_set((WeakPtr *)((uint8_t *)def + OFF_DEF_PARRYPROPERTY), db);
    CHECK(L_parry_inspect(parrier, 1, t, sizeof t) == 1 && strstr(t, "rewards will be given") && strstr(t, "reads +0x1F4"), "fix on, parrier +0x1F4 valid: rewards given: %s", t);
    CHECK(L_parry_inspect(parrier, 0, t, sizeof t) == 0 && strstr(t, "SKIPPED") && strstr(t, "reads +0x1FC"), "fix off, only +0x1F4 valid: the stock read of +0x1FC drops the rewards (the bug): %s", t);
    f_weak_set((WeakPtr *)((uint8_t *)def + OFF_DEF_PARRYPROPERTY_PRED), db);
    CHECK(L_parry_inspect(parrier, 0, t, sizeof t) == 1, "fix off, +0x1FC valid (second parry): given");
    WeakPtr *cur = (WeakPtr *)((uint8_t *)def + OFF_DEF_PARRYPROPERTY); f_weak_set(cur, NULL);
    CHECK(L_parry_inspect(parrier, 1, t, sizeof t) == 0 && strstr(t, "SKIPPED"), "fix on, +0x1F4 null: reported as skipped: %s", t);
    f_weak_set(cur, db); kill(db);
    CHECK(L_parry_inspect(parrier, 1, t, sizeof t) == 0, "a weak pointer to a destroyed object counts as null (no dangling read)");
    CHECK(L_parry_inspect(NULL, 1, t, sizeof t) == -1 && strstr(t, "no parrier"), "no parrier actor: not inspected");
    CHECK(L_parry_inspect(mk(&C_object), 1, t, sizeof t) == -1 && strstr(t, "not an AFightingCharacter"), "a non-character is never read as one");
    void *bare = mk_pawn(ROLE_AUTH, ROLE_AUTO);
    CHECK(L_parry_inspect(bare, 1, t, sizeof t) == -1 && strstr(t, "no defense component"), "a character without a defense component: not inspected");
    void *odd = mk_pawn(ROLE_AUTH, ROLE_AUTO); PTR(odd, OFF_CHAR_DEFENSE) = mk(&C_object);
    CHECK(L_parry_inspect(odd, 1, t, sizeof t) == -1 && strstr(t, "unexpected class"), "a component of the wrong class is never read as a defense component");
}

static void test_parry_log(void) {
    printf("-- parry observer text\n");
    char t[300];
    CHECK(parry_describe(1, 1, 0, t, sizeof t) == 1 && strstr(t, "reads +0x1F4") && strstr(t, "rewards will be given"), "fix on, +0x1F4 valid: rewards given even though +0x1FC is null: %s", t);
    CHECK(parry_describe(1, 0, 1, t, sizeof t) == 0 && strstr(t, "SKIPPED"), "fix on, +0x1F4 null: reported as skipped: %s", t);
    CHECK(parry_describe(0, 1, 0, t, sizeof t) == 0 && strstr(t, "reads +0x1FC") && strstr(t, "SKIPPED"), "fix off, +0x1FC null: the Forsaken bug is reported: %s", t);
    CHECK(parry_describe(0, 0, 1, t, sizeof t) == 1 && strstr(t, "given"), "fix off, +0x1FC valid (a second parry): rewards given: %s", t);
    volatile int off_cur = OFF_DEF_PARRYPROPERTY, off_pred = OFF_DEF_PARRYPROPERTY_PRED;
    CHECK(off_cur == 0x1F4 && off_pred == 0x1FC, "offsets match the parry findings document");
}

#define F32(p, off) (*(float *)((uint8_t *)(p) + (off)))
static void reset_tel(void) { reset_log(); memset(g_ncsv, 0, sizeof g_ncsv); g_npaths = 0; }

static void test_telemetry_config(void) {
    printf("-- telemetry config and caller naming\n");
    TelCfg c;
    CHECK(tel_parse_config("off\n", &c) && c.level == TEL_OFF && c.interval_ms == 0, "off");
    CHECK(tel_parse_config("basic", &c) && c.level == TEL_BASIC && c.interval_ms == 5000, "basic defaults to 5 s");
    CHECK(tel_parse_config("VERBOSE 3", &c) && c.level == TEL_VERBOSE && c.interval_ms == 3000, "case-insensitive, explicit seconds");
    CHECK(tel_parse_config("verbose", &c) && c.interval_ms == 1000, "verbose defaults to 1 s");
    CHECK(tel_parse_config("basic 500", &c) && c.interval_ms == 60000, "interval clamped to 60 s");
    CHECK(tel_parse_config("basic 0", &c) && c.interval_ms == 1000, "interval floor is 1 s");
    CHECK(tel_parse_config("  \r\nbasic 10\r\n", &c) && c.interval_ms == 10000, "leading blank space and CRLF are tolerated");
    CHECK(!tel_parse_config("loud", &c) && c.level == TEL_BASIC && c.interval_ms == 5000, "unknown word falls back to basic/5 s");
    CHECK(!tel_parse_config(NULL, &c) && c.level == TEL_BASIC, "missing text falls back to basic");
    char who[64];
    tel_describe_caller(0x7FF600001000ull, 0x7FF600000000ull, 0x4000000ull, 0x7FF700000000ull, 0x30000ull, who, sizeof who);
    CHECK(strcmp(who, "exe+0x1000") == 0, "caller in the game image is named by RVA: %s", who);
    tel_describe_caller(0x7FF700000100ull, 0x7FF600000000ull, 0x4000000ull, 0x7FF700000000ull, 0x30000ull, who, sizeof who);
    CHECK(strcmp(who, "LanNative") == 0, "caller inside our own DLL: %s", who);
    tel_describe_caller(0x7FFA00000000ull, 0x7FF600000000ull, 0x4000000ull, 0x7FF700000000ull, 0x30000ull, who, sizeof who);
    CHECK(strncmp(who, "other(", 6) == 0, "caller elsewhere: %s", who);
    tel_describe_caller(0x7FF600001000ull, 0x7FF600000000ull, 0, 0, 0, who, sizeof who);
    CHECK(strncmp(who, "other(", 6) == 0, "unknown image sizes never claim a match: %s", who);
}

static void *mk_player_pawn(void *ctrl, float x, float y, float z, int ping_quarter, int zone) {
    void *p = mk_pawn(ROLE_AUTH, ROLE_AUTO), *ps = mk(&C_ps), *root = mk(&C_object);
    PTR(p, OFF_PAWN_PLAYERSTATE) = ps; U8(ps, OFF_PS_PING) = (uint8_t)ping_quarter; U8(ps, OFF_PS_RESPAWNZONE) = (uint8_t)zone;
    PTR(p, OFF_ACTOR_ROOTCOMP) = root; float pos[3] = { x, y, z }; memcpy((uint8_t *)root + OFF_ROOT_C2W_POS, pos, 12);
    PTR(p, OFF_PAWN_CONTROLLER) = ctrl;
    return p;
}
static void set_pawn_list(void *world, void **pawns, int n) {
    WeakPtr *list = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < n; i++) f_weak_set(&list[i], pawns[i]);
    PTR(world, OFF_W_PAWNLIST) = list; I32(world, OFF_W_PAWNLIST + 8) = n;
}
static void *mk_conn(float avg, float best) {
    void *c = mk(&C_netconn_cls);
    I32(c, OFF_NC_STATE) = 3; F32(c, OFF_NC_AVGLAG) = avg; F32(c, OFF_NC_BESTLAG) = best;
    I32(c, OFF_NC_INPPS) = 30; I32(c, OFF_NC_OUTPPS) = 31; I32(c, OFF_NC_INBPS) = 4000; I32(c, OFF_NC_OUTBPS) = 5000;
    I32(c, OFF_NC_INLOST) = 1; I32(c, OFF_NC_OUTLOST) = 2; I32(c, OFF_NC_INPKTS) = 100; I32(c, OFF_NC_OUTPKTS) = 200;
    I32(c, OFF_NC_INBYTES) = 9000; I32(c, OFF_NC_OUTBYTES) = 8000; I32(c, OFF_NC_MAXPACKET) = 1024;
    return c;
}
static void *mk_driver(void *world, void **conns, int nconn, void *server) {
    void *nd = mk(&C_netdriver);
    PTR(world, OFF_W_NETDRIVER) = nd; PTR(nd, OFF_ND_SERVERCONN) = server;
    PTR(nd, OFF_ND_CLIENTCONNS) = conns; I32(nd, OFF_ND_CLIENTCONNS + 8) = nconn;
    U32(nd, OFF_ND_INBPS) = 4000; U32(nd, OFF_ND_OUTBPS) = 5000; U32(nd, OFF_ND_INPKTLOST) = 1; U32(nd, OFF_ND_OUTPKTLOST) = 2;
    return nd;
}
static void tel_paths(void) {
    set_path(W.world, "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum");
    set_path(&C_gi, "/Game/Blueprints/GameModes/1V1/1V1_GameMode.1V1_GameMode_C");   // class of the fake "authority game mode"
    set_path(&C_gs, "/Script/Absolver.ThePlainesGameState");
}

static void test_telemetry(void) {
    printf("-- telemetry\n");
    TelCfg verbose = { TEL_VERBOSE, 1000 }, off = { TEL_OFF, 0 };

    // a world with no network driver: world events only, never any snapshot rows
    make_world(1, 0); reset_tel(); tel_paths(); g_now = 1000; T_init(&API, &verbose, 4242);
    T_tick(g_now);
    CHECK(logged("world: map=/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum role=standalone"), "world change logs the loaded map and role");
    CHECK(logged("gamemode=/Game/Blueprints/GameModes/1V1/1V1_GameMode.1V1_GameMode_C"), "the game-mode CLASS path is logged (the string the ?game= option needs)");
    CHECK(logged("gamestate=/Script/Absolver.ThePlainesGameState"), "game state class is logged");
    int worlds = count_logged("world:");
    g_now = 1600; T_tick(g_now); g_now = 2200; T_tick(g_now); g_now = 5000; T_tick(g_now);
    CHECK(count_logged("world:") == worlds, "no repeated world line while nothing changed");
    CHECK(g_ncsv[0] + g_ncsv[1] + g_ncsv[2] == 0, "no snapshot rows outside a network session");
    CHECK(T_peers_now() == 0, "T_peers_now is 0 in a world without a net driver");
    PTR(W.world, OFF_W_GAMESTATE) = mk(&C_gs); g_now = 6000; T_tick(g_now);
    CHECK(count_logged("world:") == worlds + 1, "a new game state (a map load) is logged again");
    void *real_world = W.world; g_world = NULL; g_now = 7000; T_tick(g_now);
    CHECK(logged("world: none"), "world gone (loading) is logged");
    g_world = real_world; g_now = 8000; T_tick(g_now);
    CHECK(count_logged("world:") == worlds + 3, "world coming back is logged");

    // listen server with one connected client, a local pawn, a remote player pawn and an NPC
    make_world(1, 3); reset_tel(); tel_paths();
    void *client_conn = mk_conn(0.05f, 0.03f);
    void **conns = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); conns[0] = client_conn;
    mk_driver(W.world, conns, 1, NULL);
    void *host_pawn = mk_player_pawn(W.localpc, 10.f, 20.f, 30.f, 20, 4);
    void *joiner_pc = mk_joiner_pc(NULL, mk(&C_ps));
    void *joiner_pawn = mk_player_pawn(joiner_pc, -5.f, 6.f, 7.f, 50, 4);
    void *npc = mk_pawn(ROLE_AUTH, ROLE_SIM);                         // no PlayerState: counted only
    PTR(W.localpc, OFF_CTRL_PAWN) = host_pawn;
    void *pawns[3] = { host_pawn, joiner_pawn, npc }; set_pawn_list(W.world, pawns, 3);
    g_now = 10000; T_init(&API, &verbose, 4242);
    T_frame(0.010f); T_frame(0.030f);
    T_tick(g_now);
    CHECK(logged("role=host") && logged("netdriver=yes"), "listen server is a host with a net driver");
    CHECK(logged("session: started role=host peers=1"), "session start is logged when the first client connects");
    CHECK(T_peers_now() == 1, "T_peers_now counts the host's client connections (%d)", T_peers_now());
    CHECK(g_ncsv[1] == 1, "one connection row, got %d", g_ncsv[1]);
    CHECK(g_ncsv[1] == 1 && strstr(g_csv[1][0], ",3,50.0,30.0,30,31,4000,5000,1,2,100,200,9000,8000,1024") != NULL, "connection row carries lag and packet counters: %s", g_ncsv[1] ? g_csv[1][0] : "(none)");
    static const char row_start[] = "2000-01-01 00:00:00.000,10000,4242,host,OPTI_GM_1v1_City_Coliseum,0,0x";
    CHECK(g_ncsv[1] == 1 && strncmp(g_csv[1][0], row_start, sizeof row_start - 1) == 0, "rows start with wall time, tick, pid, role and short map: %s", g_ncsv[1] ? g_csv[1][0] : "(none)");
    CHECK(g_ncsv[2] == 2, "two player rows (the NPC is only counted), got %d", g_ncsv[2]);
    CHECK(g_ncsv[2] == 2 && strstr(g_csv[2][0], ",1,1,3,2,10.0,20.0,30.0,80,4") != NULL, "host pawn row: local, has controller, position, ping 20*4 ms, zone: %s", g_ncsv[2] ? g_csv[2][0] : "(none)");
    CHECK(g_ncsv[2] == 2 && strstr(g_csv[2][1], ",0,1,3,2,-5.0,6.0,7.0,200,4") != NULL, "remote pawn row is not local: %s", g_ncsv[2] > 1 ? g_csv[2][1] : "(none)");
    CHECK(g_ncsv[0] == 1 && strstr(g_csv[0][0], ",2,20.00,30.00,4000,5000,1,2,1,2,1") != NULL, "frame row: count, avg and max ms, driver stats, conns/players/npcs: %s", g_ncsv[0] ? g_csv[0][0] : "(none)");
    CHECK(logged("net: conn 0") && logged("net: player pawn") && logged("net: frames="), "verbose also writes the summary lines to the log");
    g_now = 10500; T_tick(g_now);
    CHECK(g_ncsv[1] == 1, "no new snapshot before the interval has elapsed");
    g_now = 11100; T_tick(g_now);
    CHECK(g_ncsv[1] == 2 && g_ncsv[0] == 2, "a new snapshot after the interval");
    void *c2 = mk_conn(0.07f, 0.04f); conns[1] = c2; I32(PTR(W.world, OFF_W_NETDRIVER), OFF_ND_CLIENTCONNS + 8) = 2;
    g_now = 11700; T_tick(g_now);
    CHECK(logged("session: peers 1 -> 2"), "peer count changes are logged");
    I32(PTR(W.world, OFF_W_NETDRIVER), OFF_ND_CLIENTCONNS + 8) = 0;
    g_now = 12300; T_tick(g_now);
    CHECK(logged("session: ended role=host"), "session end is logged when the last client leaves");
    int rows = g_ncsv[1]; g_now = 20000; T_tick(g_now);
    CHECK(g_ncsv[1] == rows, "no rows once the session has ended");

    // a pawn freed between ticks is skipped through its weak pointer, never dereferenced
    I32(PTR(W.world, OFF_W_NETDRIVER), OFF_ND_CLIENTCONNS + 8) = 1;
    g_now = 30000; T_tick(g_now);
    int before = g_ncsv[2]; kill(joiner_pawn);
    g_now = 31100; T_tick(g_now);
    CHECK(g_ncsv[2] == before + 1, "a destroyed pawn is dropped from the next snapshot (%d -> %d rows)", before, g_ncsv[2]);

    // client: no authority game mode, the server connection is the one connection
    make_world(0, 0); reset_tel(); tel_paths();
    void *server_conn = mk_conn(0.02f, 0.01f);
    mk_driver(W.world, NULL, 0, server_conn);
    void *cpawn = mk_player_pawn(W.localpc, 1.f, 2.f, 3.f, 5, 2); PTR(W.localpc, OFF_CTRL_PAWN) = cpawn;
    void *cpawns[1] = { cpawn }; set_pawn_list(W.world, cpawns, 1);
    g_now = 40000; T_init(&API, &verbose, 4242); T_tick(g_now);
    CHECK(logged("role=client") && logged("session: started role=client peers=1"), "a joiner is a client in a session");
    CHECK(T_peers_now() == 1, "T_peers_now is 1 for a client with a server connection (%d)", T_peers_now());
    CHECK(g_ncsv[1] == 1 && strstr(g_csv[1][0], ",0,0x") != NULL && strstr(g_csv[1][0], ",20.0,10.0,") != NULL, "client logs the server connection as idx 0: %s", g_ncsv[1] ? g_csv[1][0] : "(none)");

    // a joiner keeps the AuthorityGameMode of the single-player world it left: it must still be a client (real log: it was labelled host)
    make_world(1, 0); reset_tel(); tel_paths();
    void *j_server = mk_conn(9999.f, 9999.f);                           // lag not measured yet: the engine holds 9999 s
    mk_driver(W.world, NULL, 0, j_server);
    void *jp = mk_player_pawn(W.localpc, 1.f, 2.f, 3.f, 5, 2); PTR(W.localpc, OFF_CTRL_PAWN) = jp;
    void *jps[1] = { jp }; set_pawn_list(W.world, jps, 1);
    g_now = 45000; T_init(&API, &verbose, 4242); T_tick(g_now);
    CHECK(logged("role=client") && !logged("role=host"), "a joiner whose world still has an authority game mode is a client, not a host");
    CHECK(logged("session: started role=client peers=1"), "the joiner's session start is logged");
    CHECK(g_ncsv[1] == 1 && g_ncsv[2] == 1, "the joiner writes its connection and player rows (%d/%d)", g_ncsv[1], g_ncsv[2]);
    CHECK(g_ncsv[1] == 1 && strstr(g_csv[1][0], ",3,-1.0,-1.0,") != NULL, "an unmeasured 9999 s lag is written as -1: %s", g_ncsv[1] ? g_csv[1][0] : "(none)");
    // same world but the server connection is not where we look: the pawn-based joiner test still says client
    make_world(1, 0); reset_tel(); tel_paths();
    mk_driver(W.world, NULL, 0, NULL);
    void *jp2 = mk_pawn(ROLE_SIM, ROLE_AUTH); PTR(W.localpc, OFF_CTRL_PAWN) = jp2;
    g_now = 46000; T_init(&API, &verbose, 4242); T_tick(g_now);
    CHECK(logged("role=client"), "a joiner pawn (RemoteRole=Authority) makes the role client even without a visible server connection");
    // a listen-server host still reports host
    make_world(1, 0); reset_tel(); tel_paths();
    void *hc[1] = { mk_conn(0.02f, 0.02f) }; mk_driver(W.world, hc, 1, NULL);
    PTR(W.localpc, OFF_CTRL_PAWN) = mk_pawn(ROLE_AUTH, ROLE_AUTO);
    g_now = 47000; T_init(&API, &verbose, 4242); T_tick(g_now);
    CHECK(logged("role=host") && !logged("role=client"), "a real listen-server host is still a host");

    // marker key
    make_world(0, 0); PTR(W.world, OFF_W_PAWNLIST) = NULL; I32(W.world, OFF_W_PAWNLIST + 8) = 0;
    { void *mp = mk_player_pawn(W.localpc, 1.f, 2.f, 3.f, 5, 2); PTR(W.localpc, OFF_CTRL_PAWN) = mp; }
    reset_log(); T_mark(); T_mark();
    CHECK(logged("mark #1:") && logged("mark #2:"), "markers are numbered");
    CHECK(logged("pos=1,2,3"), "marker records the local pawn position");
    g_world = NULL; reset_log(); T_mark();
    CHECK(logged("pos=unknown"), "marker without a world does not crash");

    // wrong object types in the slots: ignored, never read as a driver or connection
    make_world(1, 0); reset_tel(); tel_paths();
    PTR(W.world, OFF_W_NETDRIVER) = mk(&C_object);
    g_now = 50000; T_init(&API, &verbose, 4242); T_tick(g_now);
    CHECK(logged("role=standalone") && g_ncsv[0] + g_ncsv[1] + g_ncsv[2] == 0, "a non-UNetDriver in the driver slot is ignored");
    reset_tel(); void *wrong[1] = { mk(&C_object) }; mk_driver(W.world, wrong, 1, NULL);
    g_now = 51000; T_init(&API, &verbose, 4242); T_tick(g_now);
    CHECK(g_ncsv[1] == 0, "a non-UNetConnection in the connection list is skipped");

    // implausible counts are refused once, not looped over
    reset_tel(); void *good[1] = { mk_conn(0.01f, 0.01f) }; void *nd = mk_driver(W.world, good, 1, NULL);
    I32(nd, OFF_ND_CLIENTCONNS + 8) = 100000; I32(W.world, OFF_W_PAWNLIST + 8) = 100000; PTR(W.world, OFF_W_PAWNLIST) = good;
    g_now = 52000; T_init(&API, &verbose, 4242); T_tick(g_now); g_now = 60000; T_tick(g_now); g_now = 70000; T_tick(g_now);
    CHECK(count_logged("implausible ClientConnections.Num=100000") == 1, "an implausible connection count is reported once");
    CHECK(g_ncsv[1] == 0, "no connection rows from an implausible list");
    I32(nd, OFF_ND_CLIENTCONNS + 8) = 1; g_now = 80000; T_tick(g_now);
    CHECK(count_logged("implausible PawnList.Num=100000") == 1, "an implausible pawn list is reported once and skipped");

    // level off: nothing at all
    make_world(1, 0); reset_tel(); tel_paths(); void *oc[1] = { mk_conn(0.01f, 0.01f) }; mk_driver(W.world, oc, 1, NULL);
    g_now = 90000; T_init(&API, &off, 4242); T_frame(0.016f); T_tick(g_now); g_now = 99000; T_tick(g_now);
    CHECK(g_nlog == 0 && g_ncsv[0] + g_ncsv[1] + g_ncsv[2] == 0, "telemetry off records and logs nothing");

    // basic level: rows, but no per-row log lines
    reset_tel(); tel_paths(); g_now = 100000; T_init(&API, &(TelCfg){ TEL_BASIC, 5000 }, 4242); T_tick(g_now);
    CHECK(g_ncsv[1] == 1 && !logged("net: conn"), "basic writes the CSV rows but keeps the log quiet");
}

static void test_edges(void) {
    printf("-- edges\n");
    g_world = NULL; reset_log();
    L_periodic(); L_summon(); L_state();
    CHECK(1, "no world: periodic/summon/state do not crash");
    make_world(0, 0);
    L_summon();
    CHECK(logged("only the host"), "summon refused on a client");
    L_state(); CHECK(logged("state:"), "state dump logs");
}

#include "../noeac.h"
static void test_noeac(void) {
    printf("-- -NoEAC gate\n");
    CHECK(cmdline_has_noeac(L"\"C:\\x\\Absolver-Win64-Shipping.exe\" -NoEAC"), "-NoEAC at the end");
    CHECK(cmdline_has_noeac(L"game.exe -noeac -log"), "lower case, followed by another flag");
    CHECK(cmdline_has_noeac(L"game.exe -NOEAC"), "upper case");
    CHECK(cmdline_has_noeac(L"game.exe /NoEAC"), "slash form");
    CHECK(!cmdline_has_noeac(L"game.exe"), "no flag: stays off");
    CHECK(!cmdline_has_noeac(L"game.exe -NoEACx"), "a longer flag does not count");
    CHECK(!cmdline_has_noeac(L"game.exe -nonoeac"), "flag inside another word does not count");
    CHECK(!cmdline_has_noeac(L"game.exe -log=-NoEAC"), "flag inside an option value does not count");
    CHECK(!cmdline_has_noeac(L"\"C:\\games-NoEAC\\game.exe\""), "flag inside an executable path does not count");
    CHECK(!cmdline_has_noeac(L""), "empty command line");
    CHECK(!cmdline_has_noeac(NULL), "null command line");
}

static void test_parryfix(void) {
    printf("-- Forsaken parry fix\n");
    uint8_t code[] = { 0x48, 0x81, 0xC1, 0xFC, 0x01, 0x00, 0x00 };
    CHECK(parryfix_apply_bytes(code, sizeof code), "original instruction is accepted");
    CHECK(code[3] == 0xF4, "FC is changed to F4");
    uint8_t wrong[] = { 0x48, 0x81, 0xC1, 0xF4, 0x01, 0x00, 0x00 };
    CHECK(!parryfix_apply_bytes(wrong, sizeof wrong), "already-patched or different bytes are refused");
    CHECK(wrong[3] == 0xF4, "refused bytes stay unchanged");
    CHECK(!parryfix_apply_bytes(code, PARRYFIX_SIZE - 1), "short buffer is refused");
}

static void test_object_item_flags(void) {
    printf("-- object-array validity\n");
    CHECK(object_item_is_live(0), "an unflagged object item is live");
    CHECK(!object_item_is_live(0x10000000u), "first engine-invalid flag is rejected");
    CHECK(!object_item_is_live(0x20000000u), "second engine-invalid flag is rejected");
    CHECK(!object_item_is_live(0x30000000u), "both engine-invalid flags are rejected");
}

static void test_console_guard(void) {
    printf("-- console guard\n");
    void *e = (void *)1, *v = (void *)2, *c = (void *)3, *existing = (void *)4;
    CHECK(!console_should_construct(NULL, v, c, NULL), "console waits for the engine");
    CHECK(!console_should_construct(e, NULL, c, NULL), "console waits for the viewport");
    CHECK(!console_should_construct(e, v, NULL, NULL), "console waits for ConsoleClass");
    CHECK(!console_should_construct(e, v, c, existing), "console preserves an existing viewport console");
    CHECK(console_should_construct(e, v, c, NULL), "console constructs only when its inputs are complete");
}

int main(void) {
    test_noeac();
    test_parryfix();
    test_object_item_flags();
    test_console_guard();
    test_join_url();
    test_pvp_gate();
    test_parry_log();
    init_classes();
    L_init(&API);
    test_telemetry_config();
    __try { test_joiner(); test_host(); test_edges(); test_telemetry(); test_parry_inspect(); test_pvp_flow(); test_pvp_teams(); test_autoleave(); test_menu(); test_menu_parse(); test_menu_altar_line(); test_hostip(); test_config(); test_npc(); test_npc_class(); test_npc_menu(); test_m43(); test_match_probe(); test_pvp_result(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_fail++; printf("FAIL: access violation (use of a freed object?) code 0x%08lX\n", GetExceptionCode()); }
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

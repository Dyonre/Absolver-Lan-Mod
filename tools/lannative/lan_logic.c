#include "lan_logic.h"
#include "parrylog.h"
#include "pvpgate.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define U8(p, off)   (*(uint8_t *)((uint8_t *)(p) + (off)))
#define U32(p, off)  (*(uint32_t *)((uint8_t *)(p) + (off)))
#define PTR(p, off)  (*(void **)((uint8_t *)(p) + (off)))
#define I32(p, off)  (*(int32_t *)((uint8_t *)(p) + (off)))

static const Api *A;
#define MAXREMOTE 8
typedef struct { WeakPtr pc, pawn; int used; } Remote;
static Remote g_rem[MAXREMOTE];
static int g_zone_logged = -2;

static void flow_reset(void);

void L_init(const Api *api) {
    A = api;
    memset(g_rem, 0, sizeof g_rem);
    g_zone_logged = -2;
    flow_reset();
}

static int isa(void *obj, void *cls) {              // walk ClassPrivate -> SuperStruct
    if (!obj || !cls) return 0;
    void *c = PTR(obj, OFF_OBJ_CLASS);
    for (int i = 0; c && i < 64; i++) {
        if (c == cls) return 1;
        c = PTR(c, OFF_STRUCT_SUPER);
    }
    return 0;
}

static int is_local_controller(void *pc) { return isa(PTR(pc, OFF_PC_PLAYER), A->cls_localplayer()); }

static void *local_pc(void) {
    void *w = A->world();
    if (!w) return NULL;
    void *gi = PTR(w, OFF_W_GAMEINST);
    void **players = gi ? (void **)PTR(gi, OFF_GI_LOCALPLAYERS) : NULL;
    int player_count = gi ? I32(gi, OFF_GI_LOCALPLAYERS + 8) : 0;
    // SwitchController updates this backlink.  The player-controller list can keep an old local controller after a join.
    if (players && player_count > 0 && player_count <= 4 && isa(players[0], A->cls_localplayer())) {
        void *pc = PTR(players[0], OFF_PLAYER_CONTROLLER);
        if (pc && PTR(pc, OFF_PC_PLAYER) == players[0]) return pc;
    }
    WeakPtr *list = (WeakPtr *)PTR(w, OFF_W_PCLIST);
    int n = I32(w, OFF_W_PCLIST + 8);
    if (!list || n <= 0 || n > 64) return NULL;
    for (int i = 0; i < n; i++) {
        void *pc = A->weak_get(&list[i]);
        if (pc && is_local_controller(pc)) {
            void *pawn = PTR(pc, OFF_CTRL_PAWN);
            if (pawn && U8(pawn, OFF_ACTOR_REMOTEROLE) == ROLE_AUTH) return pc;
        }
    }
    for (int i = 0; i < n; i++) {
        void *pc = A->weak_get(&list[i]);
        if (pc && is_local_controller(pc)) return pc;
    }
    return NULL;
}

int L_is_joiner(void) {
    void *pc = local_pc();
    void *pawn = pc ? PTR(pc, OFF_CTRL_PAWN) : NULL;
    return pawn && U8(pawn, OFF_ACTOR_REMOTEROLE) == ROLE_AUTH;
}

static int host_zone(void *gi) {
    if (!gi) return -1;
    void **players = (void **)PTR(gi, OFF_GI_LOCALPLAYERS);
    int n = I32(gi, OFF_GI_LOCALPLAYERS + 8);
    if (!players || n <= 0) return -1;
    void *lp = players[0];
    return (lp && isa(lp, A->cls_localplayer())) ? U8(lp, OFF_LP_ZONE) : -1;
}

// joiner: the host's pawn for us arrives as a SimulatedProxy, so walking is never sent. Make it an AutonomousProxy, and
// remove the character we controlled before joining (joining does not reload the world, so it would stay alive and be hit).
void L_clientrestart_pre(void *pc, WeakPtr *old_pawn) {
    void *old = pc ? PTR(pc, OFF_CTRL_PAWN) : NULL;
    A->weak_set(old_pawn, old);
}

int L_clientrestart_post(void *pc, void *new_pawn, WeakPtr *old_pawn) {
    (void)pc;
    if (!new_pawn || U8(new_pawn, OFF_ACTOR_REMOTEROLE) != ROLE_AUTH) return 0;  // host / single player pawn: untouched
    if (U8(new_pawn, OFF_ACTOR_ROLE) == ROLE_SIM) {
        U8(new_pawn, OFF_ACTOR_ROLE) = ROLE_AUTO;
        A->log("walking fix: pawn %p Role 1 -> 2", new_pawn);
    }
    void *old = A->weak_get(old_pawn);
    if (old && old != new_pawn && U8(old, OFF_ACTOR_ROLE) == ROLE_AUTH) {
        A->log("removing leftover pawn %p (Role=Authority on a joiner)", old);
        A->destroy(old);
    }
    return 1;
}

typedef struct { void *pc, *mine; WeakPtr victims[8]; int count; } CloneSweep;
static int is_cinematic_double(void *object) {
    char path[256];
    void *cls = PTR(object, OFF_OBJ_CLASS);
    for (int i = 0; cls && i < 64; i++, cls = PTR(cls, OFF_STRUCT_SUPER)) {
        if (A->object_path(cls, path, sizeof path) && !strcmp(path, "/Game/Blueprints/BP_MenuCharacter.BP_MenuCharacter_C")) return 1;
    }
    return 0;
}
static void find_clone(void *object, void *context) {
    CloneSweep *s = (CloneSweep *)context;
    if (object == s->mine || s->count >= (int)(sizeof s->victims / sizeof s->victims[0])) return;
    if ((U32(object, OFF_OBJ_FLAGS) & 0x30u) != 0 || !isa(object, A->cls_fightingchar())) return;
    // Role=Authority on a joiner means "spawned on this PC, not by the server". Two kinds: the standalone character we controlled before joining
    // (still controlled by us when the sweep runs) and, after a leave + rejoin without a world reload, the character of the PREVIOUS session, which the
    // new ClientRestart has already un-possessed (controller NULL). The server's characters (host, other joiners, NPCs) are Role 1 here.
    void *ctrl = PTR(object, OFF_PAWN_CONTROLLER);
    if (U8(object, OFF_ACTOR_ROLE) != ROLE_AUTH || (ctrl != s->pc && ctrl != NULL)) return;
    // Intro/outro doubles are intentionally local and unpossessed, not stale playable pawns.
    if (is_cinematic_double(object)) return;
    A->weak_set(&s->victims[s->count++], object);
}

int L_sweep_joiner_clones(void) {
    void *pc = local_pc(), *mine = pc ? PTR(pc, OFF_CTRL_PAWN) : NULL;
    if (!mine || U8(mine, OFF_ACTOR_REMOTEROLE) != ROLE_AUTH) return 0;
    CloneSweep s = { pc, mine, { 0 }, 0 };
    for (int i = 0; i < (int)(sizeof s.victims / sizeof s.victims[0]); i++) s.victims[i] = (WeakPtr){ -1, 0 };
    A->scan_objects(find_clone, &s);
    int removed = 0;
    for (int i = 0; i < s.count; i++) {
        void *victim = A->weak_get(&s.victims[i]);
        if (!victim) continue;
        if (A->destroy(victim)) {
            removed++;
            A->log("removed clone pawn %p (Role=Authority, our controller or none)", victim);
        } else {
            A->log("clone pawn %p matched but AActor::Destroy returned false", victim);
        }
    }
    return removed;
}

// host: remember joiners (by weak pointer, never raw), keep their respawn zone equal to ours, remove the body of one who leaves
static Remote *find_remote(void *pc) {
    for (int i = 0; i < MAXREMOTE; i++) if (g_rem[i].used && A->weak_get(&g_rem[i].pc) == pc) return &g_rem[i];
    return NULL;
}

static void sync_zone(void *pc, int zone) {
    if (zone < 0) return;
    void *ps = PTR(pc, OFF_CTRL_PLAYERSTATE);
    if (!ps || !isa(ps, A->cls_playerstate())) return;                  // never write into anything but a real FightingPlayerState
    if (U8(ps, OFF_PS_RESPAWNZONE) != (uint8_t)zone) {
        A->log("host: joiner %p respawn zone %d -> %d", pc, U8(ps, OFF_PS_RESPAWNZONE), zone);
        U8(ps, OFF_PS_RESPAWNZONE) = (uint8_t)zone;
    }
}

void L_serverack_post(void *pc, void *pawn) {
    if (!pc || is_local_controller(pc)) return;                         // the host's own controller acknowledges too
    Remote *r = find_remote(pc);
    if (!r) {
        for (int i = 0; i < MAXREMOTE && !r; i++) if (!g_rem[i].used) r = &g_rem[i];
        if (!r) { A->log("host: too many joiners to track"); return; }
        r->used = 1;
        A->weak_set(&r->pc, pc);
        A->log("host: tracking joiner controller %p", pc);
    }
    A->weak_set(&r->pawn, pawn);
    void *w = A->world();
    sync_zone(pc, host_zone(w ? PTR(w, OFF_W_GAMEINST) : NULL));
}

static int pawn_is_connected_elsewhere(void *pawn, void *except_pc) {
    void *w = A->world();
    WeakPtr *list = w ? (WeakPtr *)PTR(w, OFF_W_PCLIST) : NULL;
    int n = w ? I32(w, OFF_W_PCLIST + 8) : 0;
    if (!pawn || !list || n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) {
        void *pc = A->weak_get(&list[i]);
        if (pc && pc != except_pc && PTR(pc, OFF_PC_PLAYER) && PTR(pc, OFF_CTRL_PAWN) == pawn) return 1;
    }
    return 0;
}

static void remove_departed_body(void *body, void *old_pc, const char *where) {
    if (!body) return;
    if (pawn_is_connected_elsewhere(body, old_pc)) {
        A->log("host: %s kept pawn %p; another connected controller owns it", where, body);
        return;
    }
    A->log("host: %s removing character %p", where, body);
    A->log("host: %s destroy %s", where, A->destroy(body) ? "ok" : "returned false");
}

void L_netcleanup_pre(void *pc, CleanupPair *leaving) {
    A->weak_set(&leaving->pc, NULL); A->weak_set(&leaving->pawn, NULL);
    if (!pc || U8(pc, OFF_ACTOR_ROLE) != ROLE_AUTH) return;             // server side only (the client's own controller has Role 2)
    A->weak_set(&leaving->pc, pc);
    A->weak_set(&leaving->pawn, PTR(pc, OFF_CTRL_PAWN));
    A->log("host: net cleanup pc=%p pawn=%p", pc, PTR(pc, OFF_CTRL_PAWN));
}

void L_netcleanup_post(CleanupPair *leaving) {
    void *p = A->weak_get(&leaving->pawn);
    remove_departed_body(p, A->weak_get(&leaving->pc), "net cleanup");
}

void L_periodic(void) {
    void *w = A->world();
    if (!w) return;
    void *gs = PTR(w, OFF_W_GAMESTATE);
    if (gs && isa(gs, A->cls_gamestate()) && U8(gs, OFF_GS_COOPVOICE)) {
        U8(gs, OFF_GS_COOPVOICE) = 0;
        A->log("coop voice chat flag cleared (it crashed the game when a player left)");
    }
    if (!PTR(w, OFF_W_AUTHGM)) return;                                     // joiners stop here

    void *gi = PTR(w, OFF_W_GAMEINST);
    if (gi && isa(gi, A->cls_gameinst()) && U8(gi, OFF_GI_CANSPAWNALONE)) {
        U8(gi, OFF_GI_CANSPAWNALONE) = 0;
        A->log("host: m_bCanSpawnAlone -> 0 (a joiner killed by a player respawns here instead of being sent away)");
    }
    int zone = host_zone(gi);
    if (zone != g_zone_logged) { g_zone_logged = zone; A->log("host zone = %d", zone); }
    for (int i = 0; i < MAXREMOTE; i++) {
        Remote *r = &g_rem[i];
        if (!r->used) continue;
        void *pc = A->weak_get(&r->pc);
        if (pc && PTR(pc, OFF_PC_PLAYER)) {                              // connected
            void *pawn = PTR(pc, OFF_CTRL_PAWN);
            if (pawn) A->weak_set(&r->pawn, pawn);                       // follow respawns
            sync_zone(pc, zone);
            continue;
        }
        void *body = A->weak_get(&r->pawn);                              // controller gone or disconnected
        A->log("host: joiner %p left%s", pc, body ? "; checking its character" : "");
        remove_departed_body(body, pc, "periodic cleanup");
        A->weak_set(&r->pc, NULL); A->weak_set(&r->pawn, NULL); r->used = 0;
    }
}

int L_guard_leave(uint64_t *armed_at, uint64_t now_ms, const char *what) {
    if (!L_is_joiner()) { *armed_at = 0; return 1; }
    if (*armed_at && now_ms - *armed_at <= 5000) { *armed_at = 0; return 1; }
    *armed_at = now_ms;
    A->log("%s ignored: you are connected as a joiner. Press it again within 5 s to leave and do it anyway.", what);
    return 0;
}

void L_summon(void) {
    void *pc = local_pc(), *w = A->world();
    if (!pc || !w) { A->log("summon: no local controller"); return; }
    if (!PTR(w, OFF_W_AUTHGM)) { A->log("summon: only the host can bring players"); return; }
    void *me = PTR(pc, OFF_CTRL_PAWN), *root = me ? PTR(me, OFF_ACTOR_ROOTCOMP) : NULL;
    if (!root) { A->log("summon: no host pawn"); return; }
    float at[3]; memcpy(at, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof at);
    int n = 0;
    for (int i = 0; i < MAXREMOTE; i++) {
        if (!g_rem[i].used) continue;
        void *rpc = A->weak_get(&g_rem[i].pc), *pawn = rpc ? PTR(rpc, OFF_CTRL_PAWN) : NULL;
        if (!pawn || !PTR(rpc, OFF_PC_PLAYER)) continue;
        float to[3] = { at[0] + 150.f * (float)(++n), at[1], at[2] + 50.f };
        A->log("summon: joiner pawn %p -> %.0f,%.0f,%.0f (%s)", pawn, to[0], to[1], to[2], A->set_location(pawn, to) ? "ok" : "failed");
    }
    if (!n) A->log("summon: no connected joiners tracked");
}

int L_parry_inspect(void *parrier, int fix_applied, char *out, int cap) {
    if (!parrier) { snprintf(out, (size_t)cap, "no parrier actor"); return -1; }
    if (!isa(parrier, A->cls_fightingchar())) { snprintf(out, (size_t)cap, "parrier %p is not an AFightingCharacter", parrier); return -1; }
    void *def = PTR(parrier, OFF_CHAR_DEFENSE);
    if (!def) { snprintf(out, (size_t)cap, "parrier %p has no defense component", parrier); return -1; }
    if (!isa(def, A->cls_defense())) { snprintf(out, (size_t)cap, "parrier %p defense component %p has an unexpected class", parrier, def); return -1; }
    WeakPtr cur = *(WeakPtr *)((uint8_t *)def + OFF_DEF_PARRYPROPERTY), pred = *(WeakPtr *)((uint8_t *)def + OFF_DEF_PARRYPROPERTY_PRED);
    int cur_ok = A->weak_get(&cur) != NULL, pred_ok = A->weak_get(&pred) != NULL;
    char text[256];
    int given = parry_describe(fix_applied, cur_ok, pred_ok, text, (int)sizeof text);
    snprintf(out, (size_t)cap, "parrier=%p defense=%p %s", parrier, def, text);
    return given;
}

// LAN-PvP flow. Offline, AAdversarialGameMode::UpdateNumConnectedPlayers / UpdateExpectedPlayer take their numbers from the Sloclap
// "binder" list (0 other clients + 1 = 1) unless GameInstance.m_bDebugFlow (+0x6D90) is set. AAdversarialGameState::ArePlayersInitialized then
// compares that 1 with the real player array (2 once a friend joins) and can never be true, so the match never leaves its waiting state and the
// joiner's loading screen never ends. With the flag set the same functions use GetNumPlayers() and the mode's own expected count (2 for 1v1),
// and HandleMatchValidity no longer sends everyone home. The flag is read only by those four duel-game-mode functions (plus the title menu's
// Blueprint), so it is set only while this PC runs the duel game mode in a duel map and restored afterwards.
static void *g_flow_w, *g_flow_gm, *g_flow_gs, *g_flow_gi, *g_flow_held_gi;
static int g_flow_duel, g_flow_held;
static uint8_t g_flow_orig;
static void result_reset(void);
static int g_flow_3v3, g_perteam, g_team_held, g_team_orig;
static void *g_team_held_gm;
static void flow_reset(void) {
    g_flow_w = g_flow_gm = g_flow_gs = g_flow_gi = g_flow_held_gi = NULL;
    g_flow_duel = g_flow_held = g_flow_3v3 = 0; g_flow_orig = 0;
    g_team_held = 0; g_team_held_gm = NULL; g_team_orig = 0;
    result_reset();
}
void L_set_perteam(int n) { g_perteam = (n >= 1 && n <= 2) ? n : 0; }
int  L_perteam_held(void) { return g_team_held; }

// Holds m_iNumPlayersPerTeam at n (1..2; 0 = not wanted) while this PC runs a 3v3 game mode; puts the game's own number back otherwise.
static int g_team_val;   // the value written while held (the release only restores over its own value)
static void team_step(void *gm, int n) {
    if (n && !g_team_held) {
        int cur = I32(gm, OFF_ADVGM_PERTEAM), teams = I32(gm, OFF_ADVGM_NUMTEAMS);
        if (cur > n && cur <= 8 && teams >= 1 && teams <= 8) {
            g_team_orig = cur; I32(gm, OFF_ADVGM_PERTEAM) = n; g_team_val = n;
            g_team_held = 1; g_team_held_gm = gm;
            A->log("pvp teams: 3v3 game mode %p players per team %d -> %d (teams %d): the match now starts with %d players instead of %d", gm, cur, n, teams, n * teams, cur * teams);
        }
    } else if (n && g_team_held && gm == g_team_held_gm && I32(gm, OFF_ADVGM_PERTEAM) != n) {
        int was = I32(gm, OFF_ADVGM_PERTEAM); I32(gm, OFF_ADVGM_PERTEAM) = n;
        if (was == g_team_val) A->log("pvp teams: players per team %d -> %d (solo duel switched)", was, n);
        else A->log("pvp teams: players per team was reset by the game - set to %d again", n);
        g_team_val = n;
    } else if (!n && g_team_held) {
        if (gm && gm == g_team_held_gm && isa(gm, A->cls_advgm()) && I32(gm, OFF_ADVGM_PERTEAM) == g_team_val) I32(gm, OFF_ADVGM_PERTEAM) = g_team_orig;
        A->log("pvp teams: released - players per team back to %d (if that game mode still exists)", g_team_orig);
        g_team_held = 0; g_team_held_gm = NULL;
    }
}

// Solo duel (map testing without a second PC): holds m_iNumTeams at 1 in a 1v1 or 3v3 map (3v3: per team also 1, see team_step), so the mode expects
// ONE player (expected = teams * per team) and the match leaves its waiting state with only the host in it. Off by default (config "soloduel"); restored when the world changes.
static int g_solo, g_solo_held, g_solo_orig; static void *g_solo_gm;
void L_set_soloduel(int on) { g_solo = on ? 1 : 0; }
int  L_soloduel_held(void) { return g_solo_held; }
static void solo_step(void *gm, int want) {
    if (want && !g_solo_held) {
        int teams = I32(gm, OFF_ADVGM_NUMTEAMS);
        if (teams == 2) {
            g_solo_orig = teams; I32(gm, OFF_ADVGM_NUMTEAMS) = 1; g_solo_held = 1; g_solo_gm = gm;
            A->log("solo duel: game mode %p teams 2 -> 1 (per team %d): the duel starts with only you in it", gm, I32(gm, OFF_ADVGM_PERTEAM));
        }
    } else if (want && g_solo_held && gm == g_solo_gm && I32(gm, OFF_ADVGM_NUMTEAMS) != 1) {
        I32(gm, OFF_ADVGM_NUMTEAMS) = 1;
        A->log("solo duel: team count was reset by the game - set to 1 again");
    } else if (!want && g_solo_held) {
        if (gm && gm == g_solo_gm && isa(gm, A->cls_advgm()) && I32(gm, OFF_ADVGM_NUMTEAMS) == 1) I32(gm, OFF_ADVGM_NUMTEAMS) = g_solo_orig;
        A->log("solo duel: released - teams back to %d (if that game mode still exists)", g_solo_orig);
        g_solo_held = 0; g_solo_gm = NULL;
    }
}

// Solo duel also stretches the AFK timers (AAdversarialGameState m_fTimeBeforeAFKWarning 30 s / m_fTimeBeforeAFKLeave 60 s, from the 1V1 game state defaults), because a
// lone player never fights and would be removed from the match. The two floats are found by value in the live game state (30.0 next to 60.0) and held at ~10 hours.
#ifndef F32
#define F32(p, off)  (*(float *)((uint8_t *)(p) + (off)))
#endif
#define AFK_SCAN_FROM 0x300
#define AFK_SCAN_TO   0x1400
#define AFK_HOLD      36000.0f
static void *g_afk_w, *g_afk_obj; static int g_afk_off = -1, g_afk_told;
static int afk_scan(void *obj) {
    for (int o = AFK_SCAN_FROM; o <= AFK_SCAN_TO - 8; o += 4) {
        float a = F32(obj, o), b = F32(obj, o + 4);
        if ((a == 30.0f && b == 60.0f) || (a == 60.0f && b == 30.0f)) return o;
    }
    return -1;
}
static void afk_step(void *w, void *gs, void *gm, int want) {
    if (!want || !w) { g_afk_w = g_afk_obj = NULL; g_afk_off = -1; return; }
    if (w != g_afk_w) { g_afk_w = w; g_afk_obj = NULL; g_afk_off = -1; g_afk_told = 0; }
    if (g_afk_off < 0) {
        void *cand[10]; int n = 0; const char *what[10]; 
        if (gs) { what[n] = "game state"; cand[n++] = gs; }
        if (gm) { what[n] = "game mode"; cand[n++] = gm; }
        WeakPtr *list = (WeakPtr *)PTR(w, OFF_W_PCLIST); int pcn = I32(w, OFF_W_PCLIST + 8);
        if (list && pcn > 0 && pcn <= 16) for (int i = 0; i < pcn && n < 10; i++) { void *pc = A->weak_get(&list[i]); if (pc) { what[n] = "player controller"; cand[n++] = pc; } }
        for (int i = 0; i < n; i++) { int o = afk_scan(cand[i]); if (o >= 0) { g_afk_obj = cand[i]; g_afk_off = o; A->log("solo duel: AFK timers found in the %s %p at +0x%X; stretched to %.0f s", what[i], cand[i], o, AFK_HOLD); break; } }
        if (g_afk_off < 0) { if (!g_afk_told) { g_afk_told = 1; A->log("solo duel: AFK timers (30/60 s pair) not found yet in the game state, game mode or controllers; retrying every frame"); } return; }
    }
    F32(g_afk_obj, g_afk_off) = AFK_HOLD; F32(g_afk_obj, g_afk_off + 4) = AFK_HOLD * 2;
}

int L_pvp_flow(int enabled) {
    void *w = A->world();
    void *gi = w ? PTR(w, OFF_W_GAMEINST) : NULL;
    if (!w || !gi || !isa(gi, A->cls_gameinst())) return g_flow_held;      // loading or no game instance: leave everything as it is
    void *gm = PTR(w, OFF_W_AUTHGM), *gs = PTR(w, OFF_W_GAMESTATE);
    if (w != g_flow_w || gm != g_flow_gm || gs != g_flow_gs || gi != g_flow_gi) {
        g_flow_w = w; g_flow_gm = gm; g_flow_gs = gs; g_flow_gi = gi;
        char path[200] = "?";
        g_flow_duel = gm && isa(gm, A->cls_advgm()) && A->object_path(w, path, (int)sizeof path) && pvp_path_is_duel(path);
        g_flow_3v3 = g_flow_duel && pvp_path_is_3v3(path);
        if (g_flow_duel) A->log("pvp flow: duel world %s, and this PC runs the duel game mode", path);
        if (g_team_held && gm != g_team_held_gm) {                         // the held game mode belongs to the old world and is gone: never write to it
            A->log("pvp teams: world changed - the held 3v3 game mode %p is gone, nothing written back", g_team_held_gm);
            g_team_held = 0; g_team_held_gm = NULL;
        }
    }
    int perteam = g_solo ? 1 : g_perteam;                                    // solo duel in a 3v3 map: 1 per team (and 1 team, solo_step)
    if (perteam || g_team_held) team_step(gm, enabled && g_flow_3v3 ? perteam : 0);
    if (g_solo || g_solo_held) {
        if (g_solo_held && gm != g_solo_gm) { A->log("solo duel: world changed - the held game mode is gone, nothing written back"); g_solo_held = 0; g_solo_gm = NULL; }
        if (gm) solo_step(gm, enabled && g_flow_duel && g_solo);
        afk_step(w, gs && isa(gs, A->cls_advgs()) ? gs : NULL, gm && isa(gm, A->cls_advgm()) ? gm : NULL, g_solo_held);
    }
    if (enabled && g_flow_duel) {
        if (!g_flow_held) {
            g_flow_orig = U8(gi, OFF_GI_DEBUGFLOW);
            if (g_flow_orig == 0) {
                U8(gi, OFF_GI_DEBUGFLOW) = 1;
                g_flow_held = 1; g_flow_held_gi = gi;
                A->log("pvp flow: GameInstance.m_bDebugFlow 0 -> 1 (real player counts for the duel, no return-to-previous-map from the match check)");
            } else {
                static int told;
                if (!told) { told = 1; A->log("pvp flow: m_bDebugFlow is already %d - leaving it alone", g_flow_orig); }
            }
        } else if (U8(gi, OFF_GI_DEBUGFLOW) == 0) {
            U8(gi, OFF_GI_DEBUGFLOW) = 1;
            A->log("pvp flow: m_bDebugFlow was cleared by the game - set again");
        }
    } else if (g_flow_held) {
        if (gi == g_flow_held_gi) U8(gi, OFF_GI_DEBUGFLOW) = g_flow_orig;
        g_flow_held = 0; g_flow_held_gi = NULL;
        A->log("pvp flow: duel over for this PC - m_bDebugFlow restored to %d", g_flow_orig);
    }
    return g_flow_held;
}

// Duel result (see lan_logic.h). The native winner calculation is correct offline (scores branch); only the backend's verdict is missing.
static void *g_res_w, *g_res_gs, *g_res_gi, *g_res_held_binder;
static int g_res_duel, g_res_held;
static uint8_t g_res_set;
static void result_reset(void) {
    g_res_w = g_res_gs = g_res_gi = g_res_held_binder = NULL;
    g_res_duel = g_res_held = 0; g_res_set = 0;
}

int L_pvp_result(int enabled) {
    void *w = A->world();
    void *gi = w ? PTR(w, OFF_W_GAMEINST) : NULL;
    if (!w || !gi || !isa(gi, A->cls_gameinst())) return g_res_held;           // loading: leave everything as it is
    void *gs = PTR(w, OFF_W_GAMESTATE);
    if (w != g_res_w || gs != g_res_gs || gi != g_res_gi) {
        g_res_w = w; g_res_gs = gs; g_res_gi = gi;
        char path[200] = "?";
        g_res_duel = gs && isa(gs, A->cls_advgs()) && A->object_path(w, path, (int)sizeof path) && pvp_path_is_duel(path);
    }
    int active = 0, team = -1, winner = -1;
    void *binder = NULL;
    if (enabled && g_res_duel) {
        uint64_t post = A->fname_waitingpost ? A->fname_waitingpost() : 0;
        winner = I32(gs, OFF_ADVGS_WINNER);
        void *pc = local_pc(), *ps = pc ? PTR(pc, OFF_CTRL_PLAYERSTATE) : NULL;
        if (ps && isa(ps, A->cls_advps())) team = I32(ps, OFF_ADVPS_TEAM);
        binder = PTR(gi, OFF_GI_BINDER);
        active = post != 0 && *(uint64_t *)((uint8_t *)gs + OFF_ADVGS_CACHESTATE_L) == post && winner != -1 && team != -1 && binder != NULL;
    }
    if (active) {
        if (!g_res_held) {
            if (U8(binder, OFF_BINDER_RESULT) == 0) {
                g_res_set = (uint8_t)(team == winner ? 1 : 2);
                U8(binder, OFF_BINDER_RESULT) = g_res_set;
                g_res_held = 1; g_res_held_binder = binder;
                A->log("pvp result: match over, winner team %d, local team %d -> binder verdict set to %d (%s); the offline game never receives the Sloclap server's verdict",
                       winner, team, g_res_set, g_res_set == 1 ? "victory" : "defeat");
            }
        } else if (U8(binder, OFF_BINDER_RESULT) != g_res_set) {
            A->log("pvp result: the verdict byte changed to %d while we hold it - not touching it", U8(binder, OFF_BINDER_RESULT));
            g_res_held = 0; g_res_held_binder = NULL;
        }
    } else if (g_res_held) {
        void *now = PTR(gi, OFF_GI_BINDER);                                    // the same binder object we wrote into, if it still exists
        if (now && now == g_res_held_binder && U8(now, OFF_BINDER_RESULT) == g_res_set) U8(now, OFF_BINDER_RESULT) = 0;
        g_res_held = 0; g_res_held_binder = NULL;
        A->log("pvp result: post-match state ended - binder verdict put back to 0");
    }
    return g_res_held;
}

// 1 = ps is the PlayerState of a real player's controller (local or connected). NPC kills reach AAdversarialPlayerState::ScoreKill with a NULL
// victim (AI controllers have no PlayerState); the duel then counts them as a kill for the match score.
int L_ps_is_player(void *ps) {
    void *w = A->world();
    WeakPtr *list = w ? (WeakPtr *)PTR(w, OFF_W_PCLIST) : NULL;
    int n = w ? I32(w, OFF_W_PCLIST + 8) : 0;
    if (!ps || !list || n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) {
        void *pc = A->weak_get(&list[i]);
        if (pc && PTR(pc, OFF_CTRL_PLAYERSTATE) == ps) return 1;
    }
    return 0;
}

void *L_local_pc(void) { return local_pc(); }       // free camera (lannative.c): the controller whose view is changed

int L_teleport_local(const float xyz[3]) {
    void *pc = local_pc(), *pawn = pc ? PTR(pc, OFF_CTRL_PAWN) : NULL;
    if (!pawn || !isa(pawn, A->cls_fightingchar())) { A->log("teleport: no controllable character of your own"); return 0; }
    if (L_is_joiner()) { A->log("teleport: you are a joiner - the host owns your character's position (the host can bring you with Bring joiners)"); return 0; }
    void *root = PTR(pawn, OFF_ACTOR_ROOTCOMP);
    float from[3] = { 0, 0, 0 };
    if (root) memcpy(from, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof from);
    uint8_t ok = A->set_location(pawn, xyz);
    A->log("teleport: %.0f,%.0f,%.0f -> %.0f,%.0f,%.0f (%s)", from[0], from[1], from[2], xyz[0], xyz[1], xyz[2], ok ? "ok" : "failed");
    return ok != 0;
}

void L_state(void) {
    void *w = A->world();
    if (!w) { A->log("state: no world"); return; }
    void *pc = local_pc(), *pawn = pc ? PTR(pc, OFF_CTRL_PAWN) : NULL;
    void *gs = PTR(w, OFF_W_GAMESTATE), *gi = PTR(w, OFF_W_GAMEINST);
    A->log("state: %s, localPC=%p pawn=%p role=%d remote=%d zone=%d canSpawnAlone=%d coopVoice=%d",
           PTR(w, OFF_W_AUTHGM) ? (L_is_joiner() ? "server+joiner?" : "host/single-player") : (L_is_joiner() ? "joiner" : "client/idle"),
           pc, pawn, pawn ? U8(pawn, OFF_ACTOR_ROLE) : -1, pawn ? U8(pawn, OFF_ACTOR_REMOTEROLE) : -1, host_zone(gi),
           (gi && isa(gi, A->cls_gameinst())) ? U8(gi, OFF_GI_CANSPAWNALONE) : -1,
           (gs && isa(gs, A->cls_gamestate())) ? U8(gs, OFF_GS_COOPVOICE) : -1);
    for (int i = 0; i < MAXREMOTE; i++) if (g_rem[i].used)
        A->log("state: tracked joiner pc=%p pawn=%p", A->weak_get(&g_rem[i].pc), A->weak_get(&g_rem[i].pawn));
}

// NPC spawning (see lan_logic.h)
#define NPC_TRACK 256
typedef struct { WeakPtr sp; int used, borrowed; float home[3]; } NpcSlot;       // borrowed = the level's own Calbot spawner (moved, to be put back)
static const NpcApi *N;
static NpcSlot g_npc[NPC_TRACK];
static int g_npc_next, g_npc_repl, g_npc_full_told;
static unsigned g_npc_seq;

void L_npc_init(const NpcApi *api) {
    N = api;
    memset(g_npc, 0, sizeof g_npc);
    for (int i = 0; i < NPC_TRACK; i++) g_npc[i].sp = (WeakPtr){ -1, 0 };
    g_npc_next = g_npc_full_told = 0; g_npc_seq = 0;
}
void L_npc_set_replicated(int on) { g_npc_repl = on != 0; }

static void *npc_of(void *sp) { return A->weak_get((WeakPtr *)((uint8_t *)sp + OFF_SPAWNER_AISPAWNED)); }

static int npc_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int npc_iends(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && npc_ieq(s + n - m, suffix);
}

typedef struct { const char *path, *suffix; void *cls; void *found; } NpcFind;
static void npc_find_visit(void *object, void *context) {
    NpcFind *f = (NpcFind *)context;
    if (f->found || (U32(object, OFF_OBJ_FLAGS) & 0x30u) != 0 || !isa(object, f->cls)) return;   // class defaults and archetypes never match
    char path[300];
    if (!A->object_path(object, path, (int)sizeof path)) return;
    if (f->path ? npc_ieq(path, f->path) : npc_iends(path, f->suffix)) f->found = object;
}
static void *npc_find(void *cls, const char *path, const char *suffix) {
    NpcFind f = { path, suffix, cls, NULL };
    if (cls) A->scan_objects(npc_find_visit, &f);
    return f.found;
}

// A point 350 uu in front of the character (rotated 45 degrees further for every NPC, so repeated spawns do not stack), a little above the ground.
static int npc_spot(void *pawn, unsigned seq, float out[3]) {
    void *root = PTR(pawn, OFF_ACTOR_ROOTCOMP);
    if (!root) return 0;
    float p[3], q[4];
    memcpy(p, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof p);
    memcpy(q, (uint8_t *)root + OFF_ROOT_C2W_POS - 16, sizeof q);              // FTransform::Rotation (quaternion x y z w) sits just before the translation
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
    float yaw = atan2f(2.f * (q[3] * q[2] + q[0] * q[1]), 1.f - 2.f * (q[1] * q[1] + q[2] * q[2]));
    if (!isfinite(yaw)) yaw = 0.f;
    float a = yaw + (float)(seq % 8) * 0.7853982f;
    out[0] = p[0] + 350.f * cosf(a); out[1] = p[1] + 350.f * sinf(a); out[2] = p[2] + 50.f;
    return 1;
}

static NpcSlot *npc_slot_for(void *sp) {
    for (int i = 0; i < NPC_TRACK; i++) if (g_npc[i].used && A->weak_get(&g_npc[i].sp) == sp) return &g_npc[i];
    for (int k = 0; k < NPC_TRACK; k++) {                                       // a free slot, or one whose spawner is gone
        int i = (g_npc_next + k) % NPC_TRACK;
        if (!g_npc[i].used || !A->weak_get(&g_npc[i].sp)) { g_npc_next = (i + 1) % NPC_TRACK; return &g_npc[i]; }
    }
    NpcSlot *s = &g_npc[g_npc_next];                                           // table full of live NPCs: the oldest can no longer be removed by "Remove all"
    g_npc_next = (g_npc_next + 1) % NPC_TRACK;
    if (!g_npc_full_told) { g_npc_full_told = 1; A->log("npc: %d NPCs are tracked; older ones can no longer be removed with Remove all NPCs (they stay in the world)", NPC_TRACK); }
    return s;
}

int L_npc_live(void) {
    int n = 0;
    for (int i = 0; i < NPC_TRACK; i++) {
        if (!g_npc[i].used) continue;
        void *sp = A->weak_get(&g_npc[i].sp);
        if (!sp) { g_npc[i].used = 0; continue; }                               // the world that held it is gone
        if (npc_of(sp)) n++;
    }
    return n;
}

int L_npc_spawn(const char *archetype_path, const char *label, const char *class_path) { return L_npc_spawn_at(archetype_path, label, class_path, NULL, NULL, NULL, NULL); }

int L_npc_spawn_at(const char *archetype_path, const char *label, const char *class_path, const float *at, const float *yaw_deg, void **out_ch, void **out_sp) {
    if (out_ch) *out_ch = NULL;
    if (out_sp) *out_sp = NULL;
    if (!N) { A->log("npc: not available in this build"); return 0; }
    void *w = A->world();
    void *pc = w ? local_pc() : NULL, *pawn = pc ? PTR(pc, OFF_CTRL_PAWN) : NULL;
    if (!w || (!at && (!pawn || !isa(pawn, A->cls_fightingchar())))) { A->log("npc: no controllable character of your own - nothing spawned"); return 0; }
    if (!PTR(w, OFF_W_AUTHGM) || L_is_joiner()) { A->log("npc: only the host (or a single-player game) can spawn NPCs - you are a joiner"); return 0; }
    void *gi = PTR(w, OFF_W_GAMEINST);
    if (!gi || !isa(gi, A->cls_gameinst())) { A->log("npc: no game instance yet - nothing spawned"); return 0; }
    if (!archetype_path || !archetype_path[0]) { A->log("npc: no archetype given"); return 0; }
    if (!class_path) class_path = "";
    A->log("npc: spawning '%s' (%s)%s%s", label ? label : "?", archetype_path, class_path[0] ? " as class " : "", class_path);

    void *acls = N->cls_archetype(), *scls = N->cls_aispawner();
    void *arch = npc_find(acls, archetype_path, NULL);
    if (arch) A->log("npc: archetype %p is already loaded", arch);
    else {
        arch = N->load_object(acls, archetype_path);
        if (arch && !isa(arch, acls)) { A->log("npc: %p loaded from %s is not an archetype asset - refused", arch, archetype_path); return 0; }
        A->log("npc: archetype %s", arch ? "loaded from the game's files" : "NOT FOUND (wrong path, or not in the game's files) - nothing spawned");
    }
    if (!arch) return 0;

    float spot[3];
    if (at) memcpy(spot, at, sizeof spot);                                      // the spawner stands where the NPC must appear: an AI pawn that is teleported after it spawned never moves again
    else if (!npc_spot(pawn, g_npc_seq, spot)) { A->log("npc: cannot read your character's position - nothing spawned"); return 0; }
    g_npc_seq++;
    float rot[3] = { 0.f, yaw_deg ? *yaw_deg : 0.f, 0.f };

    void *aimgr = PTR(gi, OFF_GI_AIMANAGER);
    void *npc_cls = aimgr ? PTR(aimgr, OFF_AIMGR_CLASSES) : NULL;
    A->log("npc: spot %.0f,%.0f,%.0f  AIManager=%p character class=%p", spot[0], spot[1], spot[2], aimgr, npc_cls);

    // character classes to try, in order: the requested Blueprint class (if any and found), then the game's generic AI character
    void *cands[2] = { NULL, npc_cls };
    if (class_path[0] && npc_cls) {
        void *uclass = PTR(npc_cls, OFF_OBJ_CLASS);                           // the class of a class object = UClass
        void *c = npc_find(uclass, class_path, NULL);
        if (!c) c = N->load_object(uclass, class_path);
        A->log("npc: character class %s %p", c ? "resolved" : "NOT FOUND - using the generic AI character", c);
        cands[0] = c;
    }

    // 1) our own spawner (works anywhere, any number of NPCs)
    void *made = NULL;
    for (int k = 0; k < 2 && !made; k++) {
        void *cls = cands[k];
        if (!cls) continue;
        void *sp = (yaw_deg && N->spawn_actor_rot) ? N->spawn_actor_rot(w, scls, spot, rot) : N->spawn_actor(w, scls, spot);
        if (sp && !isa(sp, scls)) { A->log("npc: SpawnActor returned %p which is not an AAISpawner - ignored", sp); sp = NULL; }
        if (!sp) { A->log("npc: could not create a spawner actor"); break; }
        PTR(sp, OFF_SPAWNER_CLASS) = cls;
        U8(sp, OFF_SPAWNER_CANRESPAWN) = 0;                                    // a killed NPC stays dead
        U8(sp, OFF_SPAWNER_METHOD) = 2;                                        // OnAnEvent (already the native default)
        N->set_archetype(sp, arch);
        if (g_npc_repl) N->set_replicates(sp, 1);
        uint8_t can = N->can_spawn(sp);
        A->log("npc: own spawner %p%s class %p%s, CanSpawn=%d", sp, g_npc_repl ? " (replicated)" : "", cls, k == 0 && cands[0] ? " (requested)" : "", can);
        if (can) { N->wants_spawn(sp, arch); made = npc_of(sp); }
        if (made) {
            NpcSlot *s = npc_slot_for(sp);
            s->used = 1; s->borrowed = 0; A->weak_set(&s->sp, sp);
            char path[200] = "?"; A->object_path(made, path, (int)sizeof path);
            A->log("npc: spawned %p %s", made, path);
            if (out_ch) *out_ch = made;
            if (out_sp) *out_sp = sp;
            return 1;
        }
        A->log("npc: our own spawner produced no character%s - removing it", (k == 0 && cands[0] && cands[1] && cands[0] != cands[1]) ? " with the requested class" : "");
        A->destroy(sp);
        if (cands[0] == cands[1]) break;
    }
    if (!npc_cls) A->log("npc: the game has no AI character class yet (AIManager missing) - trying the level's own spawner");

    // 2) fallback: the level's own on-demand spawner (BP_AISpawner_Calbot in the open world), moved next to you. One NPC at a time.
    void *cal = npc_find(scls, NULL, ".BP_AISpawner_Calbot");
    if (!cal) { A->log("npc: no fallback spawner (BP_AISpawner_Calbot) in this map - nothing spawned"); return 0; }
    if (U8(cal, OFF_ACTOR_ROLE) != ROLE_AUTH) { A->log("npc: fallback spawner %p is not Role=Authority - nothing spawned", cal); return 0; }
    NpcSlot *s = npc_slot_for(cal);
    float home[3] = { 0, 0, 0 };
    void *root = PTR(cal, OFF_ACTOR_ROOTCOMP);
    if (root) memcpy(home, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof home);
    int first = !(s->used && A->weak_get(&s->sp) == cal);
    A->log("npc: fallback spawner %p moved from %.0f,%.0f,%.0f (%s)", cal, home[0], home[1], home[2], A->set_location(cal, spot) ? "ok" : "failed");
    uint8_t can = N->can_spawn(cal);
    A->log("npc: fallback CanSpawn=%d", can);
    if (can) { N->wants_spawn(cal, arch); made = npc_of(cal); }
    if (first) { s->used = 1; s->borrowed = 1; memcpy(s->home, home, sizeof home); A->weak_set(&s->sp, cal); }
    if (!made) { A->log("npc: the level's spawner produced no character either - nothing spawned"); return 0; }
    char path[200] = "?"; A->object_path(made, path, (int)sizeof path);
    A->log("npc: spawned %p %s (through the level's spawner; spawning again replaces this one)", made, path);
    if (out_ch) *out_ch = made;
    if (out_sp) *out_sp = cal;
    return 1;
}

int L_npc_unspawn(void *sp) {
    if (!N || !sp) return 0;
    NpcSlot *s = NULL;
    for (int i = 0; i < NPC_TRACK; i++) if (g_npc[i].used && A->weak_get(&g_npc[i].sp) == sp) s = &g_npc[i];
    if (!s) return 0;                                                           // not one of ours (or already gone): the pointer may be stale, touch nothing
    if (npc_of(sp)) N->unspawn(sp);
    if (s->borrowed) { A->log("npc: level spawner %p put back (%s)", sp, A->set_location(sp, s->home) ? "ok" : "failed"); }
    else A->log("npc: our spawner %p removed (%s)", sp, A->destroy(sp) ? "ok" : "Destroy returned false");
    s->used = 0;
    return 1;
}

int L_npc_remove_all(void) {
    int cleaned = 0;
    for (int i = 0; i < NPC_TRACK; i++) {
        NpcSlot *s = &g_npc[i];
        if (!s->used) continue;
        void *sp = A->weak_get(&s->sp);
        s->used = 0;
        if (!sp) continue;
        if (npc_of(sp)) N->unspawn(sp);
        if (s->borrowed) A->log("npc: level spawner %p put back (%s)", sp, A->set_location(sp, s->home) ? "ok" : "failed");
        else A->log("npc: our spawner %p removed (%s)", sp, A->destroy(sp) ? "ok" : "Destroy returned false");
        cleaned++;
    }
    g_npc_next = 0; g_npc_full_told = 0;
    A->log("npc: removed %d spawner(s) and their NPCs", cleaned);
    return cleaned;
}

// LanNative M3.3 telemetry. Read-only observation of the running game for testing, the LAN security assessment and netcode research.
// Rules: never write into game memory; keep no raw object pointer across ticks (the world signature below is compared, never dereferenced);
// class-check before reading fields; read nothing from a controller whose connection is gone (its Pawn can dangle). The caller wraps
// every entry point in SEH, so a fault here switches telemetry off without touching the LAN fixes.
#include "lan_telemetry.h"
#include <string.h>
#include <stdio.h>

#define U8(p, off)   (*(uint8_t *)((uint8_t *)(p) + (off)))
#define PTR(p, off)  (*(void **)((uint8_t *)(p) + (off)))
#define I32(p, off)  (*(int32_t *)((uint8_t *)(p) + (off)))
#define U32(p, off)  (*(uint32_t *)((uint8_t *)(p) + (off)))
#define F32(p, off)  (*(float *)((uint8_t *)(p) + (off)))

#define MAX_CONNS   64
#define MAX_PAWNS   512
#define MAX_PLAYERS 32

static const Api *A;
static TelCfg C;
static uint32_t g_pid;
static int g_sig_valid;
static void *g_sig_world, *g_sig_gs, *g_sig_gm, *g_sig_nd;    // compared only, never dereferenced
static char g_map[96] = "-";
static char g_role[12] = "none";
static int g_in_session, g_peers = -1;
static char g_session_role[12];
static uint64_t g_check_due, g_snap_due, g_session_start;
static double g_fsum, g_fmax;
static unsigned g_fcount;
static int g_mark_no;
static int g_warned_conns, g_warned_pawns;
static char g_match_last[900];                     // last duel match line logged (match probe)
static uint64_t g_match_beat_ms;

void T_init(const Api *api, const TelCfg *cfg, uint32_t pid) {
    A = api; C = *cfg; g_pid = pid;
    g_sig_valid = 0; g_sig_world = g_sig_gs = g_sig_gm = g_sig_nd = NULL;
    strcpy(g_map, "-"); strcpy(g_role, "none");
    g_in_session = 0; g_peers = -1; g_check_due = g_snap_due = g_session_start = 0;
    g_fsum = g_fmax = 0; g_fcount = 0; g_mark_no = 0; g_warned_conns = g_warned_pawns = 0;
    g_match_last[0] = 0; g_match_beat_ms = 0;
}

static int isa(void *obj, void *cls) {
    if (!obj || !cls) return 0;
    void *c = PTR(obj, OFF_OBJ_CLASS);
    for (int i = 0; c && i < 64; i++) {
        if (c == cls) return 1;
        c = PTR(c, OFF_STRUCT_SUPER);
    }
    return 0;
}

static void short_map(const char *path, char *out, int cap) {     // "/Game/Maps/X/Y.Y" -> "Y"
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    int n = 0;
    while (s[n] && s[n] != '.' && n < cap - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (!n) snprintf(out, (size_t)cap, "-");
}

static void path_of(void *obj, char *out, int cap) {
    if (!obj) { snprintf(out, (size_t)cap, "-"); return; }
    if (!A->object_path(obj, out, cap)) snprintf(out, (size_t)cap, "?");
}

static void class_path_of(void *obj, char *out, int cap) {
    path_of(obj ? PTR(obj, OFF_OBJ_CLASS) : NULL, out, cap);
}

typedef struct { void *w, *nd, *server; void **conns; int nconn; } NetView;

static void net_view(NetView *v) {
    memset(v, 0, sizeof *v);
    v->w = A->world();
    if (!v->w) return;
    void *nd = PTR(v->w, OFF_W_NETDRIVER);
    if (!nd || !isa(nd, A->cls_netdriver())) return;
    v->nd = nd;
    v->server = PTR(nd, OFF_ND_SERVERCONN);
    void **c = (void **)PTR(nd, OFF_ND_CLIENTCONNS);
    int n = I32(nd, OFF_ND_CLIENTCONNS + 8);
    if (n > MAX_CONNS || n < 0) {
        if (!g_warned_conns) { g_warned_conns = 1; A->log("telemetry: implausible ClientConnections.Num=%d - ignoring the list", n); }
    } else if (c && n > 0) { v->conns = c; v->nconn = n; }
}

// A joiner keeps the world (and the AuthorityGameMode) it had in single player, so "has an authority game mode" does not mean host.
// The server connection decides first (a client driver has one, a listen-server driver never does); the pawn-based joiner test from
// lan_logic.c backs it up in case that pointer is not where a joiner keeps its connection.
static const char *role_name(const NetView *v) {
    if (!v->w) return "none";
    if (v->server || (v->nd && L_is_joiner())) return "client";
    if (PTR(v->w, OFF_W_AUTHGM)) return v->nd ? "host" : "standalone";
    return "none";
}

static void row_prefix(char *out, int cap, uint64_t now) {
    char wall[40];
    A->wall(wall, (int)sizeof wall);
    snprintf(out, (size_t)cap, "%s,%llu,%u,%s,%s", wall, (unsigned long long)now, g_pid, g_role, g_map);
}

// ---------------------------------------------------------------------------------------------------------------------
// world changes and session start/stop (checked twice a second)
static void check_world(uint64_t now) {
    NetView v; net_view(&v);
    void *gs = v.w ? PTR(v.w, OFF_W_GAMESTATE) : NULL, *gm = v.w ? PTR(v.w, OFF_W_AUTHGM) : NULL;
    const char *role = role_name(&v);
    if (!g_sig_valid || v.w != g_sig_world || gs != g_sig_gs || gm != g_sig_gm || v.nd != g_sig_nd) {
        g_sig_valid = 1; g_sig_world = v.w; g_sig_gs = gs; g_sig_gm = gm; g_sig_nd = v.nd;
        snprintf(g_role, sizeof g_role, "%s", role);
        if (!v.w) {
            snprintf(g_map, sizeof g_map, "-");
            A->log("world: none (loading, or between worlds)");
        } else {
            char wp[200], gmp[200], gsp[200];
            path_of(v.w, wp, (int)sizeof wp);
            class_path_of(gm, gmp, (int)sizeof gmp);
            class_path_of(gs, gsp, (int)sizeof gsp);
            short_map(wp, g_map, (int)sizeof g_map);
            A->log("world: map=%s role=%s gamemode=%s gamestate=%s netdriver=%s world=%p", wp, role, gmp, gsp, v.nd ? "yes" : "no", v.w);
        }
    }
    snprintf(g_role, sizeof g_role, "%s", role);

    int peers = !v.nd ? 0 : (v.server ? 1 : v.nconn);
    int in_session = (strcmp(role, "host") == 0 && v.nconn > 0) || strcmp(role, "client") == 0;
    if (in_session && !g_in_session) {
        g_session_start = now; snprintf(g_session_role, sizeof g_session_role, "%s", role);
        A->log("session: started role=%s peers=%d map=%s", role, peers, g_map);
    } else if (!in_session && g_in_session) {
        A->log("session: ended role=%s after %llu s", g_session_role, (unsigned long long)((now - g_session_start) / 1000));
    } else if (in_session && peers != g_peers) {
        A->log("session: peers %d -> %d", g_peers, peers);
    }
    g_in_session = in_session; g_peers = peers;
}

// ---------------------------------------------------------------------------------------------------------------------
// duel match probe (read-only): what the duel's state machine is waiting for, on both PCs. Logs when anything changes, plus a heartbeat.

// bounded append: snprintf reports the length it WANTED, so clamp it
#define APPEND(...) do { int k_ = snprintf(line + n, sizeof line - (size_t)n, __VA_ARGS__); if (k_ > 0) n += k_; if (n > (int)sizeof line - 1) n = (int)sizeof line - 1; } while (0)

static void match_probe(uint64_t now) {
    void *w = A->world();
    void *gs = w ? PTR(w, OFF_W_GAMESTATE) : NULL;
    if (!gs || !isa(gs, A->cls_advgs())) {
        if (g_match_last[0]) { A->log("match: no duel game state any more"); g_match_last[0] = 0; }
        return;
    }
    void *gm = PTR(w, OFF_W_AUTHGM);
    if (gm && !isa(gm, A->cls_advgm())) gm = NULL;
    char state[64] = "-", cache[64] = "-", line[900];
    if (gm && !A->fname_text(*(uint64_t *)((uint8_t *)gm + OFF_GM_MATCHSTATE), state, (int)sizeof state)) snprintf(state, sizeof state, "?");
    if (!A->fname_text(*(uint64_t *)((uint8_t *)gs + OFF_ADVGS_CACHESTATE), cache, (int)sizeof cache)) snprintf(cache, sizeof cache, "?");
    int arr = I32(gs, OFF_GS_PLAYERARRAY + 8);
    int n = 0;
    APPEND("%s state=%s", gm ? "server" : "client", state);
    if (gm) APPEND(" numPlayers=%d nbPlayers=%u nbClients=%u readyWait=%u timeout=%.1fs returnAsked=%u",
                          I32(gm, OFF_GM_NUMPLAYERS), U8(gm, OFF_ADVGM_NBPLAYERS), U8(gm, OFF_ADVGM_NBCLIENTS), U8(gm, OFF_ADVGM_READYWAIT),
                          F32(gm, OFF_ADVGM_TIMEOUT), U8(gm, OFF_ADVGM_RETURNASKED));
    void *gi = PTR(w, OFF_W_GAMEINST), *binder = (gi && isa(gi, A->cls_gameinst())) ? PTR(gi, OFF_GI_BINDER) : NULL;
    int verdict = binder ? U8(binder, OFF_BINDER_RESULT) : -1;              // the backend verdict byte (0 offline until LanNative sets it)
    APPEND(" | gs: cache=%s expected=%u connected=%u playerArray=%d valid=%u readyEnd=%u countdownOver=%u once=%u full=%u winnerTeam=%d verdict=%d",
                  cache, U8(gs, OFF_ADVGS_EXPECTED), U8(gs, OFF_ADVGS_CONNECTED), arr, U8(gs, OFF_ADVGS_VALID), U8(gs, OFF_ADVGS_READYEND),
                  U8(gs, OFF_ADVGS_COUNTDOWN), U8(gs, OFF_ADVGS_ONCE), U8(gs, OFF_ADVGS_FULL), I32(gs, OFF_ADVGS_WINNER), verdict);
    WeakPtr *list = (WeakPtr *)PTR(w, OFF_W_PCLIST);
    int pcn = I32(w, OFF_W_PCLIST + 8), shown = 0;
    if (list && pcn > 0 && pcn <= 16) {
        for (int i = 0; i < pcn && n < (int)sizeof line - 160; i++) {
            void *pc = A->weak_get(&list[i]);
            if (!pc || !PTR(pc, OFF_PC_PLAYER) || !isa(pc, A->cls_fightingpc())) continue;     // disconnected or not a fighting controller: skip
            void *ps = PTR(pc, OFF_CTRL_PLAYERSTATE);
            int ans = (ps && isa(ps, A->cls_advps())) ? U8(ps, OFF_ADVPS_ANSWER) : -1;
            APPEND("%s pc%d[%s pawnInit=%u gameInit=%u loadingIdx=%u waitFor=%u answer=%d team=%d]", shown ? "" : " | pcs:", shown + 1,
                          isa(PTR(pc, OFF_PC_PLAYER), A->cls_localplayer()) ? "local" : "remote", U8(pc, OFF_FPC_PAWNINIT), U8(pc, OFF_FPC_GAMEINIT),
                          U8(pc, OFF_FPC_LOADINGIDX), U8(pc, OFF_FPC_WAITFOR), ans, ans >= 0 ? I32(ps, OFF_ADVPS_TEAM) : -1);
            shown++;
        }
    }
    if (strcmp(line, g_match_last) != 0) {
        A->log("match: %s", line);
        snprintf(g_match_last, sizeof g_match_last, "%s", line); g_match_beat_ms = now;
    } else if (now - g_match_beat_ms >= 10000) {
        A->log("match: unchanged for 10 s: %s", line);
        g_match_beat_ms = now;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// snapshot rows
static int collect_local_pcs(void *w, void **out, int cap) {      // live local controllers, compared by address only
    WeakPtr *list = (WeakPtr *)PTR(w, OFF_W_PCLIST);
    int n = I32(w, OFF_W_PCLIST + 8), found = 0;
    if (!list || n <= 0 || n > MAX_CONNS) return 0;
    for (int i = 0; i < n && found < cap; i++) {
        void *pc = A->weak_get(&list[i]);
        if (pc && isa(PTR(pc, OFF_PC_PLAYER), A->cls_localplayer())) out[found++] = pc;
    }
    return found;
}

static void write_conns(const NetView *v, uint64_t now) {
    char pre[200], line[512];
    row_prefix(pre, (int)sizeof pre, now);
    void *one[1]; void **list = v->conns; int n = v->nconn;
    if (v->server) { one[0] = v->server; list = one; n = 1; }
    for (int i = 0; i < n; i++) {
        void *c = list[i];
        if (!c || !isa(c, A->cls_netconn())) continue;
        // a connection that has not measured lag yet holds 9999 s; write -1 so analysis does not average it in
        float avg_ms = F32(c, OFF_NC_AVGLAG) * 1000.f, best_ms = F32(c, OFF_NC_BESTLAG) * 1000.f;
        if (!(avg_ms < 1000000.f)) avg_ms = -1.f;
        if (!(best_ms < 1000000.f)) best_ms = -1.f;
        snprintf(line, sizeof line, "%s,%d,0x%llX,%d,%.1f,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", pre, i, (unsigned long long)(uintptr_t)c,
                 I32(c, OFF_NC_STATE), avg_ms, best_ms,
                 I32(c, OFF_NC_INPPS), I32(c, OFF_NC_OUTPPS), I32(c, OFF_NC_INBPS), I32(c, OFF_NC_OUTBPS),
                 I32(c, OFF_NC_INLOST), I32(c, OFF_NC_OUTLOST), I32(c, OFF_NC_INPKTS), I32(c, OFF_NC_OUTPKTS),
                 I32(c, OFF_NC_INBYTES), I32(c, OFF_NC_OUTBYTES), I32(c, OFF_NC_MAXPACKET));
        A->csv(TEL_CSV_CONNS, line);
        if (C.level >= TEL_VERBOSE)
            A->log("net: conn %d state=%d lag=%.0f/%.0f ms pps in/out=%d/%d lost in/out=%d/%d", i, I32(c, OFF_NC_STATE),
                   avg_ms, best_ms, I32(c, OFF_NC_INPPS), I32(c, OFF_NC_OUTPPS),
                   I32(c, OFF_NC_INLOST), I32(c, OFF_NC_OUTLOST));
    }
}

// every pawn this PC knows about: players (they have a PlayerState) get a row, NPCs are only counted
static void write_players(const NetView *v, uint64_t now, int *players, int *npcs) {
    *players = 0; *npcs = 0;
    WeakPtr *list = (WeakPtr *)PTR(v->w, OFF_W_PAWNLIST);
    int n = I32(v->w, OFF_W_PAWNLIST + 8);
    if (!list || n <= 0) return;
    if (n > MAX_PAWNS) {
        if (!g_warned_pawns) { g_warned_pawns = 1; A->log("telemetry: implausible PawnList.Num=%d - ignoring the list", n); }
        return;
    }
    void *locals[4]; int nlocal = collect_local_pcs(v->w, locals, 4);
    char pre[200], line[512];
    row_prefix(pre, (int)sizeof pre, now);
    for (int i = 0; i < n; i++) {
        void *pawn = A->weak_get(&list[i]);
        if (!pawn || !isa(pawn, A->cls_fightingchar())) continue;
        void *ps = PTR(pawn, OFF_PAWN_PLAYERSTATE);
        if (!ps) { (*npcs)++; continue; }
        if (*players >= MAX_PLAYERS) { (*players)++; continue; }
        (*players)++;
        void *ctrl = PTR(pawn, OFF_PAWN_CONTROLLER);                  // compared with the live local controllers, never dereferenced
        int local = 0;
        for (int k = 0; k < nlocal; k++) if (ctrl && ctrl == locals[k]) local = 1;
        void *root = PTR(pawn, OFF_ACTOR_ROOTCOMP);
        float pos[3] = { 0, 0, 0 };
        if (root) memcpy(pos, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof pos);
        int ping = -1, zone = -1;
        if (isa(ps, A->cls_playerstate())) { ping = U8(ps, OFF_PS_PING) * 4; zone = U8(ps, OFF_PS_RESPAWNZONE); }
        snprintf(line, sizeof line, "%s,0x%llX,%d,%d,%d,%d,%.1f,%.1f,%.1f,%d,%d", pre, (unsigned long long)(uintptr_t)pawn, local, ctrl ? 1 : 0,
                 U8(pawn, OFF_ACTOR_ROLE), U8(pawn, OFF_ACTOR_REMOTEROLE), pos[0], pos[1], pos[2], ping, zone);
        A->csv(TEL_CSV_PLAYERS, line);
        if (C.level >= TEL_VERBOSE)
            A->log("net: player pawn=%p local=%d role=%d/%d pos=%.0f,%.0f,%.0f ping=%dms", pawn, local, U8(pawn, OFF_ACTOR_ROLE), U8(pawn, OFF_ACTOR_REMOTEROLE),
                   pos[0], pos[1], pos[2], ping);
    }
}

static void snapshot(uint64_t now) {
    NetView v; net_view(&v);
    if (!v.w || !v.nd) return;
    int players = 0, npcs = 0;
    write_conns(&v, now);
    write_players(&v, now, &players, &npcs);
    char pre[200], line[512];
    row_prefix(pre, (int)sizeof pre, now);
    double avg = g_fcount ? g_fsum / g_fcount * 1000.0 : 0.0;
    snprintf(line, sizeof line, "%s,%u,%.2f,%.2f,%u,%u,%u,%u,%d,%d,%d", pre, g_fcount, avg, g_fmax * 1000.0,
             U32(v.nd, OFF_ND_INBPS), U32(v.nd, OFF_ND_OUTBPS), U32(v.nd, OFF_ND_INPKTLOST), U32(v.nd, OFF_ND_OUTPKTLOST),
             v.server ? 1 : v.nconn, players, npcs);
    A->csv(TEL_CSV_FRAMES, line);
    if (C.level >= TEL_VERBOSE)
        A->log("net: frames=%u avg=%.1f ms max=%.1f ms conns=%d players=%d npcs=%d", g_fcount, avg, g_fmax * 1000.0, v.server ? 1 : v.nconn, players, npcs);
    g_fsum = g_fmax = 0; g_fcount = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
void T_frame(float dt) {
    if (C.level == TEL_OFF || !(dt >= 0.f && dt < 10.f)) return;     // also rejects NaN
    g_fsum += dt; g_fcount++;
    if (dt > g_fmax) g_fmax = dt;
}

void T_tick(uint64_t now) {
    if (C.level == TEL_OFF) return;
    if (now >= g_check_due) { g_check_due = now + 500; check_world(now); match_probe(now); }
    if (g_in_session && now >= g_snap_due) { g_snap_due = now + (uint64_t)C.interval_ms; snapshot(now); }
}

int T_peers_now(void) {
    NetView v; net_view(&v);
    if (!v.w || !v.nd) return 0;
    return v.server ? 1 : v.nconn;
}

// UNetConnection.InPacketsPerSecond (a per-second rate, refreshed by the engine) is 0 when nothing arrived during the last second. Every live session carries
// ~300 packets/s (telemetry_conns.csv), so a connection that stays at 0 for several seconds is dead even though the engine keeps it for ~30 s before timing out.
uint64_t T_quiet_ms(uint64_t now) {
    static void *w; static uint64_t last_active;
    NetView v; net_view(&v);
    if (!v.w || !v.nd) { w = NULL; last_active = 0; return 0; }
    if (v.w != w || !last_active) { w = v.w; last_active = now; }
    void *one[1]; void **list = v.conns; int n = v.nconn;
    if (v.server) { one[0] = v.server; list = one; n = 1; }
    int pps = 0;
    for (int i = 0; i < n; i++) if (list[i] && isa(list[i], A->cls_netconn())) pps += I32(list[i], OFF_NC_INPPS);
    if (pps > 0) last_active = now;
    return now >= last_active ? now - last_active : 0;
}

void T_mark(void) {
    uint64_t now = A->now_ms();
    char wall[40]; A->wall(wall, (int)sizeof wall);
    void *w = A->world();
    void *locals[4]; int nlocal = w ? collect_local_pcs(w, locals, 4) : 0;
    void *pawn = nlocal ? PTR(locals[0], OFF_CTRL_PAWN) : NULL;
    void *root = (pawn && isa(pawn, A->cls_fightingchar())) ? PTR(pawn, OFF_ACTOR_ROOTCOMP) : NULL;
    ++g_mark_no;
    if (root) {
        float pos[3]; memcpy(pos, (uint8_t *)root + OFF_ROOT_C2W_POS, sizeof pos);
        A->log("mark #%d: wall=%s tick=%llu role=%s map=%s pos=%.0f,%.0f,%.0f", g_mark_no, wall, (unsigned long long)now, g_role, g_map, pos[0], pos[1], pos[2]);
    } else {
        A->log("mark #%d: wall=%s tick=%llu role=%s map=%s pos=unknown", g_mark_no, wall, (unsigned long long)now, g_role, g_map);
    }
}

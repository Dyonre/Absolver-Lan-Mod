// modes.inc - the "modes" API of the script extender (included by secore.inc). Guide: docs\PLUGIN-API.md "Modes".
//   travel / console_cmd / solo_duel : start a map or a console command (the loader's pvp duel flow + the solo-duel switch do the rest)
//   match_info / on_match_state / on_round_end : read the live AAdversarialGameMode / GameState (polled 10x per second, one poll for all plugins)
//   set_mode_prop : write ruleset properties (m_iNbWinningRounds ...) on the live game mode and game state
//   show_banner / hud_score : text on the HUD, drawn with UCanvas::DrawText from the PostRender hook (md_draw, called by hk_postrender in lannative.c)
// Pure logic (banner queue, round detection, travel command): modes.h. All internal calls pass self = NULL (the helpers ignore it).
#include "modes.h"
static void *md_world_obj(const char *prop) { void *w = world(); void **p = se_valid_ptr(w) ? (void **)api_p_prop_ptr(NULL, w, prop) : NULL; return p && se_valid_ptr(*p) ? *p : NULL; }
static void *md_gm(void) { return md_world_obj("AuthorityGameMode"); }
static void *md_gs(void) { return md_world_obj("GameState"); }
static void md_fstring(const char *utf8, wchar_t *w, int cap, SeFString *fs) { u8_to_w(utf8, w, cap); fs->data = w; fs->num = (int)wcslen(w) + 1; fs->max = cap; }

// ----- console / travel
static int md_console(const char *cmd) {
    void *cdo = api_p_find_cdo(NULL, "KismetSystemLibrary"), *pc = L_local_pc(), *fn = cdo ? api_p_find_function(NULL, cdo, "ExecuteConsoleCommand") : NULL;
    if (!cdo || !pc || !fn) { logf_("modes: console '%s' impossible (library %p, player controller %p, function %p)", cmd, cdo, pc, fn); return 0; }
    static wchar_t wbuf[600]; SeFString fs; md_fstring(cmd, wbuf, 600, &fs);              // FString view of a static buffer: the engine only reads it during the call
    void *args[3] = { &pc, &fs, &pc }; int ok = api_p_call_params(NULL, cdo, fn, args, NULL);
    logf_("modes: console '%s' -> %d", cmd, ok); return ok;
}
static int api_p_console_cmd(const AbsApi *self, const char *cmd) { (void)self; if (!cmd || !*cmd || strlen(cmd) > 500) return 0; return md_console(cmd); }
static int api_p_travel(const AbsApi *self, const char *url) {
    char cmd[600]; if (!md_travel_cmd(url, cmd, sizeof cmd)) { logf_("plugin %s: travel('%s') refused (empty or too long)", plug_name_of(plug_of(self)), url ? url : "(null)"); return 0; }
    logf_("plugin %s: travel -> %s (solo duel %s)", plug_name_of(plug_of(self)), cmd, g_solo_cfg ? "ON" : "off"); return md_console(cmd);
}
static int api_p_solo_duel(const AbsApi *self, int on) {
    if (on >= 0) { g_solo_cfg = on ? 1 : 0; L_set_soloduel(g_solo_cfg); logf_("plugin %s: solo_duel(%d)%s", plug_name_of(plug_of(self)), on, g_pvpflow_on ? "" : " - but the pvp duel flow is OFF (LanNative pvpfix.txt), so it has no effect"); }
    return g_solo_cfg;
}

// ----- match info
static int md_geti(void *o, const char *prop, int dflt) { int *p = o ? (int *)api_p_prop_ptr(NULL, o, prop) : NULL; return p ? *p : dflt; }
static int md_state_text(void *gs, char *out, int cap) { out[0] = 0; if (!gs) return 0; return api_p_get_text(NULL, gs, "MatchState", out, cap) >= 0; }
static int md_team_score(void *gs, int team) {
    void *f = api_p_find_function(NULL, gs, "BPF_GetTeamScore"); uint8_t t = (uint8_t)team; int sc = -1; void *args[1] = { &t };
    return f && api_p_call_params(NULL, gs, f, args, &sc) ? sc : -1;
}
static int md_fill_info(AbsMatchInfo *r) {                            // r must be zeroed with r->size set; 1 = a game state exists
    r->winner_team = -1; for (int i = 0; i < 4; i++) r->team_score[i] = -1;
    void *gm = md_gm(), *gs = md_gs(); r->game_mode = gm; r->game_state = gs; if (!gs) return 0;
    char cn[100] = ""; if (gm) { api_p_class_name(NULL, api_p_object_class(NULL, gm), cn, sizeof cn); snprintf(r->mode, sizeof r->mode, "%s", cn); r->is_duel = api_p_is_a(NULL, gm, "AdversarialGameMode"); }
    md_state_text(gs, r->state, sizeof r->state);
    r->team_count = md_geti(gs, "m_iNumTeams", 0); r->winning_rounds = md_geti(gs, "m_iNbWinningRounds", 0);
    int played = 0; for (int i = 0; i < 4 && i < (r->team_count > 0 && r->team_count < 4 ? r->team_count : 2); i++) { r->team_score[i] = md_team_score(gs, i); if (r->team_score[i] > 0) played += r->team_score[i]; }
    r->round = played + (strstr(r->state, "InProgress") ? 1 : 0);
    int decided = r->is_duel && r->state[0] && !strstr(r->state, "InProgress") && !strstr(r->state, "Intro") && !strstr(r->state, "Countdown") && !strstr(r->state, "WaitingToStart") && !strstr(r->state, "EnteringMap") && !strstr(r->state, "Initializing") && !strstr(r->state, "LevelLoaded") && !strstr(r->state, "LevelReady");
    if (decided) { void *f = api_p_find_function(NULL, gs, "BPF_GetWinnerTeamIndex"); int wt = -1; if (f && api_p_call_params(NULL, gs, f, NULL, &wt)) r->winner_team = wt; }
    { void *f = api_p_find_function(NULL, gs, "GetServerWorldTimeSeconds"); float t = 0; if (f && api_p_call_params(NULL, gs, f, NULL, &t)) r->server_time = t; }
    return 1;
}
static int api_p_match_info(const AbsApi *self, AbsMatchInfo *out) {
    (void)self; if (!out || out->size < sizeof(uint32_t)) return 0;
    AbsMatchInfo r; memset(&r, 0, sizeof r); r.size = out->size; int ok = 0;
    __try { ok = md_fill_info(&r); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("modes: match_info: EXCEPTION 0x%08lX", GetExceptionCode()); ok = 0; }
    memcpy(out, &r, out->size < sizeof r ? out->size : sizeof r); return ok;
}

// ----- ruleset properties
static int api_p_set_mode_prop(const AbsApi *self, const char *name, const char *text) {
    const char *who = plug_name_of(plug_of(self)); if (!name || !*name || !text || strlen(name) > 60) return 0;
    for (const char *c = name; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_')) { logf_("plugin %s: set_mode_prop: bad property name '%s'", who, name); return 0; }
    void *obj[2] = { md_gm(), md_gs() }; int mask = 0;
    for (int i = 0; i < 2; i++) {
        if (!obj[i]) continue; char before[100] = "?", after[100] = "?", cn[100] = ""; void *cls = api_p_object_class(NULL, obj[i]); if (!api_p_find_property(NULL, cls, name)) continue;
        api_p_get_text(NULL, obj[i], name, before, sizeof before); int ok = api_p_set_text(NULL, obj[i], name, text); api_p_get_text(NULL, obj[i], name, after, sizeof after); api_p_class_name(NULL, cls, cn, sizeof cn);
        logf_("plugin %s: set_mode_prop %s.%s: '%s' -> '%s' (wanted '%s', import %s)", who, cn, name, before, after, text, ok ? "ok" : "FAILED"); if (ok) mask |= 1 << i;
    }
    if (!mask) logf_("plugin %s: set_mode_prop('%s'): no game mode / game state with that property (world loaded? the open world has none)", who, name);
    return mask;
}

// ----- events (polled 10x per second)
typedef struct { int plugin; void *cb, *user; } MdCb;
static MdCb g_md_state_cb[16], g_md_round_cb[16]; static int g_md_state_n, g_md_round_n;
static char g_md_state[40]; static MdRounds g_md_rounds; static float g_md_acc;
static int md_cb_add(const AbsApi *self, MdCb *arr, int *n, void *cb, void *user, const char *what) {
    int p = plug_of(self); if (p < 0 || !cb || *n >= 16) return 1;
    EnterCriticalSection(&g_lock); arr[*n].plugin = p; arr[*n].cb = cb; arr[*n].user = user; (*n)++; LeaveCriticalSection(&g_lock);
    logf_("plugin %s: %s registered (polled 10x per second)", plug_name_of(p), what); return 0;
}
static int api_p_on_match_state(const AbsApi *self, void (*cb)(const char *, const char *, void *), void *user) { return md_cb_add(self, g_md_state_cb, &g_md_state_n, (void *)cb, user, "on_match_state"); }
static int api_p_on_round_end(const AbsApi *self, void (*cb)(const AbsMatchInfo *, int, void *), void *user) { return md_cb_add(self, g_md_round_cb, &g_md_round_n, (void *)cb, user, "on_round_end"); }
static void md_poll(void) {
    void *gs = md_gs(); if (!gs) { g_md_state[0] = 0; g_md_rounds.inited = 0; return; }
    char st[40]; md_state_text(gs, st, sizeof st);
    if (strcmp(st, g_md_state)) {
        char old[40]; snprintf(old, sizeof old, "%s", g_md_state); snprintf(g_md_state, sizeof g_md_state, "%s", st);
        logf_("modes: match state '%s' -> '%s'", old, st);
        for (int i = 0; i < g_md_state_n; i++) { int p = g_md_state_cb[i].plugin; if (!plug_guard(p)) continue; __try { ((void (*)(const char *, const char *, void *))g_md_state_cb[i].cb)(old, st, g_md_state_cb[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_match_state", GetExceptionCode()); } }
    }
    if (!g_md_round_n) return;
    AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; if (!md_fill_info(&mi) || !mi.is_duel) return;
    int sc[MD_TEAMS]; for (int i = 0; i < MD_TEAMS; i++) sc[i] = mi.team_score[i] < 0 ? 0 : mi.team_score[i];
    int w = mdr_update(&g_md_rounds, sc, mi.team_count > 0 ? mi.team_count : 2);
    if (w >= 0) {
        logf_("modes: round end: team %d scored (scores %d-%d-%d-%d, state %s)", w, sc[0], sc[1], sc[2], sc[3], mi.state);
        for (int i = 0; i < g_md_round_n; i++) { int p = g_md_round_cb[i].plugin; if (!plug_guard(p)) continue; __try { ((void (*)(const AbsMatchInfo *, int, void *))g_md_round_cb[i].cb)(&mi, w, g_md_round_cb[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_round_end", GetExceptionCode()); } }
    }
}

// ----- HUD text: banner (queued, timed) and score line (persistent), drawn with UCanvas::DrawText from the PostRender hook (the same path as the loader's toast and F1 menu).
static MdBanners g_md_bq; static char g_md_banner_cur[160], g_md_score_text[160]; static unsigned g_md_world;
static int md_draw_active(void) { return g_md_banner_cur[0] || g_md_score_text[0]; }
static void md_draw_line(void *canvas, const char *utf8, float cx, float y, float scale, const uint8_t rgba_bgra[4]) {
    wchar_t wide[200]; int n = 0; u8_to_w(utf8, wide, 200); n = (int)wcslen(wide); if (!n) return;
    void *e = engine(); void *font = e ? *(void **)((uint8_t *)e + OFF_ENGINE_MEDIUMFONT) : NULL; if (!font) return;
    CanvasDrawText_t draw = (CanvasDrawText_t)(g_base + RVA_CANVAS_DRAWTEXT); uint8_t *color = (uint8_t *)canvas + OFF_CANVAS_DRAWCOLOR, saved[4]; memcpy(saved, color, 4);
    float x = cx - 0.5f * 8.0f * scale * (float)n; if (x < 10.f) x = 10.f;                       // medium font ~8 px per character at scale 1: centred by estimate
    FStringView text = { wide, n + 1, n + 1 }; uint8_t info[64]; memset(info, 0, sizeof info); static const uint8_t black[4] = { 0, 0, 0, 255 };
    memcpy(color, black, 4); draw(canvas, font, &text, x + 2.f * scale, y + 2.f * scale, scale, scale, info);
    memcpy(color, rgba_bgra, 4); draw(canvas, font, &text, x, y, scale, scale, info); memcpy(color, saved, 4);
}
static void md_draw(void *canvas) {                                      // called from hk_postrender (lannative.c); the caller has a __try
    if (!md_draw_active()) return;
    static const uint8_t expect[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x70 };
    if (memcmp(g_base + RVA_CANVAS_DRAWTEXT, expect, sizeof expect) != 0) return;
    float clip_x = *(float *)((uint8_t *)canvas + 0x30), clip_y = *(float *)((uint8_t *)canvas + 0x34), sc = 1.f;
    if (clip_y < 400.f || clip_y > 10000.f || clip_x < 400.f) { clip_x = 1920.f; clip_y = 1080.f; } else { sc = clip_y / 1080.f; if (sc < 1.f) sc = 1.f; if (sc > 3.f) sc = 3.f; }
    static const uint8_t yellow[4] = { 40, 220, 255, 255 }, white[4] = { 255, 255, 255, 255 };    // B,G,R,A
    if (g_md_score_text[0]) md_draw_line(canvas, g_md_score_text, clip_x * 0.5f, clip_y * 0.09f, 1.6f * sc, white);
    if (g_md_banner_cur[0]) md_draw_line(canvas, g_md_banner_cur, clip_x * 0.5f, clip_y * 0.30f, 2.6f * sc, yellow);
}
static int api_p_show_banner(const AbsApi *self, const char *utf8, float seconds) { int ok = mdb_push(&g_md_bq, utf8, seconds); logf_("plugin %s: show_banner('%s', %.1f s) -> %d", plug_name_of(plug_of(self)), utf8 ? utf8 : "", seconds, ok); return ok; }
static int api_p_hud_score(const AbsApi *self, const char *utf8) { (void)self; snprintf(g_md_score_text, sizeof g_md_score_text, "%s", utf8 ? utf8 : ""); return 1; }

static void md_tick(float dt) {
    void *w = world();
    if (g_se_world_gen != g_md_world) { g_md_world = g_se_world_gen; g_md_state[0] = 0; g_md_rounds.inited = 0; }          // new world: restart the state / round detection (banners and the score line stay)
    __try {
        if (g_md_state_n || g_md_round_n) { g_md_acc += dt; if (g_md_acc >= 0.1f) { g_md_acc = 0; if (w) md_poll(); } }
        const char *t = NULL; if (mdb_tick(&g_md_bq, dt, &t)) { snprintf(g_md_banner_cur, sizeof g_md_banner_cur, "%s", t); logf_("modes: banner '%s'", t); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("modes: tick EXCEPTION 0x%08lX", GetExceptionCode()); }
}
static void md_fill_api(AbsApi *a) {
    a->travel = api_p_travel; a->console_cmd = api_p_console_cmd; a->solo_duel = api_p_solo_duel; a->match_info = api_p_match_info; a->on_match_state = api_p_on_match_state; a->on_round_end = api_p_on_round_end;
    a->set_mode_prop = api_p_set_mode_prop; a->show_banner = api_p_show_banner; a->hud_score = api_p_hud_score;
    a->set_timer_id = api_p_set_timer_id; a->cancel_timer = api_p_cancel_timer;
}

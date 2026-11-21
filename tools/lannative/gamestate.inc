// gamestate.inc - API 2.29 game_state() and on_world_ready (docs/API-REFERENCE.md). Rules: gamestate.h.
// Once per script-extender frame (LN_SE_TICK 0400, before the plugins' on_tick): the world-ready check, and the flags for game_state() so a call
// from another thread (an overlay draw callback, an audio thread) gets the last frame's answer plus ABS_GS_STALLED when the game thread stopped.
#include "gamestate.h"
typedef struct { int plugin; void (*cb)(const char *map, void *user); void *user; } GsReadyCb;
static GsReadyCb g_gs_cb[16]; static int g_gs_cb_n;
static GsWorld g_gs_w; static char g_gs_path[300]; static volatile unsigned g_gs_flags; static volatile ULONGLONG g_gs_tick_ms; static int g_gs_used;
static void *g_gs_paused_fn, *g_gs_lib;

static void gs_fire(int i) {
    int p = g_gs_cb[i].plugin; if (!plug_guard(p)) return;
    __try { g_gs_cb[i].cb(g_gs_path, g_gs_cb[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_world_ready", GetExceptionCode()); }
}
static int gs_paused(void *w) {   // UGameplayStatics::IsGamePaused(world): only once a plugin asked for the state (one reflected call per frame then)
    if (!g_gs_used || !w) return 0;
    if (!g_gs_paused_fn) { g_gs_lib = api_p_find_cdo(NULL, "GameplayStatics"); void *c = se_find_class("GameplayStatics"); g_gs_paused_fn = c ? se_class_find_function(c, "IsGamePaused") : NULL; if (!g_gs_paused_fn) { g_gs_used = 0; logf_("game_state: GameplayStatics::IsGamePaused not found - paused always reads 0"); return 0; } }
    unsigned char r = 0; void *a[1] = { &w }; return g_gs_lib && api_p_call_params(NULL, g_gs_lib, g_gs_paused_fn, a, &r) ? r != 0 : 0;
}
static void gs_tick(float dt) {
    g_gs_tick_ms = GetTickCount64();
    void *w = world(), *pc = w ? L_local_pc() : NULL, *pawn = NULL; int menu_cursor = 0;
    if (g_gs_w.gen != g_se_world_gen) { g_gs_path[0] = 0; if (w) se_path_of(w, g_gs_path, sizeof g_gs_path); }
    if (pc) { void **pp = (void **)api_p_prop_ptr(NULL, pc, "Pawn"); pawn = pp ? *pp : NULL; api_p_get_bool(NULL, pc, "bShowMouseCursor", &menu_cursor); }
    unsigned gen0 = g_gs_w.gen;
    int became = gs_world_step(&g_gs_w, g_se_world_gen, g_gs_path, w != NULL, pawn != NULL, dt);
    g_gs_flags = gs_flags(&g_gs_w, w != NULL, gs_paused(w), g_ln_focus, menu_cursor, g_menu.open, 0, 1);   // before the callbacks: they see READY
    if (became) {
        logf_("world ready: %s (pawn %p) -> %d on_world_ready callback(s)", g_gs_path, pawn, g_gs_cb_n);
        for (int i = 0; i < g_gs_cb_n; i++) gs_fire(i);
    } else if (gen0 != g_gs_w.gen && g_gs_w.menu) logf_("world ready: %s is a title / menu world - on_world_ready skipped", g_gs_path);
}
LN_SE_TICK(0400, gamestate) { __try { gs_tick(g_ln_dt); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("game_state: EXCEPTION 0x%08lX in the frame check", GetExceptionCode()); } }

// ----- API
static unsigned api_p_game_state(const AbsApi *self) {
    (void)self; if (!g_gs_used) { g_gs_used = 1; logf_("game_state: first asked by %s (the paused check runs from now on)", plug_name_of(plug_of(self))); }
    unsigned f = g_gs_flags; ULONGLONG t = g_gs_tick_ms, now = GetTickCount64();
    if (!api_p_is_game_thread(self) && t && now - t >= GS_STALL_MS) f |= ABS_GS_STALLED;
    return f;
}
static int api_p_on_world_ready(const AbsApi *self, void (*cb)(const char *map_path, void *user), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_gs_cb_n >= 16) return 1;
    EnterCriticalSection(&g_lock); int i = g_gs_cb_n; g_gs_cb[i].plugin = p; g_gs_cb[i].cb = cb; g_gs_cb[i].user = user; g_gs_cb_n++; LeaveCriticalSection(&g_lock);
    if (g_gs_w.ready && api_p_is_game_thread(self)) gs_fire(i);   // the current world is ready already: once now, as for a world that becomes ready
    return 0;
}
LN_API(gamestate) { a->game_state = api_p_game_state; a->on_world_ready = api_p_on_world_ready; }

// camclaim.inc - API 2.28 camera_claim / camera_release / camera_holder (docs/API-REFERENCE.md). Rule: camclaim.h.
// Plugins that show their own camera claim the player's view target with a priority instead of calling
// SetViewTargetWithBlend themselves. After the plugins' on_tick callbacks of every frame (LN_PRETICK 0310) the loader puts the view back on the winning
// claim when anything else (the game, an unmigrated plugin) moved it, so mods stop fighting over the view. No claims = the loader does nothing at all.
// Claims are dropped when the world changes (their actors die with it), when the target actor is destroyed, and when the plugin is switched off (3 faults).
#include "camclaim.h"
static CcTable g_cc; static unsigned g_cc_gen; static int g_cc_holder = -2;   // holder: plugin whose claim the view was last put on (-1 the pawn, -2 none yet)
static void *g_cc_svt;                                                        // APlayerController::SetViewTargetWithBlend (found once)

// a UObject is still live: its GUObjectArray slot (InternalIndex, +0x0C) still points at it and is neither pending kill nor unreachable.
// Shared with hudhide.inc. Any thread; a freed pointer reads as dead (SEH).
static int ln_obj_alive(void *o) {
    if (!se_valid_ptr(o)) return 0; int ok = 0;
    __try {
        uint8_t *array = g_base + RVA_GUOBJECTARRAY, *items = *(uint8_t **)(array + 0x10); int count = *(int32_t *)(array + 0x1C), idx = *(int32_t *)((uint8_t *)o + 0x0C);
        if (items && idx >= 0 && idx < count) { uint8_t *it = items + (size_t)idx * 24; ok = *(void **)it == o && object_item_is_live(*(uint32_t *)(it + 8)); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}
static void *cc_obj_prop(void *obj, const char *name) { void **p = obj ? (void **)api_p_prop_ptr(NULL, obj, name) : NULL; return p ? *p : NULL; }
static int cc_set_view(void *pc, void *target, float blend) {
    if (!pc || !target) return 0;
    if (!g_cc_svt) g_cc_svt = api_p_find_function(NULL, pc, "SetViewTargetWithBlend");
    unsigned char fn = 0, lock = 0; float ex = 0.f; void *a[5] = { &target, &blend, &fn, &ex, &lock };
    return g_cc_svt && api_p_call_params(NULL, pc, g_cc_svt, a, NULL);
}
// the view target now (and the one a running blend goes to): FTViewTarget::Target is the struct's first field
static void cc_view_now(void *pc, void **cur, void **pend) {
    *cur = *pend = NULL; void *cm = cc_obj_prop(pc, "PlayerCameraManager"); if (!cm) return;
    void **v = (void **)api_p_prop_ptr(NULL, cm, "ViewTarget"), **pv = (void **)api_p_prop_ptr(NULL, cm, "PendingViewTarget");
    if (v) *cur = *v; if (pv) *pend = *pv;
}
static const char *cc_who(int p) { return p == -1 ? "the pawn" : p < 0 ? "nobody" : plug_name_of(p); }
// put the view where the claims say (now): the winner's target, or the pawn once no claim is left. blend < 0 = the winner's own blend.
static void cc_apply(float blend, const char *why) {
    void *pc = L_local_pc(); if (!pc) return;
    int t = cc_top(&g_cc), who = t >= 0 ? g_cc.c[t].plugin : -1; void *want = t >= 0 ? g_cc.c[t].target : cc_obj_prop(pc, "Pawn");
    if (!want) { if (g_cc_holder != -2) logf_("camera: no claim left and no pawn - view left as it is (%s)", why); g_cc_holder = -2; return; }
    float b = blend >= 0.f ? blend : t >= 0 ? g_cc.c[t].blend : 0.f;
    int ok = cc_set_view(pc, want, b);
    logf_("camera: view -> %s (%p, priority %d, blend %.2f s; %s)%s", cc_who(who), want, t >= 0 ? g_cc.c[t].priority : 0, b, why, ok ? "" : " - SetViewTargetWithBlend FAILED");
    g_cc_holder = who;
}
static void cc_drop_all(const char *why) { if (!g_cc.n) return; logf_("camera: %d claim(s) dropped (%s)", g_cc.n, why); g_cc.n = 0; g_cc_holder = -2; }

// every frame after the plugins' on_tick (they may have just claimed, released or moved the view)
static void cc_tick(void) {
    if (!g_cc.n) return;
    if (g_cc_gen != g_se_world_gen) { cc_drop_all("the world changed"); return; }
    int dropped = 0;
    for (int i = g_cc.n - 1; i >= 0; i--) {
        const char *why = !plug_guard(g_cc.c[i].plugin) ? "plugin switched off" : !ln_obj_alive(g_cc.c[i].target) ? "its camera actor is gone" : NULL;
        if (why) { logf_("camera: claim of %s dropped (%s)", plug_name_of(g_cc.c[i].plugin), why); cc_remove_at(&g_cc, i); dropped = 1; }
    }
    if (!g_cc.n) { if (dropped) cc_apply(0.f, "last claim dropped"); return; }
    void *pc = L_local_pc(); if (!pc) return;
    int t = cc_top(&g_cc); void *want = g_cc.c[t].target, *cur, *pend; cc_view_now(pc, &cur, &pend);
    if (g_cc.c[t].plugin == g_cc_holder && (cur == want || pend == want)) return;
    if (g_cc.c[t].plugin == g_cc_holder) {   // moved by someone else: back at once (rate-limited log: an unmigrated mod may do it every frame)
        static ULONGLONG next; static int quiet; ULONGLONG now = GetTickCount64();
        if (now >= next) { logf_("camera: the view was moved to %p (not a claim)%s - back to %s", pend ? pend : cur, quiet ? " (and more times, not logged)" : "", plug_name_of(g_cc_holder)); next = now + 5000; quiet = 0; } else quiet++;
        cc_set_view(pc, want, 0.f); return;
    }
    cc_apply(-1.f, "claim changed");
}
LN_PRETICK(0310, camclaim) { __try { cc_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("camera: EXCEPTION 0x%08lX in the claim check", GetExceptionCode()); } }

// ----- API
static int api_p_camera_claim(const AbsApi *self, void *target, int priority, float blend) {
    int p = plug_of(self); if (p < 0 || !target) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: camera_claim refused - not on the game thread", plug_name_of(p)); return -1; }
    if (!ln_obj_alive(target) || !api_p_is_a(self, target, "Actor")) { logf_("plugin %s: camera_claim refused - %p is not a live actor", plug_name_of(p), target); return -1; }
    if (g_cc.n && g_cc_gen != g_se_world_gen) cc_drop_all("the world changed");
    g_cc_gen = g_se_world_gen;
    int was = cc_state(&g_cc, p), had_top = cc_top(&g_cc) >= 0 ? g_cc.c[cc_top(&g_cc)].plugin : -1;
    if (cc_set(&g_cc, p, target, priority, blend) < 0) { logf_("plugin %s: camera_claim refused - %d claims in all", plug_name_of(p), CC_MAX); return -1; }
    int st = cc_state(&g_cc, p);
    if (was != st || was < 0) logf_("plugin %s: camera claim %p priority %d blend %.2f -> %s%s%s", plug_name_of(p), target, priority, blend, st == 1 ? "has the view" : "waits behind ", st == 1 ? "" : plug_name_of(g_cc.c[cc_top(&g_cc)].plugin), had_top >= 0 && had_top != p && st == 1 ? " (taken over)" : "");
    if (st == 1) { void *pc = L_local_pc(), *cur = NULL, *pend = NULL; if (pc) cc_view_now(pc, &cur, &pend); if (cur != target && pend != target) cc_apply(-1.f, "claim"); else g_cc_holder = p; }
    return st;
}
static int api_p_camera_release(const AbsApi *self, float blend) {
    int p = plug_of(self); if (p < 0) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: camera_release refused - not on the game thread", plug_name_of(p)); return -1; }
    int st = cc_state(&g_cc, p); if (st < 0) return 1;
    cc_remove(&g_cc, p); logf_("plugin %s: camera released (it %s the view)", plug_name_of(p), st == 1 ? "had" : "did not have");
    if (st == 1) cc_apply(blend < 0.f ? 0.f : blend, "release");
    return 0;
}
static int api_p_camera_holder(const AbsApi *self, char *name, int cap) {
    int p = plug_of(self), t = cc_top(&g_cc);
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", t >= 0 ? plug_name_of(g_cc.c[t].plugin) : "");
    return p < 0 ? -1 : cc_state(&g_cc, p);
}
LN_API(camclaim) { a->camera_claim = api_p_camera_claim; a->camera_release = api_p_camera_release; a->camera_holder = api_p_camera_holder; }

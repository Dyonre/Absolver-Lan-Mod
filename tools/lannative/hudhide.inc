// hudhide.inc - API 2.30 hud_hide / hud_show (docs/API-REFERENCE.md). Bookkeeping: hudhide.h.
// Replaces the plugins' own for_each_object scans (75-80 ms each): the loader walks the object array itself and compares
// class pointers (a cached per-class answer, no name text), so the one scan at the first hide is a few ms. Widgets that appear later are caught without
// any scan: UserWidget::AddToViewport / AddToPlayerScreen and Widget::SetVisibility get a wrapper (UFunction::Func swap, as replace_native) that hides a
// new widget, or re-hides one the game shows again, while the HUD is hidden. The HUD actor's bShowHUD is held off, also on a new world's controller.
// ponytail: native C++ calls of those functions skip the wrappers (Blueprint calls go through them); hud_hide scans again on every call if that matters.
#include "hudhide.h"
static HhState g_hh; static void *g_hh_uw_cls, *f_hh_inview, *f_hh_getvis, *f_hh_setvis, *g_hh_hud; static int g_hh_hud_old = 1, g_hh_wrapped, g_hh_inside; static unsigned g_hh_gen;
typedef void (*HhThunk)(void *ctx, void *frame, void *result);
static HhThunk o_hh_add, o_hh_addps, o_hh_setvis;

static int hh_setup(void) {
    if (f_hh_setvis) return 1;
    g_hh_uw_cls = se_find_class("UserWidget"); if (!g_hh_uw_cls) { logf_("hud_hide: class UserWidget not found - off"); return 0; }
    f_hh_inview = se_class_find_function(g_hh_uw_cls, "IsInViewport"); f_hh_getvis = se_class_find_function(g_hh_uw_cls, "GetVisibility"); void *sv = se_class_find_function(g_hh_uw_cls, "SetVisibility");
    if (!f_hh_inview || !f_hh_getvis || !sv) { logf_("hud_hide: UserWidget IsInViewport / GetVisibility / SetVisibility not found - off"); return 0; }
    f_hh_setvis = sv; return 1;
}
static int hh_chain_has(void *c, void *want) { for (uint8_t *s = c; se_valid_ptr(s); s = *(uint8_t **)(s + 0x30)) if (s == want) return 1; return 0; }
static int hh_is_widget(void *o) {   // class chain contains UserWidget; the answer is cached per class in an open-addressing table (a linear cache made the scan 183 ms)
    enum { HN = 8192 }; static void *cls[HN]; static unsigned char yes[HN]; static int used;
    void *c = *(void **)((uint8_t *)o + 0x10); if (!c) return 0;
    unsigned h = (unsigned)(((uintptr_t)c >> 4) * 2654435761u) & (HN - 1);
    for (int k = 0; k < HN; k++, h = (h + 1) & (HN - 1)) {
        if (cls[h] == c) return yes[h];
        if (!cls[h]) { int r = hh_chain_has(c, g_hh_uw_cls); if (used < HN * 3 / 4) { cls[h] = c; yes[h] = (unsigned char)r; used++; } return r; }
    }
    return hh_chain_has(c, g_hh_uw_cls);
}
// a widget inside another widget (its outer is a WidgetTree) is never in the viewport by itself: skipped without a reflected call (6042 user
// widgets in MainMap, 4 on screen)
static int hh_child_widget(void *o) { static void *wt; if (!wt) wt = se_find_class("WidgetTree"); void *outer = *(void **)((uint8_t *)o + 0x20); return wt && se_valid_ptr(outer) && *(void **)((uint8_t *)outer + 0x10) == wt; }
static int hh_vis(void *w, unsigned char *vis) { *vis = 0; return api_p_call_params(NULL, w, f_hh_getvis, NULL, vis); }
static int hh_set(void *w, unsigned char vis) { void *a[1] = { &vis }; g_hh_inside++; int ok = api_p_call_params(NULL, w, f_hh_setvis, a, NULL); g_hh_inside--; return ok; }
// hide one widget if it is on screen and shown; 1 = hidden now
static int hh_hide_one(void *w) {
    if ((*(uint32_t *)((uint8_t *)w + 0x08) & 0x10u) || hh_find(&g_hh, w) >= 0) return 0;   // RF_ClassDefaultObject / already ours
    unsigned char inv = 0, vis = 0;
    if (!api_p_call_params(NULL, w, f_hh_inview, NULL, &inv) || !inv || !hh_vis(w, &vis) || !hh_shown(vis)) return 0;
    if (g_hh.n >= HH_MAX) { static int said; if (!said++) logf_("hud_hide: %d widgets hidden - list full, more stay visible", HH_MAX); return 0; }
    if (!hh_set(w, 2)) return 0;
    hh_add(&g_hh, w, vis); return 1;
}
static void hh_hud_actor(int hide) {   // AHUD::bShowHUD of the local controller's HUD
    void *pc = L_local_pc(), **hp = pc ? (void **)api_p_prop_ptr(NULL, pc, "MyHUD") : NULL, *hud = hp ? *hp : NULL;
    if (hide) { if (hud && hud != g_hh_hud) { int v = 1; api_p_get_bool(NULL, hud, "bShowHUD", &v); if (api_p_set_bool(NULL, hud, "bShowHUD", 0)) { g_hh_hud = hud; g_hh_hud_old = v; } } }
    else { if (g_hh_hud && hud == g_hh_hud && ln_obj_alive(hud)) api_p_set_bool(NULL, hud, "bShowHUD", g_hh_hud_old); g_hh_hud = NULL; }
}
// ----- wrappers (UFunction::Func of Blueprint-callable natives; the thunk gets the widget as its context)
// a menu the player opens while the HUD is hidden (Esc menu, inventory) must stay visible: a focusable widget, or one added while the game shows its
// cursor, is left alone
static void hh_after_add(void *ctx) {
    if (!hh_hidden(&g_hh) || g_hh_inside || !se_valid_ptr(ctx)) return;
    __try {
        if (!hh_is_widget(ctx)) return;
        char cn[96]; se_name_of(*(void **)((uint8_t *)ctx + 0x10), cn, sizeof cn);
        int focusable = 0, cursor = 0; void *pc = L_local_pc(); api_p_get_bool(NULL, ctx, "bIsFocusable", &focusable); if (pc) api_p_get_bool(NULL, pc, "bShowMouseCursor", &cursor);
        static int n; if (focusable || cursor) { if (n++ < 40) logf_("hud_hide: new widget %s %p left visible (%s)", cn, ctx, focusable ? "focusable: a menu" : "the game shows its cursor: a menu"); return; }
        if (hh_hide_one(ctx) && n++ < 40) logf_("hud_hide: new widget %s %p hidden (added while the HUD is hidden)", cn, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void hh_th_add(void *ctx, void *frame, void *result) { o_hh_add(ctx, frame, result); hh_after_add(ctx); }
static void hh_th_addps(void *ctx, void *frame, void *result) { o_hh_addps(ctx, frame, result); hh_after_add(ctx); }
static void hh_th_setvis(void *ctx, void *frame, void *result) {
    o_hh_setvis(ctx, frame, result);
    if (!hh_hidden(&g_hh) || g_hh_inside || hh_find(&g_hh, ctx) < 0) return;
    __try { unsigned char v = 0; if (hh_vis(ctx, &v) && hh_update(&g_hh, ctx, v)) { hh_set(ctx, 2); static int n; if (n++ < 40) logf_("hud_hide: widget %p shown again by the game (visibility %u) - hidden, given back at the show", ctx, v); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void hh_wrap(const char *cls, const char *fn, void *thunk, HhThunk *orig) {
    void *c = se_find_class(cls); uint8_t *f = c ? se_class_find_function(c, fn) : NULL;
    if (!f || !se_func_off()) { logf_("hud_hide: %s::%s not wrapped (function / Func offset not found) - such widgets need a hud_hide call again", cls, fn); return; }
    void **slot = (void **)(f + g_func_off); *orig = (HhThunk)*slot; *slot = thunk;
    logf_("hud_hide: %s::%s wrapped (%p -> %p)", cls, fn, (void *)*orig, thunk);
}
// ----- hide / show everything
static int hh_scan(void) {
    LARGE_INTEGER q0, q1, q2, qf; QueryPerformanceCounter(&q0); int seen = 0, top = 0, hid = 0;
    static void *cand[4096]; int nc = 0;
    uint8_t *array = g_base + RVA_GUOBJECTARRAY, *items = *(uint8_t **)(array + 0x10); int count = *(int32_t *)(array + 0x1C);
    for (int i = 0; items && i < count; i++) {
        uint8_t *it = items + (size_t)i * 24; void *o = *(void **)it;
        if (!o || !object_item_is_live(*(uint32_t *)(it + 8)) || !hh_is_widget(o)) continue;
        seen++; if (hh_child_widget(o)) continue;
        top++; if (nc < 4096) cand[nc++] = o;
    }
    QueryPerformanceCounter(&q1);
    for (int i = 0; i < nc; i++) hid += hh_hide_one(cand[i]);   // reflected calls only for the top-level widgets, after the walk
    QueryPerformanceCounter(&q2); QueryPerformanceFrequency(&qf);
    logf_("hud_hide: scan of %d objects: %d user widget(s), %d top-level, %d hidden now (%d in all); walk %.2f ms + checks %.2f ms", count, seen, top, hid, g_hh.n,
          (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart, (double)(q2.QuadPart - q1.QuadPart) * 1000.0 / (double)qf.QuadPart);
    return hid;
}
static void hh_show_all(void) {
    int back = 0, gone = 0;
    for (int i = 0; i < g_hh.n; i++) { if (ln_obj_alive(g_hh.w[i])) back += hh_set(g_hh.w[i], g_hh.vis[i]); else gone++; }
    hh_hud_actor(0); logf_("hud_hide: HUD shown (%d widget(s) back, %d gone)", back, gone); g_hh.n = 0;
}
// every frame while hidden: a new world's controller gets its bShowHUD off; widgets of the old world are forgotten; owners switched off let go
static void hh_tick(void) {
    if (!hh_hidden(&g_hh)) return;
    for (int p = 0; p < 64; p++) if ((g_hh.owners >> p & 1) && !plug_guard(p)) { logf_("hud_hide: %s switched off - its hide let go", plug_name_of(p)); if (hh_owner(&g_hh, p, 0)) { hh_show_all(); return; } }
    if (g_hh_gen != g_se_world_gen) { g_hh_gen = g_se_world_gen; logf_("hud_hide: world changed - %d widget record(s) of the old world dropped, new widgets are hidden as they appear", g_hh.n); g_hh.n = 0; g_hh_hud = NULL; }
    hh_hud_actor(1);
}
LN_SE_TICK(0410, hudhide) { __try { hh_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("hud_hide: EXCEPTION 0x%08lX in the frame check", GetExceptionCode()); } }

// ----- API
static int api_p_hud_hide(const AbsApi *self) {
    int p = plug_of(self); if (p < 0 || p > 63) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: hud_hide refused - not on the game thread", plug_name_of(p)); return -1; }
    int r = -1;
    __try {
        if (!hh_setup()) return -1;
        if (!g_hh_wrapped) { g_hh_wrapped = 1; hh_wrap("UserWidget", "AddToViewport", (void *)hh_th_add, &o_hh_add); hh_wrap("UserWidget", "AddToPlayerScreen", (void *)hh_th_addps, &o_hh_addps); hh_wrap("Widget", "SetVisibility", (void *)hh_th_setvis, &o_hh_setvis); }
        int flip = hh_owner(&g_hh, p, 1);
        logf_("plugin %s: hud_hide%s", plug_name_of(p), flip ? " - HUD hidden" : " (already hidden by another mod or itself: scanned again)");
        if (flip) { g_hh.n = 0; g_hh_gen = g_se_world_gen; }
        hh_hud_actor(1); hh_scan(); r = g_hh.n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("hud_hide: EXCEPTION 0x%08lX", GetExceptionCode()); r = -1; }
    return r;
}
static int api_p_hud_show(const AbsApi *self) {
    int p = plug_of(self); if (p < 0 || p > 63) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: hud_show refused - not on the game thread", plug_name_of(p)); return -1; }
    if (!(g_hh.owners >> p & 1)) return 0;
    int r = 0;
    __try { if (hh_owner(&g_hh, p, 0)) { r = g_hh.n; hh_show_all(); } else logf_("plugin %s: hud_show - another mod still hides the HUD", plug_name_of(p)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf_("hud_show: EXCEPTION 0x%08lX", GetExceptionCode()); r = -1; }
    return r;
}
LN_API(hudhide) { a->hud_hide = api_p_hud_hide; a->hud_show = api_p_hud_show; }

// animstyle.inc - the weapon animation set (API 2.17: anim_style_set / anim_style / real_style / on_handled_weapon). Included by secore.inc.
// A character can use another style's ANIMATION tables (an index into the [style] arrays of GuardDB, HittedDB, DodgeDB, BaseMovementDB, ...)
// without changing its fighting style: stats, special ability, save, menus, mentoring keep the real style.
//   detour AFightingCharacter::BPF_GetCombatStyle (0x322E00): the real answer, replaced by the override only for the animation callers of
//          animstyle.h (return address inside their code) and only for a character with an override; a dwmapi.dll proxy mod (calls the getter from its
//          own code, e.g. for a deflect keyed on a style id) gets the entry's `proxy` style; anything else (Lock / Unlock, stats, AI, other plugins' detours) the real one
//   detour UFightingMovementComponent::ReplaceMovementAnimation (0x448C60): same override on its style argument (Disarm / InitStyle /
//          ServerUpdateMovementAnimation read the style inline and all end here)
// The hooks are installed at the first anim_style_set with a style (lazily: by then every plugin hooked what it hooks at load, so these detours are the
// top of their stacks and see the game's return address; a detour stacked above them later makes every caller "unknown" = real style, logged once).
// on_handled_weapon polls AFightingCharacter+0x1D80 (m_HandledVisibleWeapon, not a UPROPERTY: execBPF_HasVisibleWeaponInHands reads [rcx+0x1D80]): the
// weapon data IN HAND (drawn), NULL when sheathed / disarmed; a picked-up weapon is its own data.
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#include "animstyle.h"
#define AS_OFF_HANDLED 0x1D80
#define AS_OFF_COMP_OWNER 0xE0                                         // UActorComponent owner (UAttackComponent::OnCombatStyleChanged reads it there)
static AsOvr g_as[AS_MAX]; static void *volatile g_as_local; static volatile LONG g_as_active;   // active = number of used entries (fast path when 0)
static int g_as_state;                                                 // 0 not installed, 1 installed, -1 refused (logged)
static uint8_t (*g_as_o_style)(void *); static void (*g_as_o_repl)(void *, uint8_t, uint8_t, uint8_t); static void (*g_as_switch)(void *);
static uint8_t *g_as_px_lo, *g_as_px_hi;                                // a proxy mod's dwmapi.dll (the copy in the game folder, not System32's)
static volatile LONG g_as_calls[AS_NCALLERS], g_as_subs[AS_NCALLERS], g_as_unknown, g_as_unknown_rva, g_as_foreign, g_as_proxy_calls, g_as_proxy_subs, g_as_repl_subs;
static unsigned g_as_gen; static int g_as_refresh_pending;

static uint8_t as_real(void *ch) {                                      // the game's getter, never redirected
    if (g_as_o_style) return g_as_o_style(ch);
    uint32_t rva = api_p_find_symbol(NULL, "AFightingCharacter::BPF_GetCombatStyle"); return rva ? ((uint8_t (*)(void *))(g_base + rva))(ch) : 0xFF;
}
static uint8_t as_style_det(void *ch) {
    uint8_t s = g_as_o_style(ch);
    if (!g_as_active) return s;
    const AsOvr *o = as_lookup(g_as, ch, g_as_local); if (!o) return s;
    uint8_t *ra = (uint8_t *)_ReturnAddress(); int k = -1, proxy = 0;
    if (ra >= g_base && ra < g_base + g_exe_size) {
        k = as_caller_of(AS_CALLERS, AS_NCALLERS, (uint32_t)(ra - g_base));
        if (k < 0) { InterlockedIncrement(&g_as_unknown); g_as_unknown_rva = (LONG)(ra - g_base); }
    } else if (ra >= g_as_px_lo && ra < g_as_px_hi) { proxy = 1; InterlockedIncrement(&g_as_proxy_calls); }
    else { InterlockedIncrement(&g_as_foreign); return s; }                                           // another plugin's detour above ours: vanilla look, never wrong data
    int r = as_decide(s, o, k >= 0 ? AS_CALLERS[k].kind : -1, proxy);
    if (k >= 0) { InterlockedIncrement(&g_as_calls[k]); if (r != s) InterlockedIncrement(&g_as_subs[k]); }
    if (proxy && r != s) InterlockedIncrement(&g_as_proxy_subs);
    return (uint8_t)r;
}
static void as_repl_det(void *mc, uint8_t sex, uint8_t fam, uint8_t style) {
    if (g_as_active) {
        void *ch = NULL; __try { ch = se_valid_ptr(mc) ? *(void **)((uint8_t *)mc + AS_OFF_COMP_OWNER) : NULL; } __except (EXCEPTION_EXECUTE_HANDLER) { ch = NULL; }
        const AsOvr *o = as_lookup(g_as, ch, g_as_local); if (o && o->style != style) { style = (uint8_t)o->style; InterlockedIncrement(&g_as_repl_subs); }
    }
    g_as_o_repl(mc, sex, fam, style);
}

// the dwmapi.dll in the game's Binaries folder (a proxy mod), not the System32 one
static void as_find_proxy(void) {
    HMODULE m = GetModuleHandleW(L"dwmapi.dll"); wchar_t p[MAX_PATH] = L"", exe[MAX_PATH] = L""; if (!m) return;
    GetModuleFileNameW(m, p, MAX_PATH); GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *a = wcsrchr(p, L'\\'), *b = wcsrchr(exe, L'\\'); if (!a || !b || (a - p) != (b - exe) || _wcsnicmp(p, exe, (size_t)(a - p))) { logf_("animstyle: dwmapi.dll is the system one (%ls): no proxy mod", p); return; }
    g_as_px_lo = (uint8_t *)m; g_as_px_hi = g_as_px_lo + image_size(g_as_px_lo);
    logf_("animstyle: dwmapi.dll proxy mod %ls at %p..%p: its BPF_GetCombatStyle calls get an override's proxy style", p, (void *)g_as_px_lo, (void *)g_as_px_hi);
}
static int as_install(const AbsApi *self) {
    if (g_as_state) return g_as_state > 0;
    if (!api_p_is_game_thread(self)) return 0;                                                       // retried by the next call on the game thread
    int bad = 0, verified = 0;
    for (int i = 0; i < AS_NCALLERS; i++) {                                                          // the caller table must describe THIS build
        uint32_t rva = api_p_find_symbol(self, AS_CALLERS[i].name);
        if (!rva) logf_("animstyle: caller %s (0x%X, %u bytes, %s): not in symbols.bin, range taken as listed", AS_CALLERS[i].name, AS_CALLERS[i].rva, AS_CALLERS[i].size, AS_CALLERS[i].kind ? "animation" : "keep");
        else if (rva != AS_CALLERS[i].rva) { logf_("animstyle: caller %s is at 0x%X in symbols.bin, the table says 0x%X - another game build?", AS_CALLERS[i].name, rva, AS_CALLERS[i].rva); bad++; }
        else verified++;
    }
    if (bad) { g_as_state = -1; logf_("animstyle: NOT installed (%d caller address(es) differ): animations keep the real style", bad); return 0; }
    g_as_o_style = (uint8_t (*)(void *))api_p_hook_symbol(self, "AFightingCharacter::BPF_GetCombatStyle", (void *)as_style_det);
    if (!g_as_o_style) { g_as_state = -1; logf_("animstyle: NOT installed (BPF_GetCombatStyle hook failed)"); return 0; }
    g_as_o_repl = (void (*)(void *, uint8_t, uint8_t, uint8_t))api_p_hook_symbol(self, "UFightingMovementComponent::ReplaceMovementAnimation", (void *)as_repl_det);
    uint32_t sw = api_p_find_symbol(self, "AFightingCharacter::SwitchMovementAnimations"); g_as_switch = sw ? (void (*)(void *))(g_base + sw) : NULL;
    as_find_proxy(); g_as_state = 1;
    const HookEntry *h = hr_find(&g_hreg, 0x322E00);
    logf_("animstyle: installed: getter detour %p (stack depth %d), movement detour %p%s, refresh %p; %d of %d callers verified in symbols.bin",
          (void *)g_as_o_style, h ? h->depth : -1, (void *)g_as_o_repl, g_as_o_repl ? "" : " (MISSING: Disarm / InitStyle locomotion keeps the real style)", (void *)g_as_switch, verified, AS_NCALLERS);
    return 1;
}
static void as_refresh(void *ch) {                                     // locomotion is cached in UPlayerAnim containers: re-select it once after a change
    if (!g_as_switch || !se_valid_ptr(ch)) return;
    __try { g_as_switch(ch); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("animstyle: SwitchMovementAnimations(%p) EXCEPTION 0x%08lX", ch, GetExceptionCode()); }
}
static void as_stats_log(const char *why) {
    char line[1600]; int n = snprintf(line, sizeof line, "animstyle: %s: substituted/calls", why);
    for (int i = 0; i < AS_NCALLERS && n < (int)sizeof line - 100; i++) if (g_as_calls[i]) n += snprintf(line + n, sizeof line - n, " | %s %ld/%ld", AS_CALLERS[i].name, g_as_subs[i], g_as_calls[i]);
    snprintf(line + n, sizeof line - n, " | movement %ld | proxy mod %ld/%ld | unknown callers %ld (last rva 0x%lX) | above-our-detour %ld", g_as_repl_subs, g_as_proxy_subs, g_as_proxy_calls, g_as_unknown, g_as_unknown_rva, g_as_foreign);
    logf_("%s", line);
}
static int as_tables_hold(int style, char *why, int cap);              // animsets.inc: do all 66 style tables hold this index?
// internal (weaponslot.inc): also the style a dwmapi.dll proxy mod is told
static int as_set_ex(const AbsApi *self, void *ch, int style, int proxy) {
    const char *who = plug_name_of(plug_of(self));
    if (style > 63 || style < -1) { logf_("plugin %s: anim_style_set: style %d outside -1..63", who, style); return 0; }
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: anim_style_set refused - not on the game thread", who); return 0; }
    // an index past the stock styles only if every table holds it (HittedDB.m_HandPoses is read WITHOUT a bounds check: short = out-of-bounds read)
    { char why[240] = ""; if (style >= 6 && !as_tables_hold(style, why, sizeof why)) { logf_("plugin %s: anim_style_set(%d) REFUSED: %s (own animations stay)", who, style, why); return 0; } }
    if (style >= 0 && !as_install(self)) return 0;
    if (ch && ch == cd_local_pawn()) ch = NULL;                                                      // the local player has one entry (survives travel)
    int before = -1, before_proxy = -1; for (int i = 0; i < AS_MAX; i++) if (g_as[i].used && g_as[i].ch == ch) { before = g_as[i].style; before_proxy = g_as[i].proxy; }
    if (as_set(g_as, ch, style, proxy) == -2) { logf_("plugin %s: anim_style_set: table full (%d characters)", who, AS_MAX); return 0; }
    g_as_active = as_count(g_as);
    if (before != style || (style >= 0 && before_proxy != (proxy < 0 ? -1 : proxy))) {
        void *target = ch ? ch : cd_local_pawn();
        logf_("plugin %s: anim_style_set(%s %p): animation style %d -> %d (proxy style %d), real style %d, %ld override(s)", who, ch ? "character" : "local player", target, before, style, style >= 0 ? proxy : -1, target ? as_real(target) : -1, g_as_active);
        if (style < 0) as_stats_log("override removed");
        if (target) as_refresh(target);
    }
    return 1;
}
static int api_p_anim_style_set(const AbsApi *self, void *ch, int style) { return as_set_ex(self, ch, style, -1); }
static int api_p_real_style(const AbsApi *self, void *ch) {
    (void)self; if (!ch) ch = cd_local_pawn(); if (!se_valid_ptr(ch)) return -1; int s = 0xFF;
    __try { s = as_real(ch); } __except (EXCEPTION_EXECUTE_HANDLER) { s = 0xFF; }
    return s == 0xFF ? -1 : s;
}
static int api_p_anim_style(const AbsApi *self, void *ch) {
    if (!ch) ch = cd_local_pawn(); if (!se_valid_ptr(ch)) return -1;
    const AsOvr *o = as_lookup(g_as, ch, g_as_local); return o ? o->style : api_p_real_style(self, ch);
}

// ----- on_handled_weapon: polled per tick for the local player
typedef struct { int plugin, inited; void (*cb)(void *, void *, int, void *); void *user, *pawn, *last; } AsWatch;
static AsWatch g_asw[16]; static int g_asw_n;
static void *as_handled(void *pawn) {                                  // m_HandledVisibleWeapon, checked: NULL or a VisibleWeaponData object
    static int bad_logged; static void *ok_last; void *w = NULL;
    __try { w = *(void **)((uint8_t *)pawn + AS_OFF_HANDLED); } __except (EXCEPTION_EXECUTE_HANDLER) { w = NULL; }
    if (!w || w == ok_last) return w;
    int ok = 0; __try { ok = se_valid_ptr(w) && api_p_is_a(NULL, w, "VisibleWeaponData"); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (ok) { ok_last = w; return w; }
    { if (!bad_logged++) logf_("animstyle: character+0x%X holds %p, not a VisibleWeaponData - layout differs from this build, on_handled_weapon reports nothing in hand", AS_OFF_HANDLED, w); return NULL; }
    return w;
}
static int as_family(void *pawn) { void *ac = cd_comp(pawn, "m_AttackComponent"); uint8_t *f = ac ? api_p_prop_ptr(NULL, ac, "m_eActiveWeapon") : NULL; return f && *f < 3 ? *f : -1; }
static void as_tick(float dt) {
    (void)dt; void *pawn = cd_local_pawn(); g_as_local = pawn;
    if (g_se_world_gen != g_as_gen) { g_as_gen = g_se_world_gen; int n = as_drop_explicit(g_as); g_as_active = as_count(g_as); if (n) logf_("animstyle: new world: %d character override(s) dropped", n); }
    if (!pawn || !g_asw_n) return;
    { static void *checked; static int is_fc; if (pawn != checked) { checked = pawn; __try { is_fc = api_p_is_a(NULL, pawn, "FightingCharacter"); } __except (EXCEPTION_EXECUTE_HANDLER) { is_fc = 0; } }
      if (!is_fc) return; }                                            // title / menu pawns are other classes: +0x1D80 is not the weapon there
    void *w = as_handled(pawn);
    for (int i = 0; i < g_asw_n; i++) {
        AsWatch *x = &g_asw[i]; if (!plug_guard(x->plugin)) continue;
        if (x->pawn != pawn) { x->pawn = pawn; x->inited = 0; }
        if (x->inited && x->last == w) continue;
        x->last = w; x->inited = 1; int fam = w ? as_family(pawn) : 0;
        { static int n; if (n++ < 60) { char a[160] = "none"; if (w) se_path_of(w, a, sizeof a); logf_("animstyle: weapon in hand -> %s (family %d, plugin %s)", a, fam, plug_name_of(x->plugin)); } }
        __try { x->cb(pawn, w, fam, x->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(x->plugin, "on_handled_weapon", GetExceptionCode()); }
    }
}
static int api_p_on_handled_weapon(const AbsApi *self, void (*cb)(void *, void *, int, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_asw_n >= 16) return 1;
    AsWatch *x = &g_asw[g_asw_n]; memset(x, 0, sizeof *x); x->plugin = p; x->cb = cb; x->user = user; g_asw_n++;
    logf_("plugin %s: on_handled_weapon registered (polled per tick, local player, character+0x%X)", plug_name_of(p), AS_OFF_HANDLED); return 0;
}
static void as_fill_api(AbsApi *a) { a->anim_style_set = api_p_anim_style_set; a->anim_style = api_p_anim_style; a->real_style = api_p_real_style; a->on_handled_weapon = api_p_on_handled_weapon; }

// faejinesc.inc - Faejin gold-loop brakes (prototype, both default OFF): (1) gold-link PRESS WINDOW SHRINK (faejinws.h), (2) stamina escalation (faejinesc.h). Included by content.inc after hitpre.inc (uses hp_is_fchar / hp_comp).
// Pure logic: faejinesc.h.
//
// Where the engine charges stamina for an attack.
//   UConsumStaminaNotify (clip notify) -> UDefenseComponent::AnimNotifyStaminaFired -> StaminaImpact @0x34FF50, Consumption branch for the Attack / Cancel order types:
//   cost = notify cost * AFightingCharacter::GetActionStaminaCostCoefFromEffects(char, tick) @0x32F5E0 (its ONLY caller), then ApplyActionCostGuardGauge @0x3216D0 (the guard gauge = stamina bar).
//   The loader hooks GetActionStaminaCostCoefFromEffects (77 bytes, pure: effects table -> float) and multiplies its result by fe_mult() for Faejin characters.
//   Optional log hook on ApplyActionCostGuardGauge (the FINAL charged amount, every character): config fe_log = on.
// Chain source: UAttackComponent::m_iPerfectLinkChain (+0x950), written by LaunchAttack (0 on a non-gold launch, 1 on the first gold link, +1 per further link); read at on_attack_start.
// Safety: both hooks go through hook_symbol (symbols.bin bytes compared with the loaded exe before patching; a mismatch = no hook, logged). BPF_GetCombatStyle is called through its
// RVA only after the same byte comparison. Everything in __try. With fe_escalation off and fe_log off NOTHING is installed.
// WINDOW SHRINK (fe_window = on): three hooks, all byte-verified through symbols.bin, default off:
//   UAvailabilityLayerNotifyState::NotifyOutside @0x3062E0 (this notify state, mesh, seq, NotifyStateInfo*): info+0x14 m_bActive, +0x28 m_bComingFromRunningOrder, +0x29 m_bPrediction,
//        this+0x38 m_availabilityLayerData, mesh+0xE0 owner. The first activation of layer "PerfectAttackOnly" (the clip notify "A.L : Perfect release") = t_open of the gold window.
//   APAttackDetection::OnActionJustStackedCallback @0x3679B0 (this, InputAction): the press time of Attack1/2 (0/1), Attack1/2Perfect (19/20), DrunkenAttack/Perfect ('J' 74 / 'K' 75).
//   UAttackComponent::CanPerfectLink @0x2EEAC0 (this, next row): the decision. A true answer for the LOCAL player with style 5 is vetoed (false = plain link) when
//        ws_accept() says the press was not inside the schedule's window for the current number of chained gold links (faejinws.h).
// config.txt keys: fe_escalation = on|off (default off), fe_n0 (2), fe_a (0.35), fe_cap (3), fe_tau (0 = off), fe_reset (1.5 s), fe_style (5), fe_log = on|off (default off); window shrink: fe_window = on|off (default off), fe_ws_widths = 2,1.5,1,0.5 (frames per consecutive gold link, then none), fe_ws_center = 1 (frames after the window opened).
#include "faejinesc.h"
#include "faejinws.h"

static const AbsApi *FE_API;
static FeCfg g_fe_cfg; static int g_fe_log, g_fe_state;                  // g_fe_state: 0 not started, 1 hooks installed, -1 failed / off
typedef float (*FeCoef_t)(void *, int64_t);
typedef void (*FeApply_t)(void *, float);
typedef uint8_t (*FeStyle_t)(void *);
typedef void (*FeNotify_t)(void *, void *, void *, uint8_t *);
typedef void (*FeStack_t)(void *, int);
typedef char (*FeCpl_t)(void *, void *);
static FeCoef_t o_fe_coef; static FeApply_t o_fe_apply; static FeStyle_t g_fe_style_fn; static FeNotify_t o_fe_notify; static FeStack_t o_fe_stack; static FeCpl_t o_fe_cpl;
static WsCfg g_ws_cfg; static double g_ws_press_t; static volatile LONG g_ws_veto, g_ws_ok, g_ws_nolog;
typedef struct { void *ch; FeState s; int open; double t_open; } FeSlot;
static FeSlot g_fe_slot[16]; static unsigned g_fe_gen; static int g_fe_off_ac = -1;
static volatile LONG g_fe_logn, g_fe_scaled, g_fe_links;
static double fe_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
static FeSlot *fe_slot(void *ch, int create) {
    if (g_fe_gen != g_se_world_gen) { g_fe_gen = g_se_world_gen; memset(g_fe_slot, 0, sizeof g_fe_slot); }
    int free_ = -1; for (int i = 0; i < 16; i++) { if (g_fe_slot[i].ch == ch) return &g_fe_slot[i]; if (!g_fe_slot[i].ch && free_ < 0) free_ = i; }
    if (!create) return NULL; if (free_ < 0) free_ = (int)(fe_now() * 1000) & 15;       // table full (never in practice): recycle one
    memset(&g_fe_slot[free_], 0, sizeof g_fe_slot[free_]); g_fe_slot[free_].ch = ch; return &g_fe_slot[free_];
}
static int fe_style_of(void *ch) { return g_fe_style_fn ? (int)g_fe_style_fn(ch) : -1; }
static int fe_chain_of(void *ch) { void *ac = hp_comp(ch, &g_fe_off_ac, "m_AttackComponent"); return ac ? *(int *)((uint8_t *)ac + 0x950) : -1; }

// ----- hooks
static float hk_fe_coef(void *ch, int64_t tick) {
    float c = o_fe_coef(ch, tick);
    __try {
        if (g_fe_cfg.enabled && hp_is_fchar(ch)) {
            FeSlot *sl = fe_slot(ch, 0); int st = fe_style_of(ch);
            if (sl && st == g_fe_cfg.style) {
                double m = fe_mult(&g_fe_cfg, &sl->s, st, fe_now());
                if (m != 1.0) { c *= (float)m; LONG n = InterlockedIncrement(&g_fe_scaled); if (n <= 2000) logf_("faejinesc: stamina coef x%.3f (n_eff %.2f) -> %.3f", m, fe_n_eff(&g_fe_cfg, &sl->s, fe_now()), c); }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("faejinesc: EXCEPTION 0x%08lX in the stamina coefficient hook (coefficient left unchanged)", GetExceptionCode()); }
    return c;
}
static void hk_fe_apply(void *dc, float cost) {
    __try {
        LONG n = InterlockedIncrement(&g_fe_logn);
        if (n <= 4000) { void *own = *(void **)((uint8_t *)dc + 0xE0); char nm[96] = "?"; se_name_of(own, nm, sizeof nm); FeSlot *sl = fe_slot(own, 0);
            logf_("faejinesc: charge #%ld owner=%s style=%d cost=%.2f chain=%d n_eff=%.2f", (long)n, nm, hp_is_fchar(own) ? fe_style_of(own) : -1, cost, hp_is_fchar(own) ? fe_chain_of(own) : -1, sl ? fe_n_eff(&g_fe_cfg, &sl->s, fe_now()) : 0.0); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    o_fe_apply(dc, cost);
}
// ----- window shrink hooks
static void hk_fe_notify(void *nt, void *mesh, void *seq, uint8_t *info) {
    o_fe_notify(nt, mesh, seq, info);
    __try {
        if (!g_ws_cfg.enabled || !se_valid_ptr(mesh) || !se_valid_ptr(info)) return;
        void *ch = *(void **)((uint8_t *)mesh + 0xE0); if (!hp_is_fchar(ch) || ch != cd_local_pawn()) return;
        void *ld = *(void **)((uint8_t *)nt + 0x38); char nm[96] = ""; se_name_of(ld, nm, sizeof nm);
        static int seen; if (seen < 12) { seen++; logf_("faejinws: availability layer notify '%s' active %d fromOrder %d prediction %d", nm, info[0x14], info[0x28], info[0x29]); }
        if (!strstr(nm, "PerfectAttackOnly")) return;
        FeSlot *sl = fe_slot(ch, 1);
        if (info[0x14]) { if (info[0x28] && !sl->open) { sl->open = 1; sl->t_open = fe_now(); } } else sl->open = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("faejinws: EXCEPTION 0x%08lX in the layer notify hook", GetExceptionCode()); }
}
static void hk_fe_stack(void *det, int action) {
    o_fe_stack(det, action);
    __try { int a = action & 0xFF; if (g_ws_cfg.enabled && (a == 0 || a == 1 || a == 19 || a == 20 || a == 74 || a == 75)) g_ws_press_t = fe_now(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static char hk_fe_cpl(void *comp, void *row) {
    char r = o_fe_cpl(comp, row);
    if (!r || !g_ws_cfg.enabled) return r;
    __try {
        void *ch = *(void **)((uint8_t *)comp + 0xE0); if (!hp_is_fchar(ch) || ch != cd_local_pawn()) return r;
        int st = fe_style_of(ch); if (st != g_ws_cfg.style) return r;
        FeSlot *sl = fe_slot(ch, 1); double now = fe_now(), n = fe_n_eff(&g_fe_cfg, &sl->s, now);
        { uint8_t *rw = (uint8_t *)row; int next_ab = se_valid_ptr(rw) && (rw[0x1BC] || rw[0x1BD]); if (!next_ab && !sl->s.last_ability) return r; }     // a plain attack-to-attack gold link is not touched
        int have = sl->open && g_ws_press_t > 0 && g_ws_press_t >= sl->t_open - 1.0; double off = have ? (g_ws_press_t - sl->t_open) * 30.0 : 0.0;
        int ok = ws_accept(&g_ws_cfg, st, n, have, off);
        if (ok) InterlockedIncrement(&g_ws_ok); else InterlockedIncrement(&g_ws_veto);
        LONG k = InterlockedIncrement(&g_ws_nolog);
        if (k <= 4000) logf_("faejinws: %s gold link n=%.2f press-open=%+.2f F (have %d, eval-open %+.2f F, width %.2f F)", ok ? "ALLOW" : "VETO", n, off, have, sl->open ? (now - sl->t_open) * 30.0 : -99.0, ws_width(&g_ws_cfg, n));
        return ok ? r : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("faejinws: EXCEPTION 0x%08lX in CanPerfectLink hook (answer left unchanged)", GetExceptionCode()); return r; }
}
static int fe_row_ability(void *ch, const char *attack) {       // 1 = the attack row is a style-ability row (m_bIsDrunkenAttack +0x1BC / m_bIsSpecialAbilityAttack +0x1BD), 0 = normal, -1 = unknown
    void *ac = hp_comp(ch, &g_fe_off_ac, "m_AttackComponent"); if (!ac || !attack || !attack[0]) return -1;
    void **tbl = api_p_prop_ptr(FE_API, ac, "m_AnimDBXls"); char tp[300] = ""; if (!tbl || !se_valid_ptr(*tbl)) return -1;
    se_path_of(*tbl, tp, sizeof tp); uint8_t *row = tp[0] ? api_p_table_row(FE_API, tp, attack) : NULL;
    return row ? (row[0x1BC] || row[0x1BD]) : -1;
}
static void fe_on_attack(void *ch, int id, const char *attack, void *u) {
    (void)u; (void)id;
    __try {
        if (!hp_is_fchar(ch) || ch != cd_local_pawn()) return; int st = fe_style_of(ch); if (st != g_fe_cfg.style) return;
        int chain = fe_chain_of(ch); if (chain < 0) return; FeSlot *sl = fe_slot(ch, 1); double now = fe_now();
        int ab = fe_row_ability(ch, attack), rose = fe_chain_rose(sl->s.last_chain, chain);
        if (rose && (ab > 0 || sl->s.last_ability)) {                              // a gold link into or out of a style ability: this is the loop the brakes count
            fe_link(&g_fe_cfg, &sl->s, now); LONG n = InterlockedIncrement(&g_fe_links);
            if (n <= 2000) logf_("faejinesc: gold link #%ld attack=%s (ability %d, previous ability %d) chain=%d n=%.2f mult now %.3f", (long)n, attack, ab, sl->s.last_ability, chain, sl->s.n, fe_mult(&g_fe_cfg, &sl->s, st, now));
        }
        sl->s.last_chain = chain; sl->s.last_ability = ab > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("faejinesc: EXCEPTION 0x%08lX in on_attack_start", GetExceptionCode()); }
}
static void fe_on_hit(const AbsHitInfo *h, void *u) {                              // being hit ends the loop: counter back to 0
    (void)u;
    __try {
        if (!h || !h->victim || h->victim != cd_local_pawn()) return; FeSlot *sl = fe_slot(h->victim, 0);
        if (sl && sl->s.n > 0) { logf_("faejinesc: the player was hit (outcome %d): loop counter %.2f reset", h->outcome, sl->s.n); fe_reset(&sl->s); sl->s.last_chain = 0; sl->s.last_ability = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static int fe_verified_fn(const char *name, void **fn) {          // call-only symbol: the loaded bytes must equal symbols.bin's (from the exe on disk)
    uint32_t rva, st; uint8_t b[24]; if (!se_sym_lookup(name, &rva, &st, b)) { logf_("faejinesc: symbol %s not in symbols.bin", name); return 0; }
    if (memcmp(g_base + rva, b, st) != 0) { logf_("faejinesc: %s bytes differ from this build - not used", name); return 0; }
    *fn = g_base + rva; return 1;
}
static double fe_cfg_num(const char *key, double dflt) { char v[32] = ""; wchar_t src[MAX_PATH]; return setting_get(key, L"", 0, v, sizeof v, src) && v[0] ? atof(v) : dflt; }
static int fe_cfg_on(const char *key) { char v[32] = ""; wchar_t src[MAX_PATH]; return setting_get(key, L"", 0, v, sizeof v, src) && (!_stricmp(v, "on") || !_stricmp(v, "1") || !_stricmp(v, "true")); }
static void fe_init(const AbsApi *a) {
    FE_API = a; g_fe_cfg = fe_cfg_default();
    g_fe_cfg.enabled = fe_cfg_on("fe_escalation"); g_fe_log = fe_cfg_on("fe_log");
    g_fe_cfg.style = (int)fe_cfg_num("fe_style", 5); g_fe_cfg.n0 = fe_cfg_num("fe_n0", 2.0); g_fe_cfg.a = fe_cfg_num("fe_a", 0.35); g_fe_cfg.cap = fe_cfg_num("fe_cap", 3.0);
    g_fe_cfg.tau = fe_cfg_num("fe_tau", 0.0); g_fe_cfg.t_reset = fe_cfg_num("fe_reset", 1.5);
    g_ws_cfg = ws_cfg_default(); g_ws_cfg.enabled = fe_cfg_on("fe_window"); g_ws_cfg.style = g_fe_cfg.style; g_ws_cfg.center = fe_cfg_num("fe_ws_center", 1.0);
    { char v[96] = ""; wchar_t src[MAX_PATH]; if (setting_get("fe_ws_widths", L"", 0, v, sizeof v, src) && v[0]) { g_ws_cfg.nw = 0; for (char *p = v; *p && g_ws_cfg.nw < WS_MAX;) { g_ws_cfg.w[g_ws_cfg.nw++] = atof(p); while (*p && *p != ',') p++; if (*p == ',') p++; } } }
    if (!g_fe_cfg.enabled && !g_fe_log && !g_ws_cfg.enabled) { g_fe_state = -1; logf_("faejinesc: off (config fe_escalation / fe_window / fe_log not set; nothing installed)"); return; }
    void *sf = NULL; int ok_style = fe_verified_fn("AFightingCharacter::BPF_GetCombatStyle", &sf); g_fe_style_fn = ok_style ? (FeStyle_t)sf : NULL;
    int ok_attack = api_p_on_attack_start(a, fe_on_attack, NULL); int ok_hit = api_p_on_hit(a, fe_on_hit, NULL); if (ok_hit) logf_("faejinesc: on_hit registration failed (%d): the counter will not reset when the player is hit", ok_hit);
    if (g_fe_cfg.enabled) o_fe_coef = (FeCoef_t)api_p_hook_symbol(a, "AFightingCharacter::GetActionStaminaCostCoefFromEffects", (void *)hk_fe_coef);
    if (g_fe_log) o_fe_apply = (FeApply_t)api_p_hook_symbol(a, "UDefenseComponent::ApplyActionCostGuardGauge", (void *)hk_fe_apply);
    if (g_ws_cfg.enabled) {
        o_fe_notify = (FeNotify_t)api_p_hook_symbol(a, "UAvailabilityLayerNotifyState::NotifyOutside", (void *)hk_fe_notify);
        o_fe_stack = (FeStack_t)api_p_hook_symbol(a, "APAttackDetection::OnActionJustStackedCallback", (void *)hk_fe_stack);
        o_fe_cpl = (FeCpl_t)api_p_hook_symbol(a, "UAttackComponent::CanPerfectLink", (void *)hk_fe_cpl);
        if (!o_fe_notify || !o_fe_stack || !o_fe_cpl || !g_fe_style_fn) { logf_("faejinws: a hook or the style read is missing (notify %p, stack %p, canperfectlink %p, style %p) - window shrink OFF", (void *)o_fe_notify, (void *)o_fe_stack, (void *)o_fe_cpl, (void *)g_fe_style_fn); g_ws_cfg.enabled = 0; }
        else { logf_("faejinws: window shrink ON style %d center %.2f F widths(%d) %.2f %.2f %.2f %.2f %.2f %.2f (frames per consecutive gold link, then none)", g_ws_cfg.style, g_ws_cfg.center, g_ws_cfg.nw, g_ws_cfg.w[0], g_ws_cfg.w[1], g_ws_cfg.w[2], g_ws_cfg.w[3], g_ws_cfg.w[4], g_ws_cfg.w[5]); }
    }
    int bad = !ok_style || (g_fe_cfg.enabled && !o_fe_coef) || (g_fe_log && !o_fe_apply) || ok_attack;
    logf_("faejinesc: escalation %s style %d n0 %.2f a %.2f cap %.2f tau %.2f s reset %.2f s | log %s | style fn %p, on_attack_start %d, coef hook %p, charge-log hook %p%s", g_fe_cfg.enabled ? "ON" : "off", g_fe_cfg.style, g_fe_cfg.n0, g_fe_cfg.a,
          g_fe_cfg.cap, g_fe_cfg.tau, g_fe_cfg.t_reset, g_fe_log ? "on" : "off", (void *)g_fe_style_fn, ok_attack, (void *)o_fe_coef, (void *)o_fe_apply, bad ? " - SOME PIECES MISSING (see above)" : "");
    if (g_fe_cfg.enabled && (!o_fe_coef || !g_fe_style_fn)) g_fe_cfg.enabled = 0;           // never half-on: without the verified hook / style read there is no escalation
    g_fe_state = bad ? -1 : 1;
}

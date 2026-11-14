// hitpre.inc - on_hit_pre + damage_mod, poise (API v2.19; included by content.inc after bots.inc: uses npcs.inc's api_p_npc_archetype).
// Pure maths and the per-character table: hitpre.h. docs\PLUGIN-API.md "Pre-hit, damage_mod and poise".
//
// One detour on UHitComponent::GenerateImpact (stacks with secore's on_hit detour) opens a context for the duration of the game's own hit
// resolution; small detours on the getters GenerateImpact calls apply the scales ONLY inside that context (same thread, same victim/attacker):
//   AFightingCharacter::GetReceivedDamageMultiplier (victim)      x damage scale  -> lands on the final damage (what on_hit reports)
//   UStatsComponent::BPF_GetAttackSpecialDamageRatio (attacker)   special share (weapon)        } scaled / forced, clamped 0..1
//   AFightingCharacter::GetBareHandSpecialRatioFromEffects        special share (bare hands)    }
//   AFightingCharacter::GetKnockbackCoefFromEffects (victim)      x knockback scale
//   AFightingCharacter::GetResilienceBonusFromEffects (victim)    poise: forced resilience (hitpre.h)
//   UHitComponent::NotifyResilienceImpact                         poise: "the game took the resilience branch" = light absorbed
// Stun: row bytes scaled for the one call (hitpre.h "stun").
// Nothing here allocates; with no callback, no damage_mod and no poise the GenerateImpact detour is one test and the call.
#include "hitpre.h"

static const AbsApi *HP_API;                                      // the loader's own plugin API (hp_init from content_init)
static HpTable g_hp; static unsigned g_hp_gen;                    // per-character multipliers / poise; gen = the world they belong to
#define HP_PLAYER ((void *)1)                                     // table key "the local player, whoever that is" (damage_mod / poise_set with character NULL); survives travel
typedef struct { int plugin; void (*cb)(AbsHitPre *, void *); void *user; } HpCb;
static HpCb g_hp_cb[16]; static int g_hp_ncb;
typedef struct { char id[64], mod[64], arch[300]; float poise; int heavy; } HpPoiseDef;   // "poise" content entries
static HpPoiseDef g_hp_pd[64]; static int g_hp_npd;
static int g_hp_hooks;                                            // 0 not tried, 1 installed, -1 failed (GenerateImpact or the damage getter missing)
static volatile LONG g_hp_logn;                                   // the first impacts are logged in full, then only poise events and every 100th
static int hp_log_ok(void) { LONG n = InterlockedIncrement(&g_hp_logn); return n <= 120 || n % 100 == 0; }

typedef struct HpCtx {
    void *hc, *victim, *attacker, *stats, *def; DWORD tid;
    HpScales s; float special_forced; int res_target, res_base, res_orig, res_absorbed; int special_seen, kb_seen, dmg_seen; float special_game, kb_game, recv_game;
} HpCtx;
static HpCtx *g_hp_cur;                                           // innermost open GenerateImpact (game thread); nested calls save / restore it
static int hp_in(const HpCtx *c) { return c && c->tid == GetCurrentThreadId(); }

// ----- characters: AFightingCharacter test (class pointer chain, cached) and cached component offsets
static void *g_hp_fc_cls; static int g_hp_off_ac = -1, g_hp_off_dc = -1, g_hp_off_sc = -1;
static int hp_is_fchar(void *o) {
    if (!g_hp_fc_cls) { static int tries; if (api_p_is_game_thread(NULL) && tries++ < 20) { g_hp_fc_cls = se_find_class("FightingCharacter"); logf_("hitpre: class FightingCharacter -> %p", g_hp_fc_cls); } }
    if (!se_valid_ptr(o) || !g_hp_fc_cls) return 0;
    for (uint8_t *c = *(uint8_t **)((uint8_t *)o + 0x10); se_valid_ptr(c); c = *(uint8_t **)(c + 0x30)) if (c == g_hp_fc_cls) return 1;
    return 0;
}
static void *hp_comp(void *ch, int *off, const char *prop) {      // component pointer at a cached property offset (resolved by reflection on first use)
    if (*off < 0) { void **p = api_p_prop_ptr(NULL, ch, prop); if (!p) return NULL; *off = (int)((uint8_t *)p - (uint8_t *)ch); logf_("hitpre: %s at +0x%X", prop, *off); }
    void *c = *(void **)((uint8_t *)ch + *off); return se_valid_ptr(c) ? c : NULL;
}
static void hp_world_check(void) {                                // a new world: every character entry except "the player" goes
    if (g_hp_gen == g_se_world_gen) return; g_hp_gen = g_se_world_gen; int n = 0;
    for (int i = 0; i < HP_CHARS; i++) if (g_hp.c[i].ch && g_hp.c[i].ch != HP_PLAYER) { memset(&g_hp.c[i], 0, sizeof g_hp.c[i]); n++; }
    if (n) logf_("hitpre: new world: %d character entr%s dropped", n, n == 1 ? "y" : "ies");
}
static void hp_destroyed(void *ch, int reason, void *u) { (void)u; HpChar *c = hp_find(&g_hp, ch); if (c) { logf_("hitpre: character %p removed (EndPlay %d): its multipliers / poise dropped", ch, reason); memset(c, 0, sizeof *c); } }
static float hp_mod_of(HpChar *own, HpChar *pl, int k) { return (own ? own->m[k] : 1.0f) * (pl ? pl->m[k] : 1.0f); }

// poise content entries are matched once per character (at its first hit), by its ArchetypeAsset name or path
static void hp_poise_match(void *ch) {
    HpChar *have = hp_find(&g_hp, ch); if (!g_hp_npd || (have && have->checked)) return;
    void *arch = api_p_npc_archetype(HP_API, ch); char name[128] = "", path[300] = "";
    if (arch) { se_name_of(arch, name, sizeof name); se_path_of(arch, path, sizeof path); }
    HpChar *c = hp_get(&g_hp, ch); if (!c) { if (!g_hp.full_logged++) logf_("hitpre: character table full (%d): %p gets no poise / multipliers", HP_CHARS, ch); return; }
    c->checked = 1;
    for (int i = g_hp_npd - 1; i >= 0; i--) if (hp_arch_match(g_hp_pd[i].arch, name, path)) {      // the last loaded entry wins
        hp_poise_set(&c->poise, g_hp_pd[i].poise, g_hp_pd[i].heavy);
        logf_("hitpre: poise %.0f (heavy power %d) on %p from entry %s/%s (archetype %s)", c->poise.max, c->poise.heavy_power, ch, g_hp_pd[i].mod, g_hp_pd[i].id, path); return;
    }
}

// ----- getter detours (only inside an open context; everything else passes straight through)
static float (*o_hp_recv)(void *, int64_t); static float (*o_hp_kb)(void *, int64_t); static float (*o_hp_bare)(void *, int64_t);
static float (*o_hp_wspec)(void *, uint64_t, uint64_t); static signed char (*o_hp_res)(void *, int64_t);
static void (*o_hp_resimp)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static float hk_hp_recv(void *ch, int64_t tick) {
    float r = o_hp_recv(ch, tick); HpCtx *c = g_hp_cur;
    if (hp_in(c) && ch == c->victim && !c->dmg_seen) { c->dmg_seen = 1; c->recv_game = r; r *= c->s.damage; }
    return r;
}
static float hk_hp_kb(void *ch, int64_t tick) {
    float r = o_hp_kb(ch, tick); HpCtx *c = g_hp_cur;
    if (hp_in(c) && ch == c->victim) { c->kb_seen++; c->kb_game = r; r *= c->s.knockback; }
    return r;
}
static float hp_spec(HpCtx *c, float r) { c->special_seen++; c->special_game = r; return c->s.special == 1.0f && c->special_forced < 0.0f ? r : hp_special(r, c->s.special, c->special_forced); }
static float hk_hp_wspec(void *stats, uint64_t a, uint64_t b) { float r = o_hp_wspec(stats, a, b); HpCtx *c = g_hp_cur; return hp_in(c) && c->stats && stats == c->stats ? hp_spec(c, r) : r; }
static float hk_hp_bare(void *ch, int64_t tick) { float r = o_hp_bare(ch, tick); HpCtx *c = g_hp_cur; return hp_in(c) && ch == c->attacker ? hp_spec(c, r) : r; }
static signed char hk_hp_res(void *ch, int64_t tick) {
    signed char r = o_hp_res(ch, tick); HpCtx *c = g_hp_cur;
    if (!hp_in(c) || ch != c->victim || c->res_target < 0) return r;
    c->res_orig = r; c->res_base = c->def ? *(int *)((uint8_t *)c->def + 0x260) : 0;          // DefenseComponent m_iResilience
    return hp_res_bonus(c->res_target, c->res_base);
}
static void hk_hp_resimp(uint64_t hc, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15) {
    HpCtx *c = g_hp_cur; if (hp_in(c) && (void *)(uintptr_t)hc == c->hc) c->res_absorbed = 1;     // 16 register / stack slots forwarded bit for bit (floats sit in stack slots)
    o_hp_resimp(hc, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
}

// ----- the GenerateImpact detour
typedef void *(*HpRowByName_t)(void *ac, uint64_t name, uint64_t item_only);
static HpRowByName_t g_hp_rowfn;
static GenImpact_t o_hp_gen;
static void hk_hp_gen(void *hc, void *attacker, uint64_t attack, uint64_t p3, uint64_t perfect, void *hitres, void *hitreq, float *dmg, uint8_t *guarded, uint8_t *broken, uint8_t *absorbed) {
    if (!g_hp_ncb && !g_hp_npd && !hp_count(&g_hp)) { o_hp_gen(hc, attacker, attack, p3, perfect, hitres, hitreq, dmg, guarded, broken, absorbed); return; }   // nothing configured
    HpCtx c; memset(&c, 0, sizeof c); int ok = 0; uint8_t *row = NULL, saved[HP_STUN_N]; HpChar *vc = NULL; char an[32] = "";
    __try {
        hp_world_check();
        c.hc = hc; c.victim = se_owner(hc); c.attacker = attacker; c.tid = GetCurrentThreadId(); c.special_forced = -1.0f; c.res_target = -1;
        if (hp_is_fchar(c.victim)) {
            void *pl = cd_local_pawn(); HpChar *ppl = hp_find(&g_hp, HP_PLAYER);
            hp_poise_match(c.victim);
            vc = hp_find(&g_hp, c.victim); HpChar *ac = hp_is_fchar(attacker) ? hp_find(&g_hp, attacker) : NULL;
            float vm[HPK_N], am[HPK_N];
            for (int k = 0; k < HPK_N; k++) { vm[k] = hp_mod_of(vc, c.victim == pl ? ppl : NULL, k); am[k] = hp_mod_of(ac, attacker == pl && pl ? ppl : NULL, k); }
            c.s = hp_combine(vm, am);
            if (hp_is_fchar(attacker)) c.stats = hp_comp(attacker, &g_hp_off_sc, "m_StatsComponent");
            int id = se_attack_name(attack, an, sizeof an);
            if (g_hp_ncb) {
                AbsHitPre h; memset(&h, 0, sizeof h); h.size = sizeof h; h.victim = c.victim; h.attacker = attacker; h.attack_id = id; memcpy(h.attack, an, sizeof h.attack); h.perfect_link = (uint8_t)perfect != 0;
                h.damage_scale = c.s.damage; h.special_ratio_scale = c.s.special; h.special_ratio = -1.0f; h.stun_scale = c.s.stun; h.knockback_scale = c.s.knockback;
                for (int i = 0; i < g_hp_ncb; i++) { HpCb *b = &g_hp_cb[i]; if (!plug_guard(b->plugin)) continue; __try { b->cb(&h, b->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(b->plugin, "on_hit_pre", GetExceptionCode()); } }
                // a plugin's out-of-range value is ignored (logged), never passed to the game
                if (hp_scale_ok(h.damage_scale)) c.s.damage = h.damage_scale; else logf_("hitpre: on_hit_pre: damage_scale %g refused (0..%g)", h.damage_scale, HP_SCALE_MAX);
                if (hp_scale_ok(h.special_ratio_scale)) c.s.special = h.special_ratio_scale; else logf_("hitpre: on_hit_pre: special_ratio_scale %g refused", h.special_ratio_scale);
                if (hp_scale_ok(h.stun_scale)) c.s.stun = h.stun_scale; else logf_("hitpre: on_hit_pre: stun_scale %g refused", h.stun_scale);
                if (hp_scale_ok(h.knockback_scale)) c.s.knockback = h.knockback_scale; else logf_("hitpre: on_hit_pre: knockback_scale %g refused", h.knockback_scale);
                if (h.special_ratio == h.special_ratio && h.special_ratio >= 0.0f) c.special_forced = h.special_ratio > 1.0f ? 1.0f : h.special_ratio;
            }
            vc = vc && vc->poise.max > 0.0f ? vc : c.victim == pl && ppl && ppl->poise.max > 0.0f ? ppl : NULL;   // the slot whose poise gauge this hit uses
            if (vc) { c.res_target = hp_poise_target(&vc->poise); c.def = hp_comp(c.victim, &g_hp_off_dc, "m_DefenseComponent"); }
            if (c.s.stun != 1.0f && g_hp_rowfn && hp_is_fchar(attacker)) {                       // melee rows only (an item attack has no FAttackDataRow: stun unchanged)
                void *atkc = hp_comp(attacker, &g_hp_off_ac, "m_AttackComponent");
                row = atkc ? g_hp_rowfn(atkc, attack, 0) : NULL;
                if (row) hp_stun_scale(row, c.s.stun, saved);
            }
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("hitpre: EXCEPTION 0x%08lX preparing a hit (passed through unchanged)", GetExceptionCode()); if (row) hp_stun_restore(row, saved); row = NULL; ok = 0; }
    HpCtx *prev = g_hp_cur; if (ok) g_hp_cur = &c;
    o_hp_gen(hc, attacker, attack, p3, perfect, hitres, hitreq, dmg, guarded, broken, absorbed);
    g_hp_cur = prev;
    if (row) hp_stun_restore(row, saved);
    if (!ok) return;
    __try {
        float d = dmg ? *dmg : 0.0f; int hit = d > 0.0f && !(guarded && *guarded) && !(broken && *broken) && !(absorbed && *absorbed);
        int ev = HPP_NONE; float left = 0.0f;
        if (vc && vc->ch && vc->poise.max > 0.0f) { ev = hp_poise_after(&vc->poise, hit, c.res_absorbed, d); left = vc->poise.cur; }
        if (ev != HPP_NONE || hp_log_ok()) {
            char v[96], a[96]; se_name_of(c.victim, v, sizeof v); se_name_of(attacker, a, sizeof a);
            logf_("hitpre: hit %s <- %s attack %s: damage %.3f (scale x%g, game recv mult %.3f%s), special x%g%s%s, stun x%g%s, knockback x%g (game coef %.3f, %d call%s), resilience %s, poise %s%s",
                  v, a, an, d, c.s.damage, c.recv_game, c.dmg_seen ? "" : " NOT SEEN", c.s.special, c.special_forced >= 0 ? " forced" : "", c.special_seen ? "" : " (no special getter call)",
                  c.s.stun, c.s.stun != 1.0f ? (row ? " (row bytes scaled)" : " (no attack row: unchanged)") : "", c.s.knockback, c.kb_game, c.kb_seen, c.kb_seen == 1 ? "" : "s",
                  c.res_target < 0 ? "game's" : c.res_absorbed ? "ABSORBED the hit" : "did not absorb", ev == HPP_NONE ? (vc && vc->poise.max > 0 ? "unchanged" : "-") : hp_poise_ev[ev], "");
            if (c.res_target >= 0) logf_("hitpre:   poise target resilience %d (base m_iResilience %d, game bonus %d), gauge %.1f / %.0f, heavy power %d", c.res_target, c.res_base, c.res_orig, left, vc->poise.max, vc->poise.heavy_power);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("hitpre: EXCEPTION 0x%08lX after a hit", GetExceptionCode()); }
}

// ----- install (first use; game thread)
static int hp_install(const AbsApi *caller) {     // caller: used when a plugin asks before the loader's own plugin ran hp_init (plugins load first)
    if (g_hp_hooks) return g_hp_hooks > 0;
    const AbsApi *s = HP_API ? HP_API : caller;
    if (!s) { logf_("hitpre: no API pointer yet: on_hit_pre / damage_mod / poise not installed (retried at the next use)"); return 0; }
    g_hp_hooks = -1;
    uint32_t rva = api_p_find_symbol(s, "UAttackComponent::GetAttackDataFromName"); g_hp_rowfn = rva ? (HpRowByName_t)(g_base + rva) : NULL;
    o_hp_recv = (float (*)(void *, int64_t))api_p_hook_symbol(s, "AFightingCharacter::GetReceivedDamageMultiplier", (void *)hk_hp_recv);
    o_hp_gen = o_hp_recv ? (GenImpact_t)api_p_hook_symbol(s, "UHitComponent::GenerateImpact", (void *)hk_hp_gen) : NULL;   // the context only opens when the damage getter is in place
    if (!o_hp_gen) { logf_("hitpre: GenerateImpact / GetReceivedDamageMultiplier hooks FAILED: on_hit_pre / damage_mod / poise are off"); return 0; }
    o_hp_kb = (float (*)(void *, int64_t))api_p_hook_symbol(s, "AFightingCharacter::GetKnockbackCoefFromEffects", (void *)hk_hp_kb);
    o_hp_wspec = (float (*)(void *, uint64_t, uint64_t))api_p_hook_symbol(s, "UStatsComponent::BPF_GetAttackSpecialDamageRatio", (void *)hk_hp_wspec);
    o_hp_bare = (float (*)(void *, int64_t))api_p_hook_symbol(s, "AFightingCharacter::GetBareHandSpecialRatioFromEffects", (void *)hk_hp_bare);
    o_hp_res = (signed char (*)(void *, int64_t))api_p_hook_symbol(s, "AFightingCharacter::GetResilienceBonusFromEffects", (void *)hk_hp_res);
    o_hp_resimp = (void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))api_p_hook_symbol(s, "UHitComponent::NotifyResilienceImpact", (void *)hk_hp_resimp);
    int pd = api_p_on_pawn_destroyed(s, hp_destroyed, NULL);
    logf_("hitpre: installed: GenerateImpact %p, damage %p, knockback %p, special weapon %p / bare %p, resilience %p / impact %p, row lookup %p, on_pawn_destroyed -> %d%s",
          (void *)o_hp_gen, (void *)o_hp_recv, (void *)o_hp_kb, (void *)o_hp_wspec, (void *)o_hp_bare, (void *)o_hp_res, (void *)o_hp_resimp, (void *)g_hp_rowfn, pd,
          !o_hp_res || !o_hp_resimp ? " - poise is OFF (a resilience hook is missing)" : "");
    g_hp_hooks = 1; return 1;
}

// ----- API v2.19
static int api_p_on_hit_pre(const AbsApi *self, void (*cb)(AbsHitPre *, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_hp_ncb >= 16) { logf_("hitpre: on_hit_pre from %s refused (bad arguments or 16 callbacks)", plug_name_of(p)); return 1; }
    if (!hp_install(self)) return 2;
    EnterCriticalSection(&g_lock); g_hp_cb[g_hp_ncb].plugin = p; g_hp_cb[g_hp_ncb].cb = cb; g_hp_cb[g_hp_ncb].user = user; g_hp_ncb++; LeaveCriticalSection(&g_lock);
    logf_("plugin %s: on_hit_pre registered", plug_name_of(p)); return 0;
}
static int hp_api_gate(const AbsApi *self, const char *what) {
    if (!api_p_is_game_thread(self)) { logf_("hitpre: %s from %s refused: not the game thread", what, plug_name_of(plug_of(self))); return 0; }
    if (!hp_install(self)) { logf_("hitpre: %s from %s refused: hooks not installed", what, plug_name_of(plug_of(self))); return 0; }
    hp_world_check(); return 1;
}
static int api_p_damage_mod(const AbsApi *self, void *ch, const char *key, float value) {
    if (!hp_api_gate(self, "damage_mod")) return 0;
    int k = hp_key(key); void *who = ch ? ch : HP_PLAYER;
    if (ch && !hp_is_fchar(ch)) { logf_("hitpre: damage_mod(%p, %s, %g) from %s refused: not a character", ch, key ? key : "?", value, plug_name_of(plug_of(self))); return 0; }
    int r = hp_set_mod(&g_hp, who, k, value);
    logf_("hitpre: damage_mod(%s, %s, %g) from %s -> %s", ch ? "character" : "player", key ? key : "?", value, plug_name_of(plug_of(self)),
          r ? "ok" : k < 0 ? "REFUSED (keys: damage_taken damage_dealt stun_taken stun_dealt knockback_taken knockback_dealt special_ratio)" : !hp_scale_ok(value) ? "REFUSED (value 0..100)" : "REFUSED (table full)");
    if (ch && r) logf_("hitpre:   character %p", ch);
    return r;
}
static int api_p_poise_set(const AbsApi *self, void *ch, float poise, int heavy_power) {
    if (!hp_api_gate(self, "poise_set")) return 0;
    if (!o_hp_res || !o_hp_resimp) { logf_("hitpre: poise_set refused: resilience hooks missing"); return 0; }
    void *who = ch ? ch : HP_PLAYER; if (ch && !hp_is_fchar(ch)) { logf_("hitpre: poise_set(%p) refused: not a character", ch); return 0; }
    if (!(poise == poise) || poise > 1000000.0f) { logf_("hitpre: poise_set(%g) refused", poise); return 0; }
    HpChar *c = poise > 0.0f ? hp_get(&g_hp, who) : hp_find(&g_hp, who);
    if (!c) { if (poise > 0.0f) logf_("hitpre: poise_set refused: table full"); return poise <= 0.0f; }
    hp_poise_set(&c->poise, poise, heavy_power); c->checked = 1;                       // a plugin's poise beats a content entry
    logf_("hitpre: poise_set(%s %p, %g, heavy power %d) from %s", ch ? "character" : "player", ch, c->poise.max, c->poise.heavy_power, plug_name_of(plug_of(self)));
    return 1;
}
static float api_p_poise_get(const AbsApi *self, void *ch) { (void)self; HpChar *c = hp_find(&g_hp, ch ? ch : HP_PLAYER); return c && c->poise.max > 0.0f ? c->poise.cur : -1.0f; }
static void hp_fill_api(AbsApi *a) { a->on_hit_pre = api_p_on_hit_pre; a->damage_mod = api_p_damage_mod; a->poise_set = api_p_poise_set; a->poise_get = api_p_poise_get; }

// ----- content kind "poise": { "archetype": "<npcs / bots entry id, archetype name or path>", "poise": 30, "heavy_power": 2 }
static int hp_parse(const char *json, HpPoiseDef *o, char *err, int cap) {
    JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    const char *a = cs_str(&d, 0, "archetype"), *p = cs_str(&d, 0, "poise"), *h = cs_str(&d, 0, "heavy_power"); int ok = 1;
    double pv = p ? atof(p) : -1, hv = h ? atof(h) : HP_HEAVY_DEFAULT;
    if (!a || !a[0] || strlen(a) >= sizeof o->arch) { snprintf(err, cap, "needs \"archetype\": an npcs / bots entry id, a stock archetype name (Trickster) or its path"); ok = 0; }
    else if (!(pv > 0 && pv <= 1000000)) { snprintf(err, cap, "\"poise\" must be a number > 0 (damage the gauge absorbs before a hit interrupts)"); ok = 0; }
    else if (!(hv >= 0 && hv <= 3) || hv != (int)hv) { snprintf(err, cap, "\"heavy_power\" must be 0..3 (EAttackPowers: 0 Light, 1 Medium, 2 Strong = default; hits at or above it interrupt)"); ok = 0; }
    if (ok) { snprintf(o->arch, sizeof o->arch, "%s", a); o->poise = (float)pv; o->heavy = (int)hv; }
    jdoc_free(&d); return ok;
}
static int hp_poise_validate(const char *json, char *err, int cap, void *u) { (void)u; HpPoiseDef t; return hp_parse(json, &t, err, cap); }
static int hp_poise_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; HpPoiseDef t; char err[200]; memset(&t, 0, sizeof t);
    if (!hp_parse(json, &t, err, sizeof err)) { logf_("hitpre: poise entry %s/%s: %s", mod, id, err); return 0; }
    snprintf(t.mod, sizeof t.mod, "%s", mod); snprintf(t.id, sizeof t.id, "%s", id);
    int i = 0; while (i < g_hp_npd && strcmp(g_hp_pd[i].id, id)) i++;                  // hot reload: same id = replace
    if (i >= 64) { logf_("hitpre: poise entry %s/%s dropped: 64 entries max", mod, id); return 0; }
    g_hp_pd[i] = t; if (i == g_hp_npd) g_hp_npd++;
    for (int k = 0; k < HP_CHARS; k++) if (g_hp.c[k].ch && g_hp.c[k].ch != HP_PLAYER && g_hp.c[k].poise.max <= 0) g_hp.c[k].checked = 0;   // re-match characters without poise
    for (int k = 0; k < HP_CHARS; k++) if (g_hp.c[k].ch && hp_char_default(&g_hp.c[k])) memset(&g_hp.c[k], 0, sizeof g_hp.c[k]);
    int ins = hp_install(HP_API);
    logf_("hitpre: poise entry %s/%s: archetype '%s' poise %.0f heavy power %d (%d entr%s)%s", mod, id, t.arch, t.poise, t.heavy, g_hp_npd, g_hp_npd == 1 ? "y" : "ies", ins ? "" : " - hooks NOT installed, poise is off");
    if (ins && (!o_hp_res || !o_hp_resimp)) logf_("hitpre: poise entries are inactive: resilience hooks missing");
    return 0;
}
static void hp_init(const AbsApi *a) { HP_API = a; logf_("hitpre: ready (hooks are installed at the first on_hit_pre / damage_mod / poise_set / poise entry)"); }

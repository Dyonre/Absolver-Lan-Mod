// defense_ev.inc - on_defense: defence outcome events (guard, guard break, absorb, parry, deflect, dodge, avoid) with the attack row, attacker,
// defender and timing (API v2.20; included by content.inc after hitpre.inc: uses hitpre's hp_is_fchar / hp_comp).
// Pure logic: defense_ev.h. docs\PLUGIN-API.md "Defence events".
//
// Sources (all installed at the first on_defense registration; each stacks with the hooks already there):
//   on_hit (secore, loader's own registration)          guard / guard break / absorb, from GenerateImpact's out flags (outcome)
//   UDefenseComponent::NotifyParrySuccessful (own hook)   parry / deflect. this = the ATTACKER's defense component, actor = the parrier (HandleParried).
//                                                         NOTE secore's on_hit ABS_HIT_PARRIED reports owner(this) as victim, i.e. victim / attacker swapped.
//   UDefenseComponent::NotifyAvoidSuccessful (own hook)   dodge / avoid, with the EAvoidType
//   on_attack_start (secore)                              start time per attacker -> "time into the attack" when the defence happens
// Nothing allocates; with no on_defense callback none of this is installed.
#include "defense_ev.h"

static const AbsApi *DE_API;                                       // the loader's own plugin API (de_init from content_init)
typedef struct { int plugin; void (*cb)(const AbsDefenseInfo *, void *); void *user; } DeCb;
static DeCb g_de_cb[16]; static int g_de_ncb;
static int g_de_hooks;                                             // 0 not tried, 1 installed, -1 failed
static DeStarts g_de_starts; static unsigned g_de_gen;
static volatile LONG g_de_logn; static LONG g_de_count[DE_N];
static int g_de_off_ac = -1, g_de_off_dc = -1;
static double de_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
static void de_world_check(void) { if (g_de_gen != g_se_world_gen) { g_de_gen = g_se_world_gen; memset(&g_de_starts, 0, sizeof g_de_starts); } }

static int de_startup(void *attacker, const char *attack) {         // the row's m_iFrameBuildUp (AttackDataRow +0x10C), -1 = no row (item attack, not a character)
    if (!hp_is_fchar(attacker) || !attack[0]) return -1;
    void *ac = hp_comp(attacker, &g_de_off_ac, "m_AttackComponent"); if (!ac) return -1;
    void **tbl = api_p_prop_ptr(DE_API, ac, "m_AnimDBXls"); char tp[300] = "";
    if (!tbl || !se_valid_ptr(*tbl)) return -1;
    se_path_of(*tbl, tp, sizeof tp);
    uint8_t *row = tp[0] ? api_p_table_row(DE_API, tp, attack) : NULL;
    return row ? *(int *)(row + 0x10C) : -1;
}
static void de_fire(int type, void *defender, void *attacker, int attack_id, const char *attack, int avoid_type, int perfect, float delay) {
    if (type < 0 || type >= DE_N) return;
    __try {
        de_world_check();
        AbsDefenseInfo d; memset(&d, 0, sizeof d); d.size = sizeof d; d.type = type; d.defender = defender; d.attacker = attacker; d.attack_id = attack_id;
        snprintf(d.attack, sizeof d.attack, "%s", attack ? attack : ""); d.avoid_type = avoid_type; d.perfect_link = perfect; d.delay = delay;
        double age = de_start_age(&g_de_starts, attacker, d.attack, de_now());
        d.time_in_attack = (float)age; d.frame_in_attack = (float)de_frames(age); d.startup = de_startup(attacker, d.attack);
        LONG n = InterlockedIncrement(&g_de_logn); InterlockedIncrement(&g_de_count[type]);
        if (n <= 150 || n % 100 == 0) {
            char dn[96], an[96]; se_name_of(defender, dn, sizeof dn); se_name_of(attacker, an, sizeof an);
            logf_("defense: #%ld %s defender=%s attacker=%s attack=%s (%d) startup=%d t=%.3f s (frame %.1f) avoid_type=%d perfect=%d delay=%.3f", (long)n, de_name(type), dn, an, d.attack, attack_id,
                  d.startup, d.time_in_attack, d.frame_in_attack, avoid_type, perfect, delay);
        }
        for (int i = 0; i < g_de_ncb; i++) { DeCb *b = &g_de_cb[i]; if (!plug_guard(b->plugin)) continue; __try { b->cb(&d, b->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(b->plugin, "on_defense", GetExceptionCode()); } }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("defense: EXCEPTION 0x%08lX building a %s event", GetExceptionCode(), de_name(type)); }
}

// ----- sources
static void de_on_hit(const AbsHitInfo *h, void *u) { (void)u; int t = de_from_impact(h->outcome == ABS_HIT_GUARDED, h->outcome == ABS_HIT_GUARD_BROKEN, h->outcome == ABS_HIT_ABSORBED); if (t >= 0) de_fire(t, h->victim, h->attacker, h->attack_id, h->attack, -1, h->perfect_link, 0.0f); }
static void de_on_attack(void *ch, int id, const char *attack, void *u) { (void)u; (void)id; de_world_check(); de_start_note(&g_de_starts, ch, attack, de_now()); }
static void de_destroyed(void *ch, int reason, void *u) { (void)u; (void)reason; de_start_drop(&g_de_starts, ch); }
typedef void (*DeParry_t)(void *, float, void *, uint64_t, uint64_t);
typedef void (*DeAvoid_t)(void *, float, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void *);
static DeParry_t o_de_parry; static DeAvoid_t o_de_avoid;
static void hk_de_parry(void *dc, float delay, void *parrier, uint64_t attack, uint64_t order) {
    o_de_parry(dc, delay, parrier, attack, order);
    __try {
        char an[32]; int id = se_attack_name(attack, an, sizeof an); int guarding = 0;
        void *pdc = hp_is_fchar(parrier) ? hp_comp(parrier, &g_de_off_dc, "m_DefenseComponent") : NULL;
        if (pdc) api_p_get_bool(DE_API, pdc, "m_bIsGuarding", &guarding);
        de_fire(de_from_parry(guarding), parrier, se_owner(dc), id, an, -1, 0, delay);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("defense: EXCEPTION 0x%08lX in NotifyParrySuccessful", GetExceptionCode()); }
}
static void hk_de_avoid(void *dc, float delay, void *attacker, uint64_t type, uint64_t otype, uint64_t order, uint64_t attack, uint64_t exec, void *db) {
    o_de_avoid(dc, delay, attacker, type, otype, order, attack, exec, db);
    __try {
        char an[32]; int id = se_attack_name(attack, an, sizeof an); int at = (int)(uint8_t)type, t = de_from_avoid(at);
        if (t < 0) { logf_("defense: NotifyAvoidSuccessful with avoid type %d ignored", at); return; }
        de_fire(t, se_owner(dc), attacker, id, an, at, 0, delay);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("defense: EXCEPTION 0x%08lX in NotifyAvoidSuccessful", GetExceptionCode()); }
}

static int de_install(const AbsApi *caller) {      // caller: used when a plugin registers before the loader's own plugin ran de_init (plugins load first)
    if (g_de_hooks) return g_de_hooks > 0;
    const AbsApi *s = DE_API ? DE_API : caller;
    if (!s) { logf_("defense: no API pointer yet (retried at the next registration)"); return 0; }
    if (!DE_API) DE_API = s;
    g_de_hooks = -1;
    int hit = api_p_on_hit(s, de_on_hit, NULL), atk = api_p_on_attack_start(s, de_on_attack, NULL), pd = api_p_on_pawn_destroyed(s, de_destroyed, NULL);
    o_de_parry = (DeParry_t)api_p_hook_symbol(s, "UDefenseComponent::NotifyParrySuccessful", (void *)hk_de_parry);
    o_de_avoid = (DeAvoid_t)api_p_hook_symbol(s, "UDefenseComponent::NotifyAvoidSuccessful", (void *)hk_de_avoid);
    logf_("defense: installed: on_hit %d (guard / guard break / absorb), parry hook %p, avoid hook %p, on_attack_start %d (timing), on_pawn_destroyed %d%s", hit, (void *)o_de_parry, (void *)o_de_avoid, atk, pd,
          hit || !o_de_parry || !o_de_avoid ? " - SOME SOURCES MISSING (see above)" : "");
    if (hit && !o_de_parry && !o_de_avoid) return 0;
    g_de_hooks = 1; return 1;
}
static int api_p_on_defense(const AbsApi *self, void (*cb)(const AbsDefenseInfo *, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_de_ncb >= 16) { logf_("defense: on_defense from %s refused (bad arguments or 16 callbacks)", plug_name_of(p)); return 1; }
    if (!de_install(self)) return 2;
    EnterCriticalSection(&g_lock); g_de_cb[g_de_ncb].plugin = p; g_de_cb[g_de_ncb].cb = cb; g_de_cb[g_de_ncb].user = user; g_de_ncb++; LeaveCriticalSection(&g_lock);
    logf_("plugin %s: on_defense registered", plug_name_of(p)); return 0;
}
static void de_fill_api(AbsApi *a) { a->on_defense = api_p_on_defense; }
static void de_init(const AbsApi *a) { DE_API = a; logf_("defense: ready (hooks are installed at the first on_defense registration)"); }

// weapons.inc - content kind "weapons" (schema + pure logic in weapons.h). Included by content.inc after the CItem type, before weaponslot.inc.
//   * items.weapon -> wp_item_merge (called from items_parse; merge rules: weapons.h header)
//   * UI identity: the kind's own menu widgets (weaponslot.inc) show the kind's label and icon: around their Blueprint events the base family's entry of
//     MenuDB.m_WeaponFamilyInfos (FText label + selected / normal / mini icon) is byte-swapped with the kind's label and icon texture (wp_view_push / pop);
//     every other reader sees the vanilla family. A swap of the DATA, not a hook: the readers are native code that cannot be hooked
//     (BPF_GetEquipmentCategoryInfos: 143 bytes, jump in its prologue; the weapon list's section header copies the entry natively) plus Blueprint thunks.
//     Game thread only; wp_ui_sync (every 0.25 s, weaponslot deck watch) restores the vanilla data if the swap is left on.
//   * "defense": placeholder (a deflect defence): parsed and logged, nothing else.
#include "weapons.h"
#define WP_MAX 16
typedef struct { char mod[MOD_NAME_MAX]; WpKind k; char icon_path[300]; int gen; } CWeapon;
static CWeapon g_wp[WP_MAX]; static int g_wp_n;
static const char *ws_active_kind(void);                                  // weaponslot.inc: the kind of the item the local player has equipped (NULL = none)

// the winning (valid, not shadowed) "weapons" entry of that id; usable before the kind's own apply ran (items may come first in the load order)
static const char *wp_json(const char *id) {
    for (int i = 0; id && id[0] && i < g_ce_n; i++) { CEntry *e = &g_ce[i]; if (e->valid && !e->shadowed && e->json && !_stricmp(e->cr.kind, "weapons") && !strcmp(e->cr.id, id)) return e->json; }
    return NULL;
}
static CWeapon *wp_find(const char *id) { for (int i = 0; id && i < g_wp_n; i++) if (!strcmp(g_wp[i].k.id, id)) return &g_wp[i]; return NULL; }

static int wp_kind_validate(const char *json, char *err, int cap, void *u) {
    (void)u; WpKind k; if (!wp_parse(json, &k, err, (size_t)cap)) return 0;
    static const char *const known[] = { "id", "label", "base_family", "icon", "icon_file", "deck", "anim_set", "anim_style", "deflect_style", "moves", "moves_exclusive", "allow_bare_hands", "allow_sword_moves", "hand_switch", "damage", "grip", "defense", NULL };
    JDoc d; if (jdoc_parse(&d, json) == 0) cs_warn_unknown(&d, "weapons", k.id, known); jdoc_free(&d);
    return 1;
}

// ----- items.weapon: kind moves + gear values into the item (called by items_parse, also on a hot reload, so the item's "moves" binding stays comparable)
static void wp_item_merge(CItem *it) {
    if (!it->weapon[0]) return;
    const char *j = wp_json(it->weapon); WpKind k; char err[200];
    if (!j) { logf_("weapons: item %s: weapon kind '%s' not found (no valid \"weapons\" entry of that id) - the item stays a plain %s", it->id, it->weapon, "weapon"); return; }
    if (!wp_parse(j, &k, err, sizeof err)) { logf_("weapons: item %s: weapon kind '%s' does not parse: %s", it->id, it->weapon, err); return; }
    int n0 = it->nmoves; it->nmoves = wp_merge_moves(it->moves, it->nmoves, CI_MOVES_MAX, &k);
    if (it->nmoves == CI_MOVES_MAX) logf_("weapons: item %s: %d moves = CI_MOVES_MAX (kind moves past it are dropped)", it->id, CI_MOVES_MAX);
    int added = 0;
    if (k.ngs && !it->gtable[0]) logf_("weapons: item %s: kind '%s' has damage / grip values but the item has no \"gear_row\" - not applied (they live in the item's own gear row)", it->id, k.id);
    else if (k.ngs) it->ngs = wp_merge_gear(it->gsk, it->gsv, it->ngs, 16, &k, &added);
    logf_("weapons: item %s -> kind '%s' (label '%s', base family %s): moves %d own + kind = %d, gear values +%d of %d (item values win)%s%s", it->id, k.id, k.label, WP_FAMILY[k.family], n0, it->nmoves, added, k.ngs,
          k.socket[0] ? ", grip socket " : "", k.socket);
}

// ----- UI identity swap (MenuDB data, see the header)
typedef struct { uint8_t *at; uint8_t mine[0x28]; int n; } WpSwap;
static struct { void *db; char kind[64]; int gen, n, on; WpSwap s[8]; } g_wpu;
static void wp_flip(void) { for (int i = 0; i < g_wpu.n; i++) { WpSwap *s = &g_wpu.s[i]; for (int b = 0; b < s->n; b++) { uint8_t t = s->at[b]; s->at[b] = s->mine[b]; s->mine[b] = t; } } g_wpu.on = !g_wpu.on; }
static void wp_soft(uint8_t *o, uint64_t fn) { memset(o, 0, 0x28); *(int32_t *)o = -1; memcpy(o + 0x10, &fn, 8); }   // TSoftObjectPtr: weak ptr {-1, 0}, tag 0, FSoftObjectPath { FName, empty FString }
static void wp_reg(void *p, const void *mine, int n) { if (!p || g_wpu.n >= 8) return; WpSwap *s = &g_wpu.s[g_wpu.n++]; s->at = p; s->n = n; memcpy(s->mine, mine, (size_t)n); }
// the swap regions of MenuDB for kind w: m_WeaponFamilyInfos[family] (EquipmentCategoryInfos 0xC0: m_Name +0, m_SelectedIcon +0x48, m_Icon +0x70, m_MiniIcon +0x98)
static int wp_build(void *db, CWeapon *w) {
    g_wpu.n = 0; int fam = w->k.family; uint8_t txt[24], ic[0x28]; uint64_t fn = 0; int have_icon = w->icon_path[0] && !CA->fname_make(CA, w->icon_path, &fn);
    uint8_t *fi = CA->prop_ptr(CA, db, "m_WeaponFamilyInfos"); if (!fi) { logf_("weapons: UI: MenuDB has no m_WeaponFamilyInfos - no identity swap"); return 0; }
    if (CA->make_text(CA, w->k.label, txt)) { logf_("weapons: UI: make_text('%s') failed - no identity swap", w->k.label); return 0; }   // never released: one FText per build
    uint8_t *e = fi + (size_t)fam * 0xC0; wp_reg(e, txt, 24);
    if (have_icon) { wp_soft(ic, fn); wp_reg(e + 0x48, ic, 0x28); wp_reg(e + 0x70, ic, 0x28); wp_reg(e + 0x98, ic, 0x28); }
    logf_("weapons: UI: MenuDB %p: %d region(s) of family %s prepared for kind '%s' (label '%s', icon %s)", db, g_wpu.n, WP_FAMILY[fam], w->k.id, w->k.label, have_icon ? w->icon_path : "none: the family's own");
    return g_wpu.n;
}
// The base family keeps its own name + icon ALWAYS; the kind has its own entries (Combat Deck row, Attacks page button, weaponslot.inc).
// Those widgets read MenuDB from Blueprint events, so the swap is SCOPED: wp_view_push(kind, name) right before such an event of OUR widget runs,
// wp_view_pop() right after (on_call pre / post, game thread, nested calls counted). Outside those events every reader sees the vanilla family.
static int g_wpv_depth; static char g_wpv_name[64];
static int wp_view_push(const char *kind, const char *name) {                // always balanced with one wp_view_pop (depth counts calls, the flip happens at most once)
    if (g_wpv_depth++) return 1;
    CWeapon *w = wp_find(kind); if (!w) return 0;
    void *db = CA->find_object(CA, "/Game/DB/UI/MenuDB.MenuDB"); if (!db) return 0;
    if (db != g_wpu.db || strcmp(g_wpu.kind, w->k.id) || g_wpu.gen != w->gen || strcmp(g_wpv_name, name ? name : w->k.label)) {
        snprintf(g_wpv_name, sizeof g_wpv_name, "%s", name ? name : w->k.label); g_wpu.db = db; snprintf(g_wpu.kind, sizeof g_wpu.kind, "%s", w->k.id); g_wpu.gen = w->gen;
        char keep[64]; snprintf(keep, sizeof keep, "%s", w->k.label); snprintf(w->k.label, sizeof w->k.label, "%s", g_wpv_name); if (!wp_build(db, w)) g_wpu.n = 0; snprintf(w->k.label, sizeof w->k.label, "%s", keep);
    }
    if (!g_wpu.n) return 0;
    wp_flip(); return 1;
}
static void wp_view_pop(void) { if (g_wpv_depth > 0 && !--g_wpv_depth && g_wpu.on) wp_flip(); }
// called every 0.25 s: safety net, the scoped view must never stay on
static void wp_ui_sync(void) { if (g_wpu.on && !g_wpv_depth) { wp_flip(); logf_("weapons: UI: SAFETY: kind view '%s' was still on outside a widget event - vanilla MenuDB restored", g_wpu.kind); } }

// ----- hit sweep densifier (weapons.h wp_densify): UAttackComponent::RetrieveHitResult sweeps one sphere per FHitRequest.m_CastBoundaries entry (+0x50, TArray of
// start/end vectors). For a LONG weapon (neighbouring sample points more than WP_SWEEP_LONG uu apart: e.g. ~94 for a long pole weapon, ~42 for a sword) the
// array is replaced for the duration of the call by one with points every WP_SWEEP_GAP uu (our buffer; the engine only reads it), then put back.
// Keyed on point spacing, not on the kind (anything longer than a sword gets the denser sweep); make it a kind field if another long weapon needs other numbers.
#define WP_SWEEP_LONG 60.f
#define WP_SWEEP_GAP 40.f   // few sweeps per cast (each costs frame time); a character capsule is ~35 uu radius, so 40 uu spacing (+5 sphere radius) leaves no gap
typedef struct { float *data; int num, max; } WpCastArr;
static void (*g_o_retr)(void *, void *, uint8_t *, float, float); static long g_wps_n;
static int wp_overlap_on(void) { static int v = -1; if (v < 0) { char b[16] = ""; wchar_t src[MAX_PATH]; v = setting_get("wp_overlap", L"", 0, b, sizeof b, src) ? (atoi(b) != 0) : 1; logf_("weapons: hit sweep: overlap casts at every sampled pose %s (config wp_overlap)", v ? "ON" : "OFF"); } return v; }
static void wp_retr_det(void *ac, void *res, uint8_t *req, float a, float b) {
    WpCastArr *cb = req ? (WpCastArr *)(req + 0x50) : NULL; static float buf[64 * 6]; WpCastArr keep = { 0 }; int swapped = 0;
    __try {
        if (g_wp_n && cb && cb->data && cb->num >= 2 && cb->num <= 16) {
            float g = 0; for (int i = 0; i + 1 < cb->num; i++) { if (cb->num % 3 == 0 && (i + 1) % 3 == 0) continue; const float *p = cb->data + i * 6, *q = p + 6; float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2], d = sqrtf(dx * dx + dy * dy + dz * dz); if (d > g) g = d; }
            if (g > WP_SWEEP_LONG && g_wps_n < 3) { char line[900]; int o = 0; for (int i = 0; i < cb->num && o < 820; i++) { const float *p = cb->data + i * 6; o += snprintf(line + o, sizeof line - o, " [(%.0f %.0f %.0f)->(%.0f %.0f %.0f)]", p[0], p[1], p[2], p[3], p[4], p[5]); } logf_("weapons: hit sweep: casts in:%s", line); }
            if (g > WP_SWEEP_LONG) { int m = wp_densify(cb->data, cb->num, 3, WP_SWEEP_GAP, buf, 64);
                if (m > cb->num) { keep = *cb; cb->data = buf; cb->num = m; cb->max = m; swapped = 1; if (g_wps_n++ < 8) logf_("weapons: hit sweep: %d sample points %.0f uu apart -> %d points (every <= %.0f uu)", keep.num, g, m, WP_SWEEP_GAP); } }
        }
        // close range (config wp_overlap): a sphere SWEEP never reports a target it already overlaps at the start of the cast, so a target that is too close is missed: add a zero-length cast
        // (start == end = the current pose) for every sample, which the game runs as an overlap query (RetrieveHitResult 0x30A200: zero-length -> OverlapMultiByProfile)
        if (g_wp_n && wp_overlap_on() && cb && cb->data && cb->num >= 1 && cb->num <= 32) {
            int n = cb->num; if (!swapped) { keep = *cb; memcpy(buf, cb->data, (size_t)n * 6 * sizeof(float)); cb->data = buf; swapped = 1; }
            for (int i = 0; i < n; i++) { const float *p = cb->data + i * 6; float *o = buf + (size_t)(n + i) * 6; o[0] = o[3] = p[3]; o[1] = o[4] = p[4]; o[2] = o[5] = p[5]; }
            cb->num = 2 * n; cb->max = 2 * n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (swapped) { *cb = keep; swapped = 0; } }
    LARGE_INTEGER t0, t1; if (swapped) QueryPerformanceCounter(&t0);
    g_o_retr(ac, res, req, a, b);
    if (swapped) { *cb = keep; QueryPerformanceCounter(&t1); static LARGE_INTEGER fq; static double sum_us, max_us; static long n; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)fq.QuadPart; sum_us += us; if (us > max_us) max_us = us; if (++n % 300 == 0) { logf_("weapons: hit sweep cost: %ld densified call(s), avg %.0f us, max %.0f us (frame budget at 60 fps = 16700 us)", n, sum_us / n, max_us); } }
}
static void wp_sweep_hook(void) {
    static int done; if (done) return; done = 1;
    g_o_retr = (void (*)(void *, void *, uint8_t *, float, float))CA->hook_symbol(CA, "UAttackComponent::RetrieveHitResult", (void *)wp_retr_det);
    logf_("weapons: hit sweep densifier hook %p (long weapons: points > %.0f uu apart get one every %.0f uu)", (void *)g_o_retr, WP_SWEEP_LONG, WP_SWEEP_GAP);
}

// ----- mirrored hand strikes (weapons.h wp_mirror_fill): both hit-request builders read the weapon actor of the (mirror-
// swapped) strike slot from AFightingCharacter +0x1288 + slot*8 and give up when it is NULL. For a mirrored attack with exactly one hand actor, the empty hand
// slot holds the other hand's actor for the duration of the call (restored right after, game thread). OrderAttack +0x48 = its FNetOrderStructAttack
// (+0x10 attack FName, +0x18 mirror byte). The two hooks nest (NextInfoAndSend calls ComputeInfo twice): the inner call then sees both hands and does nothing.
#define WP_CHAR_ACTORS 0x1288
static void *ws_local_kind_obj(void);                                     // weaponslot.inc
static void (*g_o_hrci)(void *, void *, void *, void *, float, int); static void (*g_o_hrns)(void *, void *, void *, float, float); static long g_wpm_n;
static int wp_mirror_pre(void *order, void *ch, void ***slot_out) {
    *slot_out = NULL; if (!g_wp_n || !order || !ch) return 0;
    __try {
        uint8_t *net = *(uint8_t **)((uint8_t *)order + 0x48); if (!net) return 0;
        void **act = (void **)((uint8_t *)ch + WP_CHAR_ACTORS);
        int f = wp_mirror_fill(net[0x18] != 0, 1, act[0] != NULL, act[1] != NULL); if (f < 0) return 0;
        if (wp_mirror_fill(1, ch == cd_local_pawn() && ws_local_kind_obj() != NULL, act[0] != NULL, act[1] != NULL) < 0) return 0;   // kind gate checked only on this rare path (reflection lookups)
        act[f] = act[1 - f]; *slot_out = &act[f];
        static uint64_t last; uint64_t fn = *(uint64_t *)(net + 0x10); g_wpm_n++;   // one line per change of row (a strike makes several hit requests), at most 300
        if (fn != last) { static int lines; last = fn; char nm[64] = "?"; if (lines++ < 300) { CA->fname_text(CA, net + 0x10, nm, sizeof nm); logf_("weapons: mirror slot: row %s placed mirrored, %s had no weapon actor -> borrows the %s actor %p for its hit requests (%ld calls so far)", nm, f ? "RightHand" : "LeftHand", f ? "LeftHand" : "RightHand", act[f], g_wpm_n); } }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (*slot_out) { **slot_out = NULL; *slot_out = NULL; } return 0; }
}
static void wp_mirror_post(void **slot) { if (slot) *slot = NULL; }
static void wp_hrci_det(void *order, void *xf, void *ch, void *row, float r, int height) { void **s; wp_mirror_pre(order, ch, &s); g_o_hrci(order, xf, ch, row, r, height); wp_mirror_post(s); }
static void wp_hrns_det(void *order, void *ch, void *row, float a, float b) { void **s; wp_mirror_pre(order, ch, &s); g_o_hrns(order, ch, row, a, b); wp_mirror_post(s); }
static void wp_mirror_hook(void) {
    static int done; if (done) return; done = 1;
    char b[16] = ""; wchar_t src[MAX_PATH]; if (setting_get("wp_mirror_fix", L"", 0, b, sizeof b, src) && !atoi(b)) { logf_("weapons: mirror slot fix OFF (config wp_mirror_fix = 0)"); return; }
    g_o_hrci = (void (*)(void *, void *, void *, void *, float, int))CA->hook_symbol(CA, "OrderAttack::HitRequestComputeInfo", (void *)wp_hrci_det);
    g_o_hrns = (void (*)(void *, void *, void *, float, float))CA->hook_symbol(CA, "OrderAttack::HitRequestComputeNextInfoAndSend", (void *)wp_hrns_det);
    logf_("weapons: mirror slot fix hooks ComputeInfo %p, NextInfoAndSend %p (mirrored hand strikes of one-hand weapons use the weapon actor)%s", (void *)g_o_hrci, (void *)g_o_hrns, g_o_hrci && g_o_hrns ? "" : " - HOOK FAILED");
}

// ----- impact sound log: the inputs the game's hit-sound Blueprints use for a hit by the local player with a kind equipped (decoded tables: weapons.h wp_sound)
static void *ws_local_kind_obj(void);                                     // weaponslot.inc: the equipped kind item's object (NULL = none)
static void wp_on_hit(const AbsHitInfo *h, void *u) {
    (void)u; if (!h || !h->attacker || h->attacker != cd_local_pawn() || !ws_local_kind_obj()) return;
    logf_("weapons: link %s row %s (outcome %d)", h->perfect_link ? "gold" : "plain", h->attack, h->outcome);   // whether this hit was a perfect (timed) link
    if (h->outcome == ABS_HIT_DODGED || h->outcome == ABS_HIT_PARRIED || h->outcome == ABS_HIT_NO_EFFECT) {   // no damage: log the outcome and the range
        float pa[3] = { 0, 0, 0 }, pv[3] = { 0, 0, 0 }, dist = -1; if (CA->test_get_location(CA, h->attacker, pa) && h->victim && CA->test_get_location(CA, h->victim, pv)) dist = sqrtf((pa[0] - pv[0]) * (pa[0] - pv[0]) + (pa[1] - pv[1]) * (pa[1] - pv[1]));
        logf_("weapons: hit result: row %s %s dist %.0f (no damage)", h->attack, h->outcome == ABS_HIT_DODGED ? "DODGED" : h->outcome == ABS_HIT_PARRIED ? "PARRIED" : "NO EFFECT", dist); return; }
    uint8_t *row = CA->table_row(CA, CT_ATTACKS, h->attack); int slot = row ? row[0x108] : -1;                // AttackDataRow.m_eWeaponSlotAttacking1 (+0x108, SDK)
    void *hv = NULL, *fh = CA->find_function(CA, h->attacker, "BPF_GetHandledVisibleWeapon"); int fam = -1; char fs[32] = ""; if (fh) CA->call_function_params(CA, h->attacker, fh, NULL, &hv);
    if (hv && CA->get_property_text(CA, hv, "m_eWeaponFamily", fs, sizeof fs) >= 0) fam = wp_family_of(strstr(fs, "::") ? strstr(fs, "::") + 2 : fs);
    unsigned char sl = (unsigned char)(slot >= 0 ? slot : 7), surf = 0xFF; void *fn = CA->find_function(CA, h->attacker, "BPF_GetSurfaceTypeFromWeaponSlot"); void *a[1] = { &sl };
    if (fn) CA->call_function_params(CA, h->attacker, fn, a, &surf);
    unsigned char s0 = 0, surf0 = 0xFF; void *a0[1] = { &s0 }; if (fn) CA->call_function_params(CA, h->attacker, fn, a0, &surf0);
    const char *at, *hm, *wm; wp_sound(slot, fam, surf, &at, &hm, &wm); const char *wm0; { const char *x, *y; wp_sound(0, fam, surf0, &x, &y, &wm0); }
    float pa[3] = { 0, 0, 0 }, pv[3] = { 0, 0, 0 }, dist = -1; if (CA->test_get_location(CA, h->attacker, pa) && h->victim && CA->test_get_location(CA, h->victim, pv)) dist = sqrtf((pa[0] - pv[0]) * (pa[0] - pv[0]) + (pa[1] - pv[1]) * (pa[1] - pv[1]));   // range of the hit
    logf_("weapons: impact sound: row %s outcome %d dmg %.1f dist %.0f, weapon slot %d, in-hand family %d, surface %d (slot 0: %d) -> Human_Foley_Fight_Hit with Wea_Attack_Type=%s Fight_Type_Hit_Mat=%s Fight_Type_Weapon_Mat=%s", h->attack, h->outcome, h->damage, dist, slot, fam, surf, surf0, at, hm, wm0);
}

// ----- independent weapons. Three switches in the loader config, ON by default, "= 0" turns one off.
//   wp_slot_follow = 1  effects follow the weapon (GetWeaponSlotAttacking 0x363D00; rule: weapons.h wp_slot_follow)
//   wp_hand_switch = 1  a kind with "hand_switch": "mirror": at each attack start the weapon actor is attached to LeftHandSocket for a MIRRORED attack and to
//                       RightHandSocket otherwise (K2_AttachToComponent, snap); back to the right hand when the attack ends. The actor keeps its RightHand slot entry; the
//                       hit request (mirror slot borrow) reads the actor's socket bone, so the sweep follows the hand that holds it.
//   wp_mesh_offset = 1  a kind's "grip.mesh_offset" / "grip.mesh_rotation": the weapon mesh (AVisibleWeaponActor +0x600) at stock + offset, re-applied
//                       when the game resets it (draw / re-attach).
#define WP_ACTOR_MESH 0x600
// default ON like wp_mirror_fix: each acts only for a weapon KIND (the json opts in: hand_switch, grip.mesh_offset); "<key> = 0" turns it off
static int wp_cfg(const char *key) { char b[16] = ""; wchar_t src[MAX_PATH]; int set = setting_get(key, L"", 0, b, sizeof b, src), on = set ? atoi(b) != 0 : 1; logf_("weapons: P7 %s = %s (%s)", key, on ? "ON" : "OFF", set ? "loader config" : "default; '= 0' in config.txt turns it off"); return on; }
static int g_wp7_follow = -1, g_wp7_hand = -1, g_wp7_mesh = -1;
static CWeapon *wp_active_w(void) { const char *k = ws_active_kind(); return k ? wp_find(k) : NULL; }
static uint8_t (*g_o_gwsa)(void *, void *, void *); static long g_wpf_n;
static uint8_t wp_gwsa_det(void *ch, void *ac, void *row) {
    uint8_t s = g_o_gwsa(ch, ac, row);
    if (s > 1 || !ch) return s;
    __try {
        void **act = (void **)((uint8_t *)ch + WP_CHAR_ACTORS); int f = wp_slot_follow(s, act[0] != NULL, act[1] != NULL); if (f == s) return s;
        if (ch != cd_local_pawn() || !ws_local_kind_obj()) return s;   // kind gate only on this rare path (reflection)
        if (g_wpf_n++ < 60) { logf_("weapons: slot follow: strike slot %s has no weapon actor -> reported as %s (the kind weapon's hand) [%ld]", s ? "RightHand" : "LeftHand", f ? "RightHand" : "LeftHand", g_wpf_n); }
        return (uint8_t)f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return s; }
}
// hand switch state (local player only)
typedef unsigned char (*WpIsMirror_t)(void *);
static WpIsMirror_t g_wp_ismir; static void *g_wph_actor; static int g_wph_left, g_wph_idle; static long g_wph_n; static ULONGLONG g_wph_start, g_wph_end;   // start / end = GetTickCount64 of the last attack start / BPE_AttackEnded of the local player
// the game attaches a hand weapon with AActor::AttachRootComponentTo(parent, socket, EAttachLocation::SnapToTarget, weld false) (SpawnAndAttachWeapon 0x34F920); we do the
// same call (0x1240610, checked against symbols.bin) with the SAME parent (the root component's current AttachParent) and the twin socket (a SwapWeaponHandIfNeeded-style hand swap)
typedef void (*WpAttachRoot_t)(void *actor, void *parent, uint64_t socket, uint8_t loc_type, uint8_t weld);
static WpAttachRoot_t g_wp_attach_root;
static int wp_attach(void *ch, void *actor, int left) {
    (void)ch; void **root = actor ? CA->prop_ptr(CA, actor, "RootComponent") : NULL; void **par = root && *root ? CA->prop_ptr(CA, *root, "AttachParent") : NULL; uint64_t *cur = root && *root ? CA->prop_ptr(CA, *root, "AttachSocketName") : NULL; uint64_t sock = 0;
    if (!g_wp_attach_root || !par || !*par || !cur || CA->fname_make(CA, left ? "LeftHandSocket" : "RightHandSocket", &sock)) { logf_("weapons: hand switch: cannot attach (attach fn %p, root %p, parent %p)", (void *)g_wp_attach_root, root ? *root : NULL, par ? *par : NULL); return 0; }
    char was[64] = "?", now[64] = "?"; CA->fname_text(CA, cur, was, sizeof was);
    __try { g_wp_attach_root(actor, *par, sock, 2, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("weapons: hand switch: EXCEPTION 0x%08lX in AttachRootComponentTo", GetExceptionCode()); return 0; }
    CA->fname_text(CA, cur, now, sizeof now); int ok = *cur == sock;
    if (g_wph_n++ < 80) logf_("weapons: hand switch: weapon actor %p socket %s -> %s (%s) [%ld]", actor, was, now, ok ? "ok" : "NOT CHANGED", g_wph_n);
    return ok;
}
static void wp_attack_start(void *ch, int id, const char *atk, void *u) {
    (void)id; (void)u; if (g_wp7_hand != 1 || !ch || ch != cd_local_pawn()) return;
    CWeapon *w = wp_active_w(); if (!w || !w->k.hand_switch) return;
    void **act = (void **)((uint8_t *)ch + WP_CHAR_ACTORS); void *actor = NULL; __try { actor = act[1] ? act[1] : act[0]; } __except (EXCEPTION_EXECUTE_HANDLER) { actor = NULL; }
    if (!actor) return;                                                     // sheathed: nothing in hand
    void *ac = cd_comp(ch, "m_AttackComponent"); int mir = 0; __try { mir = ac && g_wp_ismir ? g_wp_ismir(ac) != 0 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { mir = 0; }
    int want = wp_hand_want(w->k.hand_switch, mir);
    if (actor != g_wph_actor) { g_wph_actor = actor; g_wph_left = 0; }   // a new weapon actor (draw) starts in the right hand
    if (want != g_wph_left && wp_attach(ch, actor, want)) g_wph_left = want;
    g_wph_idle = 0; g_wph_start = GetTickCount64();
    if (g_wph_n < 80) logf_("weapons: hand switch: attack %s mirrored %d -> weapon in the %s hand", atk ? atk : "?", mir, g_wph_left ? "LEFT" : "right");
}
// OrderAttack::OnStop (0x46EA30) calls AFightingCharacter::BPE_AttackEnded(character) for a real attack end (re-simulated orders return before it): the "swap back at attack end"
static void wp_attack_ended(void *obj, void *func, void *params, void *u) { (void)func; (void)params; (void)u; if (g_wph_left && obj && obj == cd_local_pawn()) g_wph_end = GetTickCount64(); }
// 0.1 s: hand switch back after the attack, mesh offset (re)apply
static void wp_p7_tick(void *u) {
    (void)u; void *ch = cd_local_pawn(); if (!ch) { g_wph_actor = NULL; g_wph_left = 0; return; }
    CWeapon *w = wp_active_w(); void **act = (void **)((uint8_t *)ch + WP_CHAR_ACTORS); void *actor = NULL; __try { actor = act[1] ? act[1] : act[0]; } __except (EXCEPTION_EXECUTE_HANDLER) { actor = NULL; }
    if (actor != g_wph_actor) { g_wph_actor = actor; g_wph_left = 0; }
    if (g_wph_left && actor) {                                              // back to the right hand once no attack plays (weapons.h wp_hand_back: not on a combo's stale BPE_AttackEnded)
        static int quiet; int playing = -1; void *ac = cd_comp(ch, "m_AttackComponent"), *f = ac ? CA->find_function(CA, ac, "BPF_IsPlayingAttack") : NULL; unsigned char pl = 0;
        if (f && CA->call_function_params(CA, ac, f, NULL, &pl)) playing = pl != 0;
        quiet = playing == 0 ? quiet + 1 : 0;
        int ended = g_wph_end > g_wph_start && GetTickCount64() - g_wph_end >= 80;
        if (ended && playing > 0 && g_wph_n < 80) { logf_("weapons: hand switch: attack end event after the latest start, but an attack still plays: weapon stays in the LEFT hand [%ld]", g_wph_n); g_wph_n++; g_wph_end = 0; }
        if (wp_hand_back(g_wph_left, playing, quiet, ended)) {
            ULONGLONG held = GetTickCount64() - g_wph_start;
            if (wp_attach(ch, actor, 0)) g_wph_left = 0; g_wph_end = 0; quiet = 0;
            if (g_wph_n < 80) logf_("weapons: hand switch: back to the right hand %llu ms after the attack start (playing %d)", held, playing);
        }
    }
    if (g_wp7_mesh == 1 && w && actor && (w->k.has_mesh_off || w->k.has_mesh_rot)) {
        void *comp = NULL; __try { comp = *(void **)((uint8_t *)actor + WP_ACTOR_MESH); } __except (EXCEPTION_EXECUTE_HANDLER) { comp = NULL; }
        float *cl = comp ? (float *)CA->prop_ptr(CA, comp, "RelativeLocation") : NULL, *cr = comp ? (float *)CA->prop_ptr(CA, comp, "RelativeRotation") : NULL; if (!cl || !cr) return;
        static void *base_comp; static float bl[3], br[3]; if (comp != base_comp) { base_comp = comp; memcpy(bl, cl, 12); memcpy(br, cr, 12); logf_("weapons: mesh offset: weapon mesh %p stock loc (%.2f %.2f %.2f) rot (%.2f %.2f %.2f)", comp, bl[0], bl[1], bl[2], br[0], br[1], br[2]); }
        float tl[3], tr[3]; int diff = 0; for (int k = 0; k < 3; k++) { tl[k] = bl[k] + (w->k.has_mesh_off ? w->k.mesh_off[k] : 0); tr[k] = br[k] + (w->k.has_mesh_rot ? w->k.mesh_rot[k] : 0); if (fabsf(cl[k] - tl[k]) > 0.01f || fabsf(cr[k] - tr[k]) > 0.05f) diff = 1; }
        if (diff) { void *f = CA->find_function(CA, comp, "K2_SetRelativeLocationAndRotation"); unsigned char sweep = 0, tele = 1; static unsigned char hit[0x200]; memset(hit, 0, sizeof hit); void *a[5] = { tl, tr, &sweep, hit, &tele };
            int r = f ? CA->call_function_params(CA, comp, f, a, NULL) : 0; static long n; if (n++ < 20) logf_("weapons: mesh offset '%s': loc (%.2f %.2f %.2f) rot (%.2f %.2f %.2f) -> %d [%ld]", w->k.id, tl[0], tl[1], tl[2], tr[0], tr[1], tr[2], r, n); }
    }
}
// probe (config wp_probe_hvwa = 1, log only): AFightingCharacter::HandledVisibleWeaponIsAttacking (0x364E10) for the local player with a kind in hand: does the game count
// this strike as a WEAPON strike (true) or a bare-handed one (false)? Callers: GenerateImpact, OrderHitted::OnStart, GetCurrentHitDamage, weapon durability. Counts per mirror state.
static unsigned char (*g_o_hvwa)(void *, void *, void *); static long g_hv_cnt[2][2];
static unsigned char wp_hvwa_det(void *ch, void *ac, void *row) {
    unsigned char r = g_o_hvwa(ch, ac, row);
    __try { if (ch == cd_local_pawn() && ws_local_kind_obj()) { int mir = ac && g_wp_ismir ? g_wp_ismir(ac) != 0 : -1; if (mir >= 0) { long n = ++g_hv_cnt[mir][r != 0];
        if (n <= 6) logf_("weapons: probe HandledVisibleWeaponIsAttacking: %s attack -> %s (weapon strike counted: mirrored true %ld / false %ld, unmirrored true %ld / false %ld)", mir ? "MIRRORED" : "unmirrored", r ? "true" : "FALSE", g_hv_cnt[1][1], g_hv_cnt[1][0], g_hv_cnt[0][1], g_hv_cnt[0][0]); } } } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return r;
}
static void wp_p7_hook(void) {
    static int done; if (done) return; done = 1;
    g_wp7_follow = wp_cfg("wp_slot_follow"); g_wp7_hand = wp_cfg("wp_hand_switch"); g_wp7_mesh = wp_cfg("wp_mesh_offset");
    if (g_wp7_follow == 1) { static const uint8_t ex[16] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };   // 0x363D00 prologue (4 whole instructions)
        g_o_gwsa = (uint8_t (*)(void *, void *, void *))CA->hook_rva(CA, "AFightingCharacter::GetWeaponSlotAttacking(UAttackComponent)", 0x363D00, ex, 16, (void *)wp_gwsa_det);
        logf_("weapons: slot follow hook %p%s", (void *)g_o_gwsa, g_o_gwsa ? "" : " - HOOK FAILED (bytes differ?)"); }
    { uint32_t ar = CA->find_symbol(CA, "AActor::AttachRootComponentTo"); if (ar == 0x1240610) g_wp_attach_root = (WpAttachRoot_t)(CA->game_base + ar); else if (g_wp7_hand == 1) { g_wp7_hand = 0; logf_("weapons: hand switch OFF: AActor::AttachRootComponentTo is at %#x, expected 0x1240610", ar); } }
    { uint32_t rva = CA->find_symbol(CA, "UAttackComponent::IsAttackMirror");               // 0x300E80 in symbols.bin (PDB, 345 bytes); called directly, never patched
      if (rva == 0x300E80) g_wp_ismir = (WpIsMirror_t)(CA->game_base + rva); else if (g_wp7_hand == 1) { g_wp7_hand = 0; logf_("weapons: hand switch OFF: UAttackComponent::IsAttackMirror is at %#x, expected 0x300E80 (symbols.bin missing / other build)", rva); } }
    if (g_wp7_hand == 1) logf_("weapons: hand switch on attack start: event %d, attack end (BPE_AttackEnded) %d", CA->on_attack_start(CA, wp_attack_start, NULL), CA->on_call(CA, NULL, "BPE_AttackEnded", NULL, wp_attack_ended, NULL));
    { char b[16] = ""; wchar_t src[MAX_PATH]; if (setting_get("wp_probe_hvwa", L"", 0, b, sizeof b, src) && atoi(b) && g_wp_ismir) { static const uint8_t ex[14] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x83,0xB9,0x80,0x1D,0x00,0x00,0x00 };
        g_o_hvwa = (unsigned char (*)(void *, void *, void *))CA->hook_rva(CA, "AFightingCharacter::HandledVisibleWeaponIsAttacking", 0x364E10, ex, 14, (void *)wp_hvwa_det); logf_("weapons: probe HandledVisibleWeaponIsAttacking hook %p", (void *)g_o_hvwa); } }
    if (g_wp7_hand == 1 || g_wp7_mesh == 1) logf_("weapons: P7 0.1 s watch: timer %d", CA->set_timer_id(CA, 0.1f, 1, wp_p7_tick, NULL));
}

static int wp_kind_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; WpKind k; char err[200]; if (!wp_parse(json, &k, err, sizeof err)) { logf_("weapons: %s/%s: %s - not applied", mod, id, err); return 0; }
    wp_sweep_hook(); wp_mirror_hook(); wp_p7_hook();
    CWeapon *w = wp_find(k.id);
    if (!w) { if (g_wp_n >= WP_MAX) { logf_("weapons: too many weapon kinds (%d), %s ignored", WP_MAX, id); return 0; } w = &g_wp[g_wp_n++]; memset(w, 0, sizeof *w); }
    snprintf(w->mod, sizeof w->mod, "%s", mod); w->k = k; w->gen++; w->icon_path[0] = 0;
    if (k.icon[0]) snprintf(w->icon_path, sizeof w->icon_path, "%s", k.icon);
    if (k.icon_file[0]) {                                                  // runtime texture /Game/Mods/<mod>/<id>_WeaponIcon[<gen>] (a hot reload makes a new object)
        char file[MAX_PATH * 3 + 300], ms[MOD_NAME_MAX], tp[300], gen[16] = ""; cs_sanitize(mod, ms, sizeof ms); if (w->gen > 1) snprintf(gen, sizeof gen, "%d", w->gen);
        snprintf(file, sizeof file, "%s\\%s", g_ce_dir, k.icon_file); snprintf(tp, sizeof tp, "/Game/Mods/%s/%s_WeaponIcon%s.%s_WeaponIcon%s", ms, k.id, gen, k.id, gen);
        if (CA->texture_from_file(CA, file, tp)) snprintf(w->icon_path, sizeof w->icon_path, "%s", tp); else logf_("weapons: %s: icon_file %s could not be made into a texture - the family's own icon stays", k.id, file);
    }
    logf_("weapons: kind %s/%s: label '%s', base family %s, icon %s, %d move(s), %d gear value(s), deck %d%s", mod, k.id, k.label, WP_FAMILY[k.family], w->icon_path[0] ? w->icon_path : "-", k.nmoves, k.ngs, k.has_deck,
          k.has_defense ? ", \"defense\" present: reserved (not used)" : "");
    return 0;
}

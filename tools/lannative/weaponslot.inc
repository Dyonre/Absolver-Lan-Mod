// weaponslot.inc - "own weapon slot" for a weapon item of the content registry (included by content.inc after the CItem type).
// The weapon family enum is compiled (BareHands / Sword / WarGloves), so a weapon kind stays in its base family (e.g. Sword) and gets, through these fields:
//   "deck":  { "first_slot": 5, "count": 4, "label": "Staff", "layout": {...}, "layouts": [null, {...}, ...] }   (decklayout.h dl_parse_range; legacy { "slot": "4" } = one deck)
//            the item's own decks "5".."8" in the family's deck list (entries 5-8; entries before first_slot are filled with copies of deck 1 so the
//            vanilla 4 boxes never show an item deck), created at run time if missing and seeded once (layouts[k], null = "layout", else "seed").
//            Equip: the item deck used last (kind state "deck", default the first) is loaded, the family deck in use remembered (family state);
//            unequip: that deck back. Slot boxes: the item decks are unlocked only while the item is equipped, decks 1-4 keep vanilla progression.
//            The Meditation > Combat Deck page gets a row "<label>" under the family row: a copy of the vanilla family row (same widget classes and
//            layout) whose boxes show the item decks (mouse + keyboard like the vanilla row).
//            "deck.layout" (decklayout.h): a deck as a stance-valid layout per in-game row (FrontRight / FrontLeft / BackRight / BackLeft: up to 3 chained rows each, "373m" = placed mirrored)
//            plus "alts" (1 alternate per row), so an attack input works from every start stance; "seed" stays the fallback (one chain in the first row's quadrant) when there is no layout.
//   "anim_style": 7               WHILE THE ITEM IS DRAWN (in hand: on_handled_weapon) the local player's animation lookups use that style index of the
//                                 style-indexed tables (animstyle.inc, anim_style_set); the fighting style itself (stats, ability, save, menus) never changes.
//                                 Sheathed / disarmed / another weapon in hand: the player's own animations.
//   "anim_set": "Staff"           the same, but the index is the runtime anim set of that id (kind "anim_sets", animsets.inc); "anim_style" is then only the
//                                 fallback when the set is unavailable (config anim_sets = off). Any index past 5 is refused unless the tables hold it.
//   "deflect_style": 6            optional, with anim_style: while drawn, a dwmapi.dll proxy mod (e.g. a deflect keyed on a style id) is told this style.
//   "style": 7                    DEPRECATED: read as "anim_style" (logged).
//   A character whose saved fighting style is an item's anim_style gets its previous style back (ws_style_heal).
//   "moves_exclusive": true       the family check answers for this family: item rows only while the item's deck is the current deck, never otherwise;
//                                 every other row never while the item's deck is current (family moves stay out of the item deck and the other way round).
// Save safety: a save never names an item deck (mechanism: the "save safety" section above ws_writelevels_det).
// These fields belong to a weapon KIND (content kind "weapons", weapons.inc); an item gets them with "weapon": "<kind id>" (then its own copies are ignored, logged).
// On an item without "weapon" they are still read (logged as deprecated). Kind state keys are per ITEM id (kind.<item id>.deck).
// Remembered values: per character AND per kind in the character's sidecar (kindstate.inc / sidecar.inc); "legacy.*" keys are read as a fallback.
#include "decklayout.h"
#include "kindrules.h"
#define WS_MAX 16
typedef struct { void *menu, *row, *label, *fb, *fb0, *btn[DL_MAX_DECKS], *amenu, *abtn, *alabel; char label_text[64]; } WsUi;   // per kind: the widgets it added (Combat Deck row, Attacks-page entry)
typedef struct { WsUi ui; CItem *it; char anim_set[64], kind[64]; int bare, sword, excl, anim_style, deflect_style, has_deck, nseed, lock_others; char seed[16][DL_ID_BUF]; DlRange rng; uint64_t fns[DL_MAX_DECKS]; } WsItem;
static int ws_own(const WsItem *w, uint64_t fn) { for (int k = 0; w->has_deck && k < w->rng.count; k++) if (fn && w->fns[k] == fn) return k; return -1; }   // index of fn in the item's deck range, -1 = not an item deck
static WsItem g_ws[WS_MAX]; static int g_ws_n, g_ws_hooked; static void ws_save_restore(const char *why); static int g_wsp_patched, g_wsp_busy;   // busy = a save is being written (WriteLevelsToSave .. the game saw its task finish)
static int ws_any_own(int fam, uint64_t fn) { for (int i = 0; i < g_ws_n; i++) if (g_ws[i].it->family == fam && ws_own(&g_ws[i], fn) >= 0) return 1; return 0; }   // fn is a deck of ANY kind of the family
static int as_set_index(const char *id);                               // animsets.inc
#include "kindstate.inc"
static WsItem *ws_active(void) { KsChar *k = ks_char(NULL, 0); return k && k->equipped >= 0 && k->equipped < g_ws_n ? &g_ws[k->equipped] : NULL; }   // the kind the local player has equipped
static const char *ws_active_kind(void) { WsItem *w = ws_active(); return w && w->kind[0] ? w->kind : NULL; }   // weapons.inc UI identity
static void ws_set_active(void *pawn, WsItem *w) { KsChar *k = ks_char(pawn, 1); if (k) k->equipped = w ? (int)(w - g_ws) : -1; }
static int ws_row_quadrant(const char *row) { uint8_t *r = CA->table_row(CA, CT_ATTACKS, row); return r && r[0xD0] < 4 ? r[0xD0] : -1; }   // m_eStartQuadrant (+0xD0)

// seed a (new) item deck from its layout (the deck is loaded): clear the runtime deck, put the seed rows as one chain into the first row's start quadrant, save the runtime deck back
static void ws_seed_layout(WsItem *w, int k0, void *ac, void *pc) {
    int fam = w->it->family; unsigned char f8 = (unsigned char)fam; const DlLayout *L = &w->rng.lay[k0]; char empty[80];
    void *fcnt = CA->find_function(CA, ac, "BPF_GetAttackCountInCombo"), *frem = CA->find_function(CA, ac, "BPF_RemoveComboAttack"), *fset = CA->find_function(CA, ac, "BPF_SetComboAttack"), *fsave = CA->find_function(CA, pc, "BPF_SaveCombo");
    if (!fcnt || !frem || !fset || !fsave) { logf_("weaponslot: layout seed: attack component functions missing"); return; }
    int removed = 0, added = 0, alts = 0, expect = L->total; dl_empty_rows(L, empty, sizeof empty);
    for (int q = 0; q < 4; q++) { unsigned char q8 = (unsigned char)q; int cnt = 0; void *a[2] = { &f8, &q8 }; CA->call_function_params(CA, ac, fcnt, a, &cnt); for (int i = cnt - 1; i >= 0; i--) { void *r[3] = { &f8, &q8, &i }; if (CA->call_function_params(CA, ac, frem, r, NULL)) removed++; } }
    for (int r = 0; r < DL_ROWS; r++) {
        unsigned char q8 = (unsigned char)dl_api_quadrant(r);                                         // stance row -> its EQuadrantTypes (decklayout.h)
        for (int k = 0; k < L->n[r]; k++) {
            uint64_t fn; CA->fname_make(CA, L->mv[r][k], &fn); unsigned char mir = L->mir[r][k]; int idx = k; void *a[5] = { &fn, &f8, &q8, &idx, &mir };
            if (CA->call_function_params(CA, ac, fset, a, NULL)) added++; else logf_("weaponslot: layout: SetComboAttack %s row %s failed", L->mv[r][k], DL_ROW_NAME[r]);
            if (!k) { int sq = ws_row_quadrant(L->mv[r][0]); static const int ig[DL_ROWS] = { 1, 0, 2, 3 }, mi[4] = { 1, 0, 3, 2 }; int want = L->mir[r][0] ? mi[ig[r]] : ig[r]; logf_("weaponslot: layout: row %s first move %s starts in quadrant %d (wanted%s: %d)%s", DL_ROW_NAME[r], L->mv[r][0], sq, L->mir[r][0] ? " mirrored" : "", want, sq == want ? "" : "  <-- MISMATCH"); }
        }
        uint64_t fn = 0; if (L->alt[r][0]) CA->fname_make(CA, L->alt[r], &fn); unsigned char mir = L->altmir[r]; int idx = -1; void *a[5] = { &fn, &f8, &q8, &idx, &mir };   // index -1 = the row's alternate (name None clears it)
        if (CA->call_function_params(CA, ac, fset, a, NULL) && L->alt[r][0]) alts++;
    }
    int saved = CA->call_function_params(CA, pc, fsave, NULL, NULL), got = 0;
    for (int r = 0; r < DL_ROWS; r++) { unsigned char q8 = (unsigned char)dl_api_quadrant(r); int cnt = 0; void *a[2] = { &f8, &q8 }; CA->call_function_params(CA, ac, fcnt, a, &cnt); got += cnt; if (cnt != L->n[r]) logf_("weaponslot: layout: row %s holds %d move(s), layout says %d", DL_ROW_NAME[r], cnt, L->n[r]); }
    logf_("weaponslot: seeded deck '%s' from %s: removed %d, set %d of %d move(s) + %d alternate(s), live deck now holds %d move(s) (empty rows: %s), BPF_SaveCombo %d", w->rng.names[k0], w->rng.has[k0] == 1 ? "its own layout" : "\"layout\"", removed, added, expect, alts, got, empty[0] ? empty : "none", saved);
}
static void ws_seed(WsItem *w, int k0) {                                                     // the runtime deck must be deck k0 of the range (loaded)
    void *pawn = cd_local_pawn(), *ac = cd_comp(pawn, "m_AttackComponent"), *pc = L_local_pc(); int fam = w->it->family;
    if (w->rng.has[k0] && ac && pc) { ws_seed_layout(w, k0, ac, pc); return; }
    if (!ac || !pc || !w->nseed) return; int q0 = ws_row_quadrant(w->seed[0]); if (q0 < 0) { logf_("weaponslot: seed row %s unknown - deck left as copied", w->seed[0]); return; }
    void *fcnt = CA->find_function(CA, ac, "BPF_GetAttackCountInCombo"), *frem = CA->find_function(CA, ac, "BPF_RemoveComboAttack"), *fset = CA->find_function(CA, ac, "BPF_SetComboAttack"), *fsave = CA->find_function(CA, pc, "BPF_SaveCombo");
    if (!fcnt || !frem || !fset || !fsave) { logf_("weaponslot: seed: attack component functions missing"); return; }
    unsigned char f8 = (unsigned char)fam; int removed = 0, added = 0;
    for (int q = 0; q < 4; q++) { unsigned char q8 = (unsigned char)q; int cnt = 0; void *a[2] = { &f8, &q8 }; CA->call_function_params(CA, ac, fcnt, a, &cnt); for (int i = cnt - 1; i >= 0; i--) { void *r[3] = { &f8, &q8, &i }; if (CA->call_function_params(CA, ac, frem, r, NULL)) removed++; } }
    for (int k = 0; k < w->nseed; k++) { uint64_t fn; CA->fname_make(CA, w->seed[k], &fn); unsigned char q8 = (unsigned char)q0, mir = 0; int idx = k; void *a[5] = { &fn, &f8, &q8, &idx, &mir }; if (CA->call_function_params(CA, ac, fset, a, NULL)) added++; }
    int cnt = 0; { unsigned char q8 = (unsigned char)q0; void *a[2] = { &f8, &q8 }; CA->call_function_params(CA, ac, fcnt, a, &cnt); }
    int saved = CA->call_function_params(CA, pc, fsave, NULL, NULL);
    logf_("weaponslot: seeded deck '%s': removed %d attack(s), set %d of %d seed row(s) in quadrant %d (count now %d), BPF_SaveCombo %d", w->rng.names[k0], removed, added, w->nseed, q0, cnt, saved);
}

// ----- the animations of an item's rows ("moves" + deck seed): the game loads only the animations of the rows in its loaded decks, and an attack whose animation is not in memory
// starts and stops at once. On equip (before the item's deck is loaded) every row's m_Anim is loaded and rooted, once per item.
static CItem *g_wsa_done[64]; static int g_wsa_n;
static void ws_anims_load(CItem *it) {
    for (int i = 0; i < g_wsa_n; i++) if (g_wsa_done[i] == it) return;
    if (!CA->find_object(CA, CT_ATTACKS)) { logf_("content: item %s: attacks table not loaded - row animations not preloaded yet", it->id); return; }
    static char rows[CI_MOVES_MAX + 64][DL_ID_BUF]; const int RN = CI_MOVES_MAX + 64; int n = 0; for (int k = 0; k < it->nmoves; k++) n = tk_rowlist_add(rows, n, RN, it->moves[k]);
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].it == it) for (int k = 0; k < g_ws[i].nseed; k++) n = tk_rowlist_add(rows, n, RN, g_ws[i].seed[k]);
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].it == it) for (int d = 0; d < g_ws[i].rng.count; d++) if (g_ws[i].rng.has[d]) { const DlLayout *L = &g_ws[i].rng.lay[d]; for (int r = 0; r < DL_ROWS; r++) { for (int k = 0; k < L->n[r]; k++) n = tk_rowlist_add(rows, n, RN, L->mv[r][k]); if (L->alt[r][0]) n = tk_rowlist_add(rows, n, RN, L->alt[r]); } }
    int loaded = 0, had = 0, bad = 0; ULONGLONG t0 = GetTickCount64();
    for (int k = 0; k < n; k++) {
        char ap[300]; int r = se_row_anim_ensure(CA, CT_ATTACKS, rows[k], 1, ap, sizeof ap);
        if (r == 1) { loaded++; logf_("content: item %s: row %s animation loaded + rooted: %s", it->id, rows[k], ap); } else if (r == 0) had++; else { bad++; logf_("content: item %s: row %s animation '%s' could NOT be loaded - that move will not play", it->id, rows[k], ap); }
    }
    if (g_wsa_n < 64) g_wsa_done[g_wsa_n++] = it;
    logf_("content: item %s: %d row animation(s): %d loaded now, %d already in memory, %d failed (%llu ms)", it->id, n, loaded, had, bad, GetTickCount64() - t0);
}
static void ws_anims_weapon_changed(void *pawn, void *old, void *nw, int handled, void *u) {
    (void)pawn; (void)old; (void)handled; (void)u; if (!nw) return;
    for (int i = 0; i < g_items_n; i++) if (g_items[i].obj == nw && g_items[i].nmoves) ws_anims_load(&g_items[i]);
}

// the item's decks: entries before first_slot are filled (copies of deck 1, named by their number) so the item decks sit at list entries first_slot.. and the vanilla
// boxes (entries 1-4) never show one; missing item decks are appended (copies of deck 1) and seeded once. Returns the number created.
static int ws_ensure_decks(WsItem *w) {
    int fam = w->it->family, made = 0; SeTArr *a = cd_deck_list(fam); if (!a) return 0;
    for (int guard = 0; !w->rng.legacy && a->num < w->rng.first - 1 && guard < 16; guard++) {
        char nm[16]; uint64_t fn; snprintf(nm, sizeof nm, "%d", a->num + 1); CA->fname_make(CA, nm, &fn);
        if (cd_deck_index(fam, fn) >= 0 || ws_own(w, fn) >= 0) snprintf(nm, sizeof nm, "%d_", a->num + 1);                 // name taken by a renamed / reordered deck
        int idx = CA->combo_slot_ensure(CA, fam, nm, 0); logf_("weaponslot: filler deck '%s' (copy of deck 1) at list entry %d", nm, idx + 1); if (idx < 0) break; a = cd_deck_list(fam); if (!a) return made;
    }
    char cur[64] = ""; CA->combo_current(CA, fam, cur, sizeof cur);
    for (int k = 0; k < w->rng.count; k++) {
        int before = cd_deck_index(fam, w->fns[k]); if (before >= 0) continue;
        int idx = CA->combo_slot_ensure(CA, fam, w->rng.names[k], 0); if (idx < 0) continue; made++;
        if (CA->combo_load(CA, fam, w->rng.names[k])) ws_seed(w, k);
        if (!w->rng.legacy && idx != w->rng.first - 1 + k) logf_("weaponslot: deck '%s' is list entry %d, not %d (the deck list had other entries) - the boxes follow the names", w->rng.names[k], idx + 1, w->rng.first + k);
    }
    if (made && cur[0]) CA->combo_load(CA, fam, cur);
    logf_("weaponslot: %s: decks %s..%s ready (%d created)", w->it->id, w->rng.names[0], w->rng.names[w->rng.count - 1], made);
    return made;
}
// The Practice / deck editor family switch (BP_ComboEditorMenu.ChangeWeapon) only switches when UAttackComponent::BPF_ValidateCombo(current family) == Success;
// otherwise it opens the "invalid deck" window. Item decks are seeded ONCE, and later row data changes (e.g. other end quadrants) can leave a stored deck
// stance-invalid. So on equip every item deck is validated; an invalid one is re-seeded from its layout (the game would refuse to leave it anyway).
static ULONGLONG g_ws_cur_at;                                                                   // ws_famchk's current-deck cache (below): reset after a deck load
static int ws_validate(int fam) {
    void *ac = cd_comp(cd_local_pawn(), "m_AttackComponent"), *f = ac ? CA->find_function(CA, ac, "BPF_ValidateCombo") : NULL; unsigned char f8 = (unsigned char)fam, r = 0xFF; void *a[1] = { &f8 };
    g_ws_cur_at = 0; if (f) CA->call_function_params(CA, ac, f, a, &r); return r;       // EComboAttackValidationStates: 0 Success, 1 WrongEndQuadrant, 2 PreviousAttackIsEmpty, 3 PreviousAttackHasWrongEndQuadrant, 4 CannotBeEmpty, 5 AttackLocked
}
static void ws_validate_decks(WsItem *w) {
    int fam = w->it->family, bad = 0;
    for (int k = 0; k < w->rng.count; k++) {
        if (cd_deck_index(fam, w->fns[k]) < 0 || !CA->combo_load(CA, fam, w->rng.names[k])) continue;
        int v = ws_validate(fam); if (!v) continue; bad++;
        if (!w->rng.has[k] && !w->nseed) { logf_("weaponslot: deck '%s' is INVALID (validation state %d) and has no layout to re-seed from - left as is (Practice / the deck editor will refuse to leave it)", w->rng.names[k], v); continue; }
        ws_seed(w, k); logf_("weaponslot: deck '%s' was INVALID (validation state %d: stale rows / stances) -> re-seeded from its %s, now %d", w->rng.names[k], v, w->rng.has[k] ? "layout" : "seed rows", ws_validate(fam));
    }
    logf_("weaponslot: %s: %d of %d item deck(s) were invalid", w->it->id, bad, w->rng.count);
}
// the item deck to load: the one used last (kind state, else legacy.staff_deck) if it still exists, else the first
static int ws_pick(WsItem *w) { char d[64]; uint64_t fn; ks_kind_get(w->it->id, "deck", "legacy.staff_deck", d, sizeof d); if (d[0]) { CA->fname_make(CA, d, &fn); int k = ws_own(w, fn); if (k >= 0 && cd_deck_index(w->it->family, fn) >= 0) return k; } return 0; }
// the family deck for the family's other weapons: the remembered one if it exists and is not an item deck, else entry 1
static void ws_family_deck(WsItem *w, char *d, int cap) {
    uint64_t fn = 0; ks_family_get(w->it->family, d, cap); if (d[0]) CA->fname_make(CA, d, &fn);
    if (!d[0] || cd_deck_index(w->it->family, fn) < 0 || ws_any_own(w->it->family, fn)) { SeTArr *a = cd_deck_list(w->it->family); d[0] = 0; if (a && a->num) fname_text(*(FNameRaw *)a->data, d, cap); }
}
static uint64_t g_ws_seen;                                                                      // last deck name ws_deck_watch acted on
static void ws_enter(WsItem *w, void *pawn) {
    int fam = w->it->family; char cur[64] = "";
    if (w->has_deck) {
        uint64_t *c = cd_current_slot(fam); CA->combo_current(CA, fam, cur, sizeof cur); if (cur[0] && c && ws_own(w, *c) < 0) ks_family_set(fam, cur);
        ws_ensure_decks(w); ws_validate_decks(w); int k = ws_pick(w);
        logf_("weaponslot: %s equipped: current deck '%s', own deck '%s' (index %d) -> %d", w->it->id, cur, w->rng.names[k], cd_deck_index(fam, w->fns[k]), CA->combo_load(CA, fam, w->rng.names[k]));
        g_ws_seen = w->fns[k];
    }
    ws_set_active(pawn, w);
}
static void ws_leave(WsItem *w, void *pawn) {
    int fam = w->it->family; char d[64], cur[64] = ""; ws_set_active(pawn, NULL);
    if (w->has_deck) {
        uint64_t *c = cd_current_slot(fam); CA->combo_current(CA, fam, cur, sizeof cur);
        if (c && ws_own(w, *c) >= 0) { ks_kind_set(w->it->id, "deck", cur); ws_family_deck(w, d, sizeof d); logf_("weaponslot: %s unequipped: deck '%s' remembered, deck back to '%s' -> %d", w->it->id, cur, d, d[0] ? CA->combo_load(CA, fam, d) : 0); }
    }
}
// a weapon change while a save is being serialised waits until the game saw the save task finish (ws_save_restore replays it): a deck switch now would put
// an item deck name into the save object while the worker thread may still be reading it
static struct { int on; void *pawn, *old, *nw; } g_ws_defer;
static void ws_weapon_changed(void *pawn, void *old, void *nw, int handled, void *u) {
    (void)handled; (void)u;
    if (g_wsp_busy) { if (!g_ws_defer.on) { g_ws_defer.on = 1; g_ws_defer.old = old; } g_ws_defer.pawn = pawn; g_ws_defer.nw = nw; logf_("weaponslot: weapon change %p -> %p during a save: deferred until the save is written", old, nw); return; }
    if (!old) { KsChar *k = ks_char(pawn, 0); if (k && k->equipped >= 0) { logf_("weaponslot: a character appeared: kind state of the previous one cleared"); k->equipped = -1; } }   // the first event of a character
    for (int i = 0; i < g_ws_n; i++) if (old && g_ws[i].it->obj == old && g_ws[i].it->obj != nw) ws_leave(&g_ws[i], pawn);
    for (int i = 0; i < g_ws_n; i++) if (nw && g_ws[i].it->obj == nw && (old != nw)) ws_enter(&g_ws[i], pawn);
}
static void ws_defer_replay(void *u) {
    (void)u; if (!g_ws_defer.on || g_wsp_busy) return; g_ws_defer.on = 0;
    if (g_ws_defer.pawn != cd_local_pawn()) { logf_("weaponslot: deferred weapon change dropped (the character changed)"); return; }
    logf_("weaponslot: deferred weapon change %p -> %p replayed after the save", g_ws_defer.old, g_ws_defer.nw); ws_weapon_changed(g_ws_defer.pawn, g_ws_defer.old, g_ws_defer.nw, 0, NULL);
}
// ----- "anim_style": the item's animation set while it is IN HAND (drawn); the fighting style never changes. Drawn state per character (kindstate).
static void ws_handled(void *pawn, void *weapon, int fam, void *u) {
    (void)u; (void)fam; int on = -1; KsChar *k = ks_char(pawn, 1); if (!k) return;
    for (int i = 0; i < g_ws_n; i++) if ((g_ws[i].anim_style >= 0 || g_ws[i].anim_set[0]) && weapon && g_ws[i].it->obj == weapon) on = i;
    if (on == k->drawn) return;
    if (k->drawn >= 0) { WsItem *w = &g_ws[k->drawn]; logf_("weaponslot: %s no longer in hand: the player's own animations (style %d) -> %d", w->it->id, cd_style_of(pawn), as_set_ex(CA, NULL, -1, -1)); }
    k->drawn = on;
    if (on >= 0) { WsItem *w = &g_ws[on]; int rs = w->anim_set[0] ? as_set_index(w->anim_set) : -1, st = rs >= 0 ? rs : w->anim_style;
        if (w->anim_set[0]) logf_("weaponslot: %s: anim set '%s' -> index %d%s", w->it->id, w->anim_set, st, rs < 0 ? " (set unavailable: the numeric anim_style fallback)" : "");
        logf_("weaponslot: %s drawn: animation style %d (proxy style %d), fighting style stays %d -> %d", w->it->id, st, w->deflect_style, cd_style_of(pawn), st >= 0 ? as_set_ex(CA, NULL, st, w->deflect_style) : 0); }
}
// self-heal: a save may hold an item's anim_style (e.g. 7) as the fighting style. That is a hidden table index, not a playable style:
// put back the style stored in the sidecar as legacy.style, else Windfall (1).
static void ws_style_heal(void *pawn, int old, int nw, void *u) {
    (void)u; (void)old; char s[16]; int bad = 0;
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].anim_style >= 0 && nw == g_ws[i].anim_style) bad = 1;
    if (!bad || !pawn) return;
    sc_read_key("legacy.style", s, sizeof s); int st = s[0] ? atoi(s) : 1; if (st < 0 || st > 63) st = 1;
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].anim_style == st) st = 1;
    logf_("weaponslot: fighting style %d is a hidden animation index (an older save): style back to %d -> %d", nw, st, CA->set_combat_style(CA, NULL, st));
}

// ----- a deck picked in the menu while the item is equipped (Enter / click on a box): an item deck is remembered as the one to load next time; a family deck
// (boxes 1-4) becomes the remembered deck for the family's other weapons and the item deck is reloaded. While nothing is equipped from us, an item deck
// that became current (it cannot be picked: locked) is replaced by the family deck.
static ULONGLONG g_wsp_since;
static void ws_deck_watch(void *u) {
    (void)u; wp_ui_sync();                                          // weapons.inc: menu identity of the equipped kind
    if (g_wsp_busy) { if (GetTickCount64() - g_wsp_since > 10000) ws_save_restore("SAFETY: no save-completed signal within 10 s"); return; }
    WsItem *w = ws_active();
    if (!w) { for (int i = 0; i < g_ws_n; i++) if (g_ws[i].has_deck) { uint64_t *c = cd_current_slot(g_ws[i].it->family); char d[64]; if (c && *c && ws_own(&g_ws[i], *c) >= 0 && cd_local_pawn()) { ws_family_deck(&g_ws[i], d, sizeof d); logf_("weaponslot: item deck current without %s equipped: deck back to '%s' -> %d", g_ws[i].it->id, d, d[0] ? CA->combo_load(CA, g_ws[i].it->family, d) : 0); } } return; }
    if (!w->has_deck) return; uint64_t *c = cd_current_slot(w->it->family); if (!c || !*c || *c == g_ws_seen) return;
    char n[64]; fname_text(*(FNameRaw *)c, n, sizeof n); int k = ws_own(w, *c);
    if (k >= 0) { g_ws_seen = *c; ks_kind_set(w->it->id, "deck", n); logf_("weaponslot: deck '%s' picked while %s is equipped", n, w->it->id); return; }
    ks_family_set(w->it->family, n); k = ws_pick(w); g_ws_seen = w->fns[k];
    logf_("weaponslot: deck '%s' picked while %s is equipped: kept for the other weapons, own deck '%s' reloaded -> %d", n, w->it->id, w->rng.names[k], CA->combo_load(CA, w->it->family, w->rng.names[k]));
}
// ----- deck slot boxes: BPF_IsComboSlotUnlocked(family, &name, mode)
static unsigned char (*g_o_slotunl)(void *, unsigned char, uint64_t *, unsigned char);
static unsigned char ws_slotunl_det(void *th, unsigned char fam, uint64_t *name, unsigned char mode) {
    WsItem *act = ws_active();
    for (int i = 0; i < g_ws_n; i++) {
        WsItem *w = &g_ws[i]; if (!w->has_deck || fam != w->it->family || !name) continue;
        if (ws_own(w, *name) >= 0) return act == w;                                         // own decks: open only while the item is equipped
        if (act == w && w->lock_others) return 0;                                           // the family's other decks: closed while it is equipped ("lock_family_decks": true; default false, because a fully locked row blocks the menu's keyboard navigation to the item's row)
    }
    return g_o_slotunl(th, fam, name, mode);
}
// ----- exclusive family check (called from items_famchk_det): 1 = answered (*r set), 0 = not ours
static uint64_t g_ws_cur[3];
static int ws_famchk(unsigned char fam, void *row, unsigned char *r) {
    unsigned char mine[WS_MAX], cur[WS_MAX], bare[WS_MAX]; int n = 0, any_mine = 0;               // one entry per eligible kind (kindrules.h decides)
    for (int i = 0; i < g_ws_n; i++) {
        WsItem *w = &g_ws[i]; CItem *it = w->it; if (!w->excl || !w->has_deck || fam != it->family || !it->obj) continue;
        ULONGLONG now = GetTickCount64(); if (now - g_ws_cur_at > 150) { g_ws_cur_at = now; for (int f = 0; f < 3; f++) { uint64_t *c = cd_current_slot(f); g_ws_cur[f] = c ? *c : 0; } }
        mine[n] = 0; items_resolve_moves(it); for (int k = 0; k < it->nmoves; k++) if (it->move_rows[k] == row) mine[n] = 1;
        any_mine |= mine[n]; bare[n] = (unsigned char)(w->sword ? 2 : w->bare ? 1 : 0); cur[n] = ws_own(w, g_ws_cur[fam]) >= 0; n++;   // allow level: kindrules.h
    }
    return kr_famchk(n, mine, cur, bare, any_mine ? 0 : kr_row_level((const unsigned char *)row), r);
}

// ----- save safety. AFightingPlayerController::SaveGame: SaveParameters -> UThePlainesGameInstance::WriteLevelsToSave ->
// SaveCharacterProfile -> SaveProfile -> UThePlainesGameInstance::LaunchSave. SaveCharacterProfile does NOT serialise: it queues an FSavedata whose weak pointer is the
// LIVE save object plus a serialise lambda (0x3A21B0: entry+0x28 = rdx, the save). LaunchSave -> SaveHelpers::LaunchSaveTask -> FAsyncSaveTask::DoWork runs
// that lambda on a worker thread (at once when LaunchSave's bool is set), then hashes / encrypts / writes. The game thread learns the end in
// UThePlainesGameInstance::TickSaveLoad (0x3E6700): m_SaveTask (+0x6BB0, stored by LaunchSave at 0x3D6335) with WorkNotFinishedCounter (+0x28) 0 -> OnSaveCompleted,
// task deleted, m_SaveTask = NULL. DoWork, FAsyncTask::DoThreadedWork and SaveCharacterProfile cannot be hooked (RIP-relative operand / call in the first 14 bytes);
// TickSaveLoad can (18-byte prologue). So: the name is patched after WriteLevelsToSave, LaunchSave's task is remembered, and the live name comes back in TickSaveLoad
// once that task is gone. Our own deck switches wait for it (ws_weapon_changed defers, ws_deck_watch pauses); SaveCombo aimed at the patched name is redirected.
// Fallbacks: no TickSaveLoad hook -> a 1.5 s timer; no completion seen within 10 s -> restored by ws_deck_watch (logged as SAFETY).
#define WS_OFF_SAVETASK 0x6BB0
static uint64_t *g_wsp_slot, g_wsp_keep, g_wsp_fn; static void *g_wsp_task;   // g_wsp_fn = the family deck name written into the save object
static void *ws_savetask(void *gi) { void *t = NULL; __try { t = gi ? *(void **)((uint8_t *)gi + WS_OFF_SAVETASK) : NULL; } __except (EXCEPTION_EXECUTE_HANDLER) { t = NULL; } return t; }
static void ws_save_restore(const char *why) {
    if (!g_wsp_busy && !g_wsp_patched) return; int same = 2;
    if (g_wsp_patched) __try { same = *g_wsp_slot == g_wsp_fn; if (same) *g_wsp_slot = g_wsp_keep; } __except (EXCEPTION_EXECUTE_HANDLER) { same = -1; }
    g_wsp_patched = 0; g_wsp_busy = 0; g_wsp_task = NULL;
    logf_("weaponslot: %s: live deck %s, %llu ms after WriteLevelsToSave", why, same == 2 ? "was not patched" : same == 1 ? "restored" : same < 0 ? "NOT restored (EXCEPTION)" : "left as the game set it (changed during the save)", GetTickCount64() - g_wsp_since);
    if (g_ws_defer.on) CA->set_timer(CA, 0.01f, 0, ws_defer_replay, NULL);
}
static void ws_save_restore_job(void *u) { (void)u; ws_save_restore("1.5 s timer (no TickSaveLoad hook)"); }
static void (*g_o_writelevels)(void *); static void (*g_o_launchsave)(void *, void *, unsigned char); static void (*g_o_ticksave)(void *);
static void ws_launchsave_det(void *gi, void *data, unsigned char sync) {
    g_o_launchsave(gi, data, sync);
    if (!g_wsp_busy) return;
    g_wsp_task = ws_savetask(gi);
    if (!g_o_ticksave) { CA->set_timer(CA, 1.5f, 0, ws_save_restore_job, NULL); logf_("weaponslot: save task %p launched: no TickSaveLoad hook, save treated as written in 1.5 s", g_wsp_task); }
    else logf_("weaponslot: save task %p launched (%s, deck name patched %d): our deck switches wait until the game sees it finish", g_wsp_task, sync ? "synchronous, already serialised" : "worker thread", g_wsp_patched);
}
static void ws_ticksave_det(void *gi) {
    void *before = g_wsp_busy ? ws_savetask(gi) : NULL;
    g_o_ticksave(gi);
    if (g_wsp_busy && g_wsp_task && before == g_wsp_task && ws_savetask(gi) != g_wsp_task) ws_save_restore("save written (TickSaveLoad: save task finished)");
}
// AFightingPlayerController::SaveCombo(name, family) copies the runtime deck into the deck NAMED name (callers pass the current name): a call aimed at the patched
// family deck name while the save is written goes to the item deck that is really current, so no family deck is overwritten
static void (*g_o_savecombo)(void *, uint64_t, unsigned char);
static void ws_savecombo_det(void *pc, uint64_t name, unsigned char fam) {
    if (g_wsp_patched && name == g_wsp_fn) { name = g_wsp_keep; logf_("weaponslot: SaveCombo during a save: redirected to the current deck"); }
    g_o_savecombo(pc, name, fam);
}
static void ws_writelevels_det(void *gi) {
    g_o_writelevels(gi);
    WsItem *w = ws_active(); void *save = cd_live_save(); uint64_t *slot = NULL; uint8_t *st = NULL; char why[100], d[64] = "";
    if (g_ws_n && save) { if (!g_wsp_busy) g_wsp_since = GetTickCount64(); g_wsp_busy = 1; g_wsp_task = NULL; }   // any save: our deck switches wait for it (an item may become equipped a tick later)
    __try {
        if (w && save && w->has_deck) {
            char p[80]; snprintf(p, sizeof p, "m_SavedData.m_CurrentComboSaveName[%d]", w->it->family); slot = cd_path(save, p, why, sizeof why); st = cd_path(save, "m_SavedData.m_uiCombatStyle", why, sizeof why);   // the style is only logged, never changed
            if (slot) {
                uint64_t live = g_wsp_patched && slot == g_wsp_slot && *slot == g_wsp_fn ? g_wsp_keep : *slot;   // a save while the last one is still being written keeps its patch
                if (ws_own(w, live) >= 0) { ws_family_deck(w, d, sizeof d); if (d[0]) { uint64_t fn = 0; CA->fname_make(CA, d, &fn); g_wsp_slot = slot; g_wsp_keep = live; *slot = fn; g_wsp_fn = fn; g_wsp_patched = 1; } }
                else if (g_wsp_patched) { if (*slot == g_wsp_fn) *slot = g_wsp_keep; g_wsp_patched = 0; logf_("weaponslot: the current deck is a family deck again: patch dropped"); }
            }
            logf_("weaponslot: save being written while %s is equipped: deck name in the file '%s' (patched %d), fighting style in the file %d (unchanged)", w->it->id, d[0] ? d : "(the current one)", g_wsp_patched, st ? *st : -1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("weaponslot: save patch EXCEPTION 0x%08lX", GetExceptionCode()); }
}
// ----- Meditation > Combat Deck: a row "<label>" under the family's row, a copy of the vanilla row. Vanilla widget tree:
//   <Family>_Slots (HorizontalBox) > Overlay { Border > VerticalBox { TextBlock "<Family>" ; TableWidget_<F> > HorizontalBox > 4 x BP_CombatDeckSlotButton_C } ;
//                                              SizeBox > ScaleBox > BP_CombatDeckFamilyButton_C (the badge: icon + current deck number) }
// Every widget is the same class as its vanilla counterpart with all its (non transient) properties copied; the row label is the TextBlock (NOT the family
// button's TextBlock_0, which is the badge's deck number); the boxes are added through TableWidget::BPF_AddItem, so the table binds their click / focus
// delegates (mouse and keyboard), and the table's own delegates are the vanilla table's (the menu's handlers for that family's boxes).
static void *ws_parent(void *wd) { void **s = wd ? CA->prop_ptr(CA, wd, "Slot") : NULL; void **p = s && *s ? CA->prop_ptr(CA, *s, "Parent") : NULL; return p ? *p : NULL; }
static int ws_call1(void *obj, const char *fn, void *arg, void *ret) { void *f = obj ? CA->find_function(CA, obj, fn) : NULL; void *args[1] = { arg }; return f && CA->call_function_params(CA, obj, f, arg ? args : NULL, ret); }
static void *ws_child(void *panel, int i) { void *c = NULL; if (panel) ws_call1(panel, "GetChildAt", &i, &c); return c; }
static int ws_owned_by(void *o, void *owner) { for (int d = 0; o && d < 8; d++) { if (o == owner) return 1; o = *(void **)((uint8_t *)o + 0x20); } return 0; }
// every property of src (same class as dst) as engine text into dst: no transient ones, no slot / tree links, no objects owned by src (a user widget's inner
// widgets), delegates only when asked (a button's delegates point at the vanilla table). Returns the number copied; failures are logged.
static int ws_clone_props(void *src, void *dst, int delegates) {
    static const char *const skip[] = { "Slot", "Slots", "Parent", "Content", "Navigation", "bIsVariable", "bCreatedByConstructionScript", "WidgetTree", "UberGraphFrame", "m_bIsSelected", "m_bIsMouseOver", "m_bIsMouseDown", NULL };
    int n = 0, bad = 0; if (!src || !dst || *(void **)((uint8_t *)src + 0x10) != *(void **)((uint8_t *)dst + 0x10)) return 0;
    __try {
        for (uint8_t *s = *(uint8_t **)((uint8_t *)src + 0x10); s; s = *(uint8_t **)(s + 0x30))
            for (uint8_t *f = *(uint8_t **)(s + 0x38); f; f = *(uint8_t **)(f + 0x28)) {
                if (!field_is_property(f) || (*(uint64_t *)(f + 0x38) & 0x2000)) continue;                 // CPF_Transient
                char pn[100]; fname_text(*(FNameRaw *)(f + 0x18), pn, sizeof pn); int sk = 0; for (int i = 0; skip[i]; i++) if (!strcmp(pn, skip[i])) sk = 1; if (sk) continue;
                const char *k = prop_kind(f); if ((!delegates && strstr(k, "Delegate")) || !strcmp(k, "Delegate")) continue;   // single delegates are all unbound on these widgets (and do not import)
                int dim = *(int *)(f + 0x30), esz = *(int *)(f + 0x34), off = *(int *)(f + 0x44);
                for (int i = 0; i < dim; i++) {
                    uint8_t *vs = (uint8_t *)src + off + (size_t)i * esz, *vd = (uint8_t *)dst + off + (size_t)i * esz;
                    if (!strcmp(k, "Object") && ws_owned_by(*(void **)vs, src)) continue;
                    wchar_t *t = prop_export_w(f, vs, src); if (!t) continue;
                    if (prop_import_w(f, vd, dst, t)) n++; else if (bad++ < 4) logf_("weaponslot: row copy: property %s not imported", pn);
                    free(t);
                }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("weaponslot: row copy EXCEPTION 0x%08lX", GetExceptionCode()); }
    return n;
}
static void *ws_new_like(void *outer, void *like, int delegates) { void *o = like ? CA->construct_object(CA, CA->object_class(CA, like), outer) : NULL; if (o) ws_clone_props(like, o, delegates); return o; }
static void *ws_add(void *panel, void *child, void *like) {                                     // AddChild + the slot as `like`'s slot (memory: the panel is not on screen yet)
    void *slot = NULL; ws_call1(panel, "AddChild", &child, &slot); void **ls = like ? CA->prop_ptr(CA, like, "Slot") : NULL; if (slot && ls && *ls) ws_clone_props(*ls, slot, 0); return slot;
}
static void ws_label_refresh(void *u) {
    (void)u;
    for (int i = 0; i < g_ws_n; i++) {
        WsUi *ui = &g_ws[i].ui; if (!ui->label || !ui->label_text[0]) continue;
        unsigned char t[24]; char now[64] = ""; void *cur = CA->prop_ptr(CA, ui->label, "Text"); if (cur) CA->text_to_string(CA, cur, now, sizeof now);
        if (strcmp(now, ui->label_text) && !CA->make_text(CA, ui->label_text, t)) ws_call1(ui->label, "SetText", t, NULL);
    }
}
// the boxes show list entries by index: bind them to the item decks' current entries (decks are created on the first equip, maybe after the row was built)
static void ws_rebind(WsItem *w) {
    for (int k = 0; k < w->rng.count; k++) {
        void *b = w->ui.btn[k]; if (!b) continue; int idx = cd_deck_index(w->it->family, w->fns[k]); if (idx < 0) idx = (w->rng.legacy ? 3 : w->rng.first - 1) + k;
        int *si = CA->prop_ptr(CA, b, "m_iSlotIndex"); if (si && *si != idx) { logf_("weaponslot: box %d -> deck list entry %d ('%s')", k + 1, idx + 1, w->rng.names[k]); ws_call1(b, "BPF_SetSlotIndex", &idx, NULL); }
    }
}
static void ws_deck_row(void *menu, WsItem *w) {                                                  // one row per kind with decks
    if (menu == w->ui.menu && w->ui.row) { ws_rebind(w); ws_label_refresh(NULL); return; }
    static const char *const fams[3] = { "BareHandsCombatDeckFamilyButton", "SwordCombatDeckFamilyButton_C_1", "WarGlovesCombatDeckFamilyButton_C_0" }, *const tabs[3] = { "TableWidget_BH", "TableWidget_Sword", "TableWidget_WG" };
    int fam = w->it->family; void **pf = CA->prop_ptr(CA, menu, fams[fam]), **pt = CA->prop_ptr(CA, menu, tabs[fam]);
    void *fb0 = pf ? *pf : NULL, *tb0 = pt ? *pt : NULL, *vb0 = ws_parent(tb0), *bd0 = ws_parent(vb0), *ov0 = ws_parent(bd0), *wr0 = ws_parent(ov0), *rows = ws_parent(wr0);
    void *sc0 = ws_parent(fb0), *sz0 = ws_parent(sc0), *tx0 = ws_child(vb0, 0), *hb0 = ws_child(tb0, 0), *bt0 = ws_child(hb0, 0), *pc = CA->get_local_player_controller(CA);
    int ok = fb0 && tb0 && tx0 && hb0 && bt0 && rows && pc && CA->is_a(CA, vb0, "VerticalBox") && CA->is_a(CA, bd0, "Border") && CA->is_a(CA, ov0, "Overlay") && CA->is_a(CA, wr0, "HorizontalBox") && CA->is_a(CA, rows, "VerticalBox")
          && CA->is_a(CA, sc0, "ScaleBox") && CA->is_a(CA, sz0, "SizeBox") && CA->is_a(CA, tx0, "TextBlock") && CA->is_a(CA, bt0, "CombatDeckButton") && ws_parent(sz0) == ov0;
    logf_("weaponslot: Combat Deck page %p: vanilla row %p (overlay %p, border %p, box %p, label %p, table %p, row %p, box1 %p, badge %p in %p/%p), rows %p -> %s", menu, wr0, ov0, bd0, vb0, tx0, tb0, hb0, bt0, fb0, sc0, sz0, rows, ok ? "copying" : "LAYOUT CHANGED - no row");
    if (!ok) return;
    void *tree = *(void **)((uint8_t *)rows + 0x20), *bcls = CA->object_class(CA, bt0), *fcls = CA->object_class(CA, fb0);   // the widgets' Outer: the menu's WidgetTree
    void *wr = ws_new_like(tree, wr0, 1), *ov = ws_new_like(tree, ov0, 1), *bd = ws_new_like(tree, bd0, 1), *vb = ws_new_like(tree, vb0, 1), *tx = ws_new_like(tree, tx0, 1), *tb = ws_new_like(tree, tb0, 1);
    void *sz = ws_new_like(tree, sz0, 1), *sc = ws_new_like(tree, sc0, 1), *fb = CA->create_widget(CA, fcls, pc);
    if (!wr || !ov || !bd || !vb || !tx || !tb || !sz || !sc || !fb) { logf_("weaponslot: row copy: widget creation failed"); return; }
    w->ui.fb = fb;                                                                                  // now: its Construct / focus events run with the kind's icon (ws_ev_pre)
    int nfb = ws_clone_props(fb0, fb, 0);
    { static const char *const ev[] = { "m_OnClick", "m_OnFocusReceived", "m_OnFocusLost", NULL }; static char b[600]; for (int i = 0; ev[i]; i++) if (CA->get_property_text(CA, fb0, ev[i], b, sizeof b) >= 0) CA->set_property_text(CA, fb, ev[i], b); }   // the badge: the menu's handlers of the vanilla family badge
    unsigned char f8 = (unsigned char)fam; ws_call1(fb, "BPF_SetWeaponFamily", &f8, NULL);
    int *ipr = CA->prop_ptr(CA, tb, "m_iItemsPerRow"); if (ipr) *ipr = w->rng.count < 4 ? 4 : w->rng.count;   // vanilla: 3 per row (its 4th box is outside the table's item grid); every box of ours is an item
    int added = 0;
    for (int k = 0; k < w->rng.count; k++) {
        void *b = CA->create_widget(CA, bcls, pc); w->ui.btn[k] = b; if (!b) continue; ws_clone_props(bt0, b, 0); ws_call1(b, "BPF_SetWeaponFamily", &f8, NULL);
        void *args[1] = { &b }; void *fa = CA->find_function(CA, tb, "BPF_AddItem"); if (fa && CA->call_function_params(CA, tb, fa, args, NULL)) added++;
        void *like = ws_child(hb0, k < 4 ? k : 3); void **ls = like ? CA->prop_ptr(CA, like, "Slot") : NULL, **ns = CA->prop_ptr(CA, b, "Slot"); if (ls && *ls && ns && *ns) ws_clone_props(*ls, *ns, 0);
    }
    {   void *hb = ws_child(tb, 0); void **ls = CA->prop_ptr(CA, hb0, "Slot"), **ns = hb ? CA->prop_ptr(CA, hb, "Slot") : NULL; char hc[80] = "-";
        if (hb && CA->object_class(CA, hb) == CA->object_class(CA, hb0)) ws_clone_props(hb0, hb, 0); if (ls && *ls && ns && *ns) ws_clone_props(*ls, *ns, 0);
        if (hb) CA->class_name(CA, CA->object_class(CA, hb), hc, sizeof hc); logf_("weaponslot: row copy: %d of %d box(es) added to the table (its row: %s %p)", added, w->rng.count, hc, hb); }
    ws_add(vb, tx, tx0); ws_add(vb, tb, tb0); ws_add(bd, vb, vb0); ws_add(sc, fb, fb0); ws_add(sz, sc, sc0);
    { int ib = -1, is = -1; ws_call1(ov0, "GetChildIndex", &bd0, &ib); ws_call1(ov0, "GetChildIndex", &sz0, &is); if (ib < is) { ws_add(ov, bd, bd0); ws_add(ov, sz, sz0); } else { ws_add(ov, sz, sz0); ws_add(ov, bd, bd0); } }
    ws_add(wr, ov, ov0);
    snprintf(w->ui.label_text, sizeof w->ui.label_text, "%s", w->rng.label); w->ui.label = tx; ws_label_refresh(NULL);
    {   // the rows box is on screen: its new slot through the setters (they reach the live Slate slot)
        void *slot = NULL; ws_call1(rows, "AddChild", &wr, &slot); void **ls = CA->prop_ptr(CA, wr0, "Slot");
        if (slot && ls && *ls) { float *pd = CA->prop_ptr(CA, *ls, "Padding"); uint8_t *ha = CA->prop_ptr(CA, *ls, "HorizontalAlignment"), *va = CA->prop_ptr(CA, *ls, "VerticalAlignment"); void *szz = CA->prop_ptr(CA, *ls, "Size");
            if (pd) ws_call1(slot, "SetPadding", pd, NULL); if (ha) ws_call1(slot, "SetHorizontalAlignment", ha, NULL); if (va) ws_call1(slot, "SetVerticalAlignment", va, NULL); if (szz) ws_call1(slot, "SetSize", szz, NULL); }
    }
    {   // the deck panel has a fixed height for three rows: grow the first canvas-placed ancestor of the rows by one row (vanilla row pitch 106 px at 1080p)
        void *c = rows; for (int d = 0; d < 12 && c; d++) { void *p = ws_parent(c); if (p && CA->is_a(CA, p, "CanvasPanel")) break; c = p; }
        void **cs = c ? CA->prop_ptr(CA, c, "Slot") : NULL; float size[2] = { 0, 0 }; char cn[80] = "";
        if (cs && *cs && CA->is_a(CA, *cs, "CanvasPanelSlot") && ws_call1(*cs, "GetSize", NULL, size)) { CA->object_name(CA, c, cn, sizeof cn); float ns[2] = { size[0], size[1] + 106.0f }, pos[2] = { 0, 0 }; ws_call1(*cs, "SetSize", ns, NULL); if (ws_call1(*cs, "GetPosition", NULL, pos)) { pos[1] += 53.0f; ws_call1(*cs, "SetPosition", pos, NULL); } logf_("weaponslot: panel %s grown %.0f x %.0f -> %.0f", cn, size[0], size[1], ns[1]); }
        else logf_("weaponslot: no canvas-placed panel above the rows - panel size unchanged");
    }
    w->ui.menu = menu; w->ui.row = wr; w->ui.fb = fb; w->ui.fb0 = fb0; ws_rebind(w);
    { static char b1[600], b2[600]; b1[0] = b2[0] = 0; if (w->ui.btn[0]) CA->get_property_text(CA, w->ui.btn[0], "m_OnClick", b1, sizeof b1); CA->get_property_text(CA, tb, "m_OnItemClicked", b2, sizeof b2);
      logf_("weaponslot: added the '%s' row %p (badge %p, %d badge properties copied, table %p) under %p; box 1 click -> %s; table click -> %s", w->ui.label_text, wr, fb, nfb, tb, rows, b1, b2); }
}
static void ws_menu(void *menu, const char *cls, const char *state, void *u) {
    (void)u; if (strcmp(cls, "BP_CharacterMenu_C") || strcmp(state, "CombatDeckFamilies")) return;
    for (int i = 0; i < g_ws_n; i++) if (g_ws[i].has_deck) ws_deck_row(menu, &g_ws[i]);
}

// ----- the kind as its OWN family entry (a separate entry with the kind's label; the base family entry stays vanilla).
// (a) Meditation > Attacks (BP_CharacterMenu state "AttackFamily"): the vanilla page has two designer buttons (AttackFamilyTable > VerticalBox_11 > HorizontalBox_20 /
//     _22 { ScaleBox > BP_AttackFamilyButton ; TextBlock label }); the table's only item is VerticalBox_11, the buttons carry their own click delegates
//     (BndEvt__BP_AttackFamilyButton_* -> CurrentWeaponFamily = 0 / 1, SetState Attacks). A third row "<deck label>" is a copy of the Sword row; its button's delegates
//     are the Sword button's, and a pre-hook on that click event remembers WHICH button was clicked (g_ws_list).
// (b) the list itself: UAttackComponent::FilterAttack (every attack list: Attacks page, deck editor, counts) is detoured: for the kind's base family it lists ONLY the
//     kind's rows when the kind entry is shown (the kind's Attacks page entry, or a kind deck is current in the deck editor) and hides them otherwise.
// (c) icons / names: our widgets read MenuDB in their Blueprint events (Construct, focus, select...): those events of OUR widgets run inside wp_view_push/pop
//     (weapons.inc), so they show the kind's label + icon while every vanilla reader keeps the base family.
static int g_ws_list = -1;                                                                          // g_ws_list: -1 = no Attacks-page choice, 0 = base family, n >= 1 = the kind g_ws[n-1] (one entry per kind)
static WsItem *ws_list_kind(void) { return g_ws_list >= 1 && g_ws_list <= g_ws_n ? &g_ws[g_ws_list - 1] : NULL; }
static WsItem *ws_ui_owner(void *o) { if (o) for (int i = 0; i < g_ws_n; i++) if (g_ws[i].ui.abtn == o || g_ws[i].ui.fb == o) return &g_ws[i]; return NULL; }   // the kind whose Attacks button / deck badge o is
static void *ws_local_kind_obj(void) { WsItem *w = ws_active(); return w ? w->it->obj : NULL; }
static unsigned char (*g_o_filter)(void *, uint8_t *, void *);
// A row is listed when its kind's entry is shown; a row of no kind when no kind entry is shown. Kind entry shown = chosen on the Attacks page, else the kind's deck is current.
static unsigned char ws_filter_det(void *ac, uint8_t *filter, void *row) {
    unsigned char r = g_o_filter(ac, filter, row);
    if (!r || !filter) return r;
    int n = 0, owner = -1; unsigned char show[WS_MAX], bare[WS_MAX];
    for (int i = 0; i < g_ws_n; i++) {
        WsItem *w = &g_ws[i]; CItem *it = w->it; if (!w->excl || !w->has_deck || filter[2] != it->family || !it->obj) continue;   // AttackFilter.m_eWeaponFamily (+0x02)
        if (g_ws_list >= 0) show[n] = g_ws_list == i + 1;
        else { uint64_t *c = cd_current_slot(it->family); show[n] = c && ws_own(w, *c) >= 0; }
        items_resolve_moves(it); for (int k = 0; k < it->nmoves; k++) if (it->move_rows[k] == row) owner = n;
        bare[n] = (unsigned char)(w->sword ? 2 : w->bare ? 1 : 0); n++;
    }
    return n ? (unsigned char)kr_filter(n, show, bare, owner, owner < 0 ? kr_row_level((const unsigned char *)row) : 0) : r;
}
// our widgets' MenuDB-reading events run with the kind's identity (on_call pre / post on these functions; obj filter below)
static int ws_ev_pre(void *obj, void *func, void *params, void *u) { (void)func; (void)params; (void)u; WsItem *w = ws_ui_owner(obj); if (w) wp_view_push(w->kind, w->rng.label); return 0; }
static void ws_ev_post(void *obj, void *func, void *params, void *u) { (void)func; (void)params; (void)u; if (ws_ui_owner(obj)) wp_view_pop(); }
// the Attacks list page entered after a click on a kind's button: same scoped view (header "AttackFamilyName" = the kind label, mini family table)
static int ws_list_pre(void *obj, void *func, void *params, void *u) { (void)obj; (void)func; (void)params; (void)u; WsItem *w = ws_list_kind(); if (w) wp_view_push(w->kind, w->rng.label); return 0; }
static void ws_list_post(void *obj, void *func, void *params, void *u) { (void)obj; (void)func; (void)params; (void)u; if (ws_list_kind()) wp_view_pop(); }
// which family button was clicked: the click events' first parameter is the button
static int ws_click_pre(void *obj, void *func, void *params, void *u) {
    (void)obj; (void)func; int fam = (int)(intptr_t)u; void *b = params ? *(void **)params : NULL; int nw = -1;
    for (int i = 0; b && i < g_ws_n; i++) if (b == g_ws[i].ui.abtn) nw = i + 1;
    if (nw < 0) nw = fam == 1 || fam == 0 ? 0 : -1;
    if (nw != g_ws_list) { if (nw >= 1) logf_("weaponslot: Attacks page: '%s' entry clicked", g_ws[nw - 1].rng.label); else logf_("weaponslot: Attacks page: %s entry clicked", nw == 0 ? "vanilla family" : "other"); }
    g_ws_list = nw; return 0;
}
static void ws_attacks_page(void *menu, WsItem *w) {
    if (menu == w->ui.amenu && w->ui.abtn) return;
    void **pt = CA->prop_ptr(CA, menu, "AttackFamilyTable"), **pb = CA->prop_ptr(CA, menu, "BP_AttackFamilyButton_C_300"), *pc = CA->get_local_player_controller(CA);
    void *tb = pt ? *pt : NULL, *bt0 = pb ? *pb : NULL, *sc0 = ws_parent(bt0), *hb0 = ws_parent(sc0), *vb = ws_parent(hb0), *tx0 = ws_child(hb0, 1);
    int ok = tb && bt0 && pc && vb && ws_parent(vb) == tb && CA->is_a(CA, sc0, "ScaleBox") && CA->is_a(CA, hb0, "HorizontalBox") && CA->is_a(CA, tx0, "TextBlock");
    logf_("weaponslot: Attacks page %p: table %p, Sword row %p (scale box %p, button %p, label %p) -> %s '%s'", menu, tb, hb0, sc0, bt0, tx0, ok ? "copying for" : "LAYOUT CHANGED - no kind entry for", w->kind);
    if (!ok) return;
    void *tree = *(void **)((uint8_t *)vb + 0x20), *hb = ws_new_like(tree, hb0, 1), *sc = ws_new_like(tree, sc0, 1), *tx = ws_new_like(tree, tx0, 1), *bt = CA->create_widget(CA, CA->object_class(CA, bt0), pc);
    if (!hb || !sc || !tx || !bt) { logf_("weaponslot: Attacks page: widget creation failed"); return; }
    int nb = ws_clone_props(bt0, bt, 1);                                                           // multicast delegates included: the click goes to the menu's Sword handler
    w->ui.abtn = bt; w->ui.amenu = menu; w->ui.alabel = tx;                                         // before AddChild: the button's Construct must already run with the kind view
    ws_add(sc, bt, bt0); ws_add(hb, sc, sc0); ws_add(hb, tx, tx0);
    { unsigned char t[24]; if (!CA->make_text(CA, w->rng.label, t)) ws_call1(tx, "SetText", t, NULL); }
    void *slot = NULL; ws_call1(vb, "AddChild", &hb, &slot); void **ls = CA->prop_ptr(CA, hb0, "Slot");
    if (slot && ls && *ls) { float *pd = CA->prop_ptr(CA, *ls, "Padding"); if (pd) ws_call1(slot, "SetPadding", pd, NULL); }
    logf_("weaponslot: Attacks page: '%s' entry %p added (button %p, %d properties copied)", w->rng.label, hb, bt, nb);
}
static void ws_menu_kinds(void *menu, const char *cls, const char *state, void *u) {
    (void)u; if (strcmp(cls, "BP_CharacterMenu_C")) return;
    if (strcmp(state, "Attacks")) g_ws_list = -1;                                                   // any other page: the deck editor rule (current deck) again
    if (!strcmp(state, "AttackFamily")) for (int i = 0; i < g_ws_n; i++) if (g_ws[i].has_deck && g_ws[i].kind[0]) ws_attacks_page(menu, &g_ws[i]);
    WsItem *w = ws_list_kind();
    if (w && !strcmp(state, "Attacks")) { void **pn = CA->prop_ptr(CA, menu, "AttackFamilyName"); unsigned char t[24]; if (pn && *pn && !CA->make_text(CA, w->rng.label, t)) ws_call1(*pn, "SetText", t, NULL); }
}
// Practice / deck editor family switch (BP_ComboEditorMenu.ChangeWeapon(family)): logs what the Blueprint will decide (it switches only when the current family's deck validates)
static int ws_change_weapon_pre(void *obj, void *func, void *params, void *u) {
    (void)func; (void)u; unsigned char to = params ? *(unsigned char *)params : 0xFF, cur = 0xFF; void *f = CA->find_function(CA, obj, "BPF_GetWeaponFamily"); if (f) CA->call_function_params(CA, obj, f, NULL, &cur);
    char d[64] = "?"; if (cur < 3) CA->combo_current(CA, cur, d, sizeof d);
    logf_("weaponslot: Practice / deck editor: family switch %d -> %d requested; current deck '%s' of family %d validates as %d (0 = switch, else the game shows its invalid-deck window)", cur, to, d, cur, cur < 3 ? ws_validate(cur) : -1);
    return 0;
}
// Practice badge "None" on an item deck: BP_CombatDeckFamilyButton.GetCombatDeckName reads BPF_GetCombo(family 0 (!), m_iSlotIndex) - a vanilla Blueprint bug that is
// invisible for vanilla decks (their names are "1".."4" in every family) but yields None for list entries past the Bare-Hands list (e.g. item decks 5-8).
// Fix: AFightingPlayerController::BPF_GetCombo's exec thunk is wrapped (replace_native); when the caller is GetCombatDeckName of a non-Bare-Hands badge, its out
// struct (the caller's local CallFunc_BPF_GetCombo__OutResult, FComboSave: m_ComboName at +0) gets the name of entry m_iSlotIndex of the badge's OWN family.
// (A post hook on GetCombatDeckName is too late: EX_Return already copied the name out.) FFrame: Node +0x10, Object +0x18, Locals +0x28.
static void (*g_o_getcombo)(void *, void *, void *);
static void ws_getcombo_thunk(void *ctx, void *frame, void *result) {
    g_o_getcombo(ctx, frame, result);
    __try {
        uint8_t *node = frame ? *(uint8_t **)((uint8_t *)frame + 0x10) : NULL; void *obj = frame ? *(void **)((uint8_t *)frame + 0x18) : NULL; uint8_t *locals = frame ? *(uint8_t **)((uint8_t *)frame + 0x28) : NULL;
        if (!node || !obj || !locals) return; char fn[64]; fname_text(*(FNameRaw *)(node + 0x18), fn, sizeof fn); if (strcmp(fn, "GetCombatDeckName")) return;
        uint8_t *fam = CA->prop_ptr(CA, obj, "m_eWeaponFamily"); int *si = CA->prop_ptr(CA, obj, "m_iSlotIndex"); uint8_t *op = api_p_find_property(NULL, node, "CallFunc_BPF_GetCombo__OutResult");
        if (!fam || !si || !op || *fam == 0 || *fam > 2 || *si < 0) return;
        SeTArr *a = cd_deck_list(*fam); if (!a || *si >= a->num) return;
        uint64_t *out = (uint64_t *)(locals + *(int *)(op + 0x44)), nm = *(uint64_t *)((uint8_t *)a->data + (size_t)*si * CD_COMBOSAVE_SIZE);
        if (*out != nm) { static int n; char was[64], now[64]; fname_text(*(FNameRaw *)out, was, sizeof was); fname_text(*(FNameRaw *)&nm, now, sizeof now); if (n++ < 6) logf_("weaponslot: deck badge (family %d, entry %d): name '%s' -> '%s' (the Blueprint reads the Bare-Hands list)", *fam, *si + 1, was, now); *out = nm; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
// deck editor weapon switch: BP_ComboEditorMenu.NextWeapon(direction) / BPE_SetNextWeapon (L1/R1, Practice keys 3 = next, 1 = previous). Params byte 0 = direction
// (0 next, 1 previous); the order of the families is Bare-Hands(0) -> Wargloves(2) -> Sword(1) -> Bare-Hands. With a kind equipped the Sword family already shows THAT kind's deck (the equipped kind owns the family's
// current deck), so a kind tab = one more step INSIDE the Sword family per OWNED kind: next / previous from the Sword family equips the next / previous owned kind (inventory equip -> ws_leave / ws_enter swap the decks
// and the animation set), and only past the last / first kind does the editor's own family switch run. The runtime deck is saved into its named deck first (BPF_SaveCombo) so edits survive the switch.
static void *g_ws_ed_menu;
static void ws_editor_refresh(void *menu, const char *why) {
    static const char *const fn[] = { "BPE_WeaponFamilyChanged", "BPF_NotifyDataUpdated", "LaunchNotifyDataUpdated", NULL };
    if (!menu || !tk_ptr_ok(menu)) return;
    for (int i = 0; fn[i]; i++) { void *f = CA->find_function(CA, menu, fn[i]); if (f && CA->call_function_params(CA, menu, f, NULL, NULL)) { logf_("weaponslot: deck editor %s: refreshed through %s", why, fn[i]); return; } }
    logf_("weaponslot: deck editor %s: NO refresh function found on the menu", why);
}
static void ws_editor_refresh_timer(void *u) { (void)u; ws_editor_refresh(g_ws_ed_menu, "after the kind switch (timer)"); }
static int ws_owned_kind(int i) { return i >= 0 && i < g_ws_n && g_ws[i].has_deck && g_ws[i].it->obj && g_ws[i].it->family == 1 && CA->inventory_index(CA, g_ws[i].it->obj) >= 0; }
static int ws_next_weapon_pre(void *obj, void *func, void *params, void *u) {
    (void)func; static int n; int log = n++ < 40;
    unsigned char cur = 0xFF, dir = 0xFF; void *f = CA->find_function(CA, obj, "BPF_GetWeaponFamily"); if (f) CA->call_function_params(CA, obj, f, NULL, &cur);
    __try { if (params) dir = *(unsigned char *)params; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    // Experimental, OFF by default (config deck_editor_kind_tab = 1): in Practice EquipFromInventory returns 1 but the weapon does not change, so the switch does nothing there.
    // Equip the other kind in Meditation > Equipment first: the Sword family of the editor then shows that kind's decks.
    { static int on = -1; if (on < 0) { char b[16] = ""; wchar_t src[MAX_PATH]; on = setting_get("deck_editor_kind_tab", L"", 0, b, sizeof b, src) ? atoi(b) != 0 : 0; } if (!on) return 0; }
    if (strcmp((const char *)u, "NextWeapon") || cur != 1 || dir > 1) return 0;                  // only NextWeapon (BPE_SetNextWeapon calls it), only from the Sword family
    WsItem *w = ws_active(); if (!w) return 0; int pos = (int)(w - g_ws);                          // a kind must be equipped (its deck is the family's current deck)
    int to = -1; if (dir == 0) { for (int j = pos + 1; j < g_ws_n && to < 0; j++) if (ws_owned_kind(j)) to = j; } else { for (int j = pos - 1; j >= 0 && to < 0; j--) if (ws_owned_kind(j)) to = j; }
    if (to < 0) return 0;                                                                           // last / first kind: the editor's own switch to the next / previous family
    int v = ws_validate(1); if (v) { if (log) logf_("weaponslot: deck editor: kind switch '%s' -> '%s' not done, the current deck is invalid (%d): the editor's own window decides", w->rng.label, g_ws[to].rng.label, v); return 0; }
    void *pc = L_local_pc(), *sf = pc ? CA->find_function(CA, pc, "BPF_SaveCombo") : NULL; int saved = sf ? CA->call_function_params(CA, pc, sf, NULL, NULL) : -1;
    int idx = CA->inventory_index(CA, g_ws[to].it->obj), r = 0; g_ws_ed_menu = obj; g_ws_cur_at = 0;
    { void *ef = pc ? CA->find_function(CA, pc, "EquipFromInventory") : NULL; uint32_t ui = (uint32_t)idx; int slot = -1; void *ea[2] = { &ui, &slot }; if (ef && idx >= 0) r = CA->call_function_params(CA, pc, ef, ea, NULL); }   // controller route (the InventoryMenu route drives the menu's character preview)
    logf_("weaponslot: deck editor: %s from Sword kind '%s' -> kind '%s' (%s): deck saved %d, inventory %d equipped -> %d", dir ? "previous" : "next", w->rng.label, g_ws[to].rng.label, g_ws[to].it->id, saved, idx, r);
    ws_editor_refresh(obj, "kind switch"); CA->set_timer(CA, 0.5f, 0, ws_editor_refresh_timer, NULL); return 1;
}
static void ws_kind_hooks(void) {
    static int done; if (done) return; done = 1;
    g_o_filter = (unsigned char (*)(void *, uint8_t *, void *))CA->hook_symbol(CA, "UAttackComponent::FilterAttack", (void *)ws_filter_det);
    static const char *const ev[][2] = { { "BP_AttackFamilyButton_C", "Construct" }, { "BP_AttackFamilyButton_C", "BPE_OnSelected" }, { "BP_AttackFamilyButton_C", "BPE_OnDeselected" },
        { "BP_AttackFamilyButton_C", "BPE_OnSynchronizeProperties" }, { "BP_CombatDeckFamilyButton_C", "Construct" }, { "BP_CombatDeckFamilyButton_C", "OnFocusReceived" },
        { "BP_CombatDeckFamilyButton_C", "OnFocusLost" }, { "BP_CombatDeckFamilyButton_C", "BPE_OnDataUpdated" } };
    int n = 0; for (size_t i = 0; i < sizeof ev / sizeof ev[0]; i++) n += !CA->on_call(CA, ev[i][0], ev[i][1], ws_ev_pre, ws_ev_post, NULL);
    n += !CA->on_call(CA, "BP_CharacterMenu_C", "EnterState Attacks", ws_list_pre, ws_list_post, NULL);
    n += !CA->on_call(CA, "BP_CharacterMenu_C", "BndEvt__BP_AttackFamilyButton_C_300_K2Node_ComponentBoundEvent_5471_ButtonUserWidgetClickDelegate__DelegateSignature", ws_click_pre, NULL, (void *)(intptr_t)1);
    n += !CA->on_call(CA, "BP_CharacterMenu_C", "BndEvt__BP_AttackFamilyButton_299_K2Node_ComponentBoundEvent_5394_ButtonUserWidgetClickDelegate__DelegateSignature", ws_click_pre, NULL, (void *)(intptr_t)0);
    n += !CA->on_call(CA, "BP_ComboEditorMenu_C", "ChangeWeapon", ws_change_weapon_pre, NULL, NULL);
    n += !CA->on_call(CA, "BP_ComboEditorMenu_C", "NextWeapon", ws_next_weapon_pre, NULL, (void *)"NextWeapon"); n += !CA->on_call(CA, "BP_ComboEditorMenu_C", "BPE_SetNextWeapon", ws_next_weapon_pre, NULL, (void *)"BPE_SetNextWeapon");
    n += !CA->replace_native(CA, "FightingPlayerController", "BPF_GetCombo", (void *)ws_getcombo_thunk, (void **)&g_o_getcombo);
    logf_("weaponslot: kind entries: FilterAttack hook %p, %d of 13 widget event hooks + deck badge name fix, menu event %d, impact sound log %d", (void *)g_o_filter, n, CA->on_menu(CA, ws_menu_kinds, NULL), CA->on_hit(CA, wp_on_hit, NULL));
}

// ----- registration from items_apply (after the object exists and "moves" were bound)
static void ws_item_ready(CItem *it, const char *json) {
    static int anims_hooked; if (it->nmoves && !anims_hooked) { anims_hooked = 1; logf_("content: row animation preload on equip: weapon event %d", CA->on_weapon_changed(CA, ws_anims_weapon_changed, NULL)); }   // before ws_weapon_changed: animations in memory before the item's deck loads
    // an item that names a weapon kind ("weapon") takes these fields from the kind entry; item-level fields are deprecated
    const char *kj = NULL;
    {   JDoc di; if (jdoc_parse(&di, json) != 0) return; char own[120] = ""; size_t ow = 0;
        for (int i = 0; WP_ITEM_OWNED[i]; i++) if (jfind(&di, 0, WP_ITEM_OWNED[i]) >= 0) ow += (size_t)snprintf(own + ow, ow < sizeof own ? sizeof own - ow : 0, "%s%s", ow ? ", " : "", WP_ITEM_OWNED[i]);
        jdoc_free(&di);
        if (it->weapon[0]) { kj = wp_json(it->weapon); if (!kj) { logf_("weaponslot: item %s: weapon kind '%s' not found - no decks / anim set / exclusivity", it->id, it->weapon); return; }
            if (own[0]) logf_("weaponslot: item %s: field(s) %s IGNORED: the weapon kind '%s' defines them", it->id, own, it->weapon); }
        else if (own[0]) logf_("weaponslot: item %s: field(s) %s on the item are DEPRECATED: put them in a \"weapons\" kind entry and give the item \"weapon\": \"<kind id>\" (still read)", it->id, own);
    }
    if (kj) json = kj;
    JDoc d; if (jdoc_parse(&d, json) != 0) return;
    int dk = jfind(&d, 0, "deck"); const char *av = cs_str(&d, 0, "anim_style"), *asv = cs_str(&d, 0, "anim_set"), *sv = cs_str(&d, 0, "style"), *dv = cs_str(&d, 0, "deflect_style"); int ex = cs_true(&d, 0, "moves_exclusive");
    if (sv) { logf_("weaponslot: item %s: \"style\" is DEPRECATED (it no longer changes the fighting style): read as \"anim_style\"%s", it->id, av ? " - ignored, \"anim_style\" is given" : ""); if (!av) av = sv; }
    if (dk < 0 && !av && !asv && !ex) { jdoc_free(&d); return; }
    { char fam[40] = ""; CA->get_property_text(CA, it->obj, "m_eWeaponFamily", fam, sizeof fam); it->family = !strcmp(fam, "BareHands") ? 0 : !strcmp(fam, "Sword") ? 1 : !strcmp(fam, "WarGloves") ? 2 : -1; }
    if (it->family < 0) { logf_("weaponslot: item %s: deck / anim_style / moves_exclusive need a weapon (family %d) - ignored", it->id, it->family); jdoc_free(&d); return; }
    if (kj) { int bf = wp_family_of(cs_str(&d, 0, "base_family")); if (bf != it->family) { logf_("weaponslot: item %s: its base asset is family %d but weapon kind '%s' is base_family %d - the kind is NOT applied", it->id, it->family, it->weapon, bf); jdoc_free(&d); return; } }
    WsItem *w = NULL; for (int i = 0; i < g_ws_n; i++) if (g_ws[i].it == it) w = &g_ws[i];
    if (!w && g_ws_n >= WS_MAX) { logf_("weaponslot: item %s ignored: already %d weapon kinds (WS_MAX)", it->id, WS_MAX); jdoc_free(&d); return; }
    if (!w) { w = &g_ws[g_ws_n++]; memset(w, 0, sizeof *w); }
    w->it = it; w->excl = ex; w->bare = cs_true(&d, 0, "allow_bare_hands"); w->sword = cs_true(&d, 0, "allow_sword_moves"); w->has_deck = dk >= 0; snprintf(w->kind, sizeof w->kind, "%s", it->weapon);   // sword: the kind also takes every vanilla Sword row (one-way)
    w->anim_style = av ? atoi(av) : -1; if (w->anim_style > 63) { logf_("weaponslot: item %s: anim_style %d outside 0..63 - ignored", it->id, w->anim_style); w->anim_style = -1; }
    snprintf(w->anim_set, sizeof w->anim_set, "%s", asv ? asv : "");
    w->deflect_style = dv && (w->anim_style >= 0 || w->anim_set[0]) ? atoi(dv) : -1; if (w->deflect_style > 63) w->deflect_style = -1;
    if (dk >= 0) {
        char re[200]; if (dl_parse_range(&d, dk, &w->rng, re, sizeof re)) { logf_("weaponslot: item %s: \"deck\" ignored (no item decks, no Combat Deck row): %s", it->id, re); w->has_deck = 0; }
        if (kj && jfind(&d, dk, "label") < 0) { const char *lb = cs_str(&d, 0, "label"); snprintf(w->rng.label, sizeof w->rng.label, "%s", lb ? lb : it->weapon); }   // the kind's label names its deck row
        for (int k = 0; k < w->rng.count; k++) CA->fname_make(CA, w->rng.names[k], &w->fns[k]);
        if (w->has_deck && !w->rng.legacy && w->rng.first <= 4) logf_("weaponslot: item %s: deck.first_slot %d overlaps the 4 vanilla boxes of the family row", it->id, w->rng.first);
        // two DIFFERENT kinds of one family must not share deck boxes (same kind = same decks on purpose): the later one loses its decks instead of writing into the other's
        if (w->has_deck && !w->rng.legacy) for (int o = 0; o < g_ws_n; o++) { const WsItem *x = &g_ws[o];
            if (x == w || !x->has_deck || x->rng.legacy || !x->it || x->it->family != it->family || !strcmp(x->kind, w->kind)) continue;
            if (w->rng.first < x->rng.first + x->rng.count && x->rng.first < w->rng.first + w->rng.count) {
                logf_("weaponslot: item %s (kind %s): deck boxes %d-%d OVERLAP kind %s (%d-%d) on the same family - its decks are OFF; give it a free deck.first_slot", it->id, w->kind, w->rng.first, w->rng.first + w->rng.count - 1, x->kind, x->rng.first, x->rng.first + x->rng.count - 1);
                w->has_deck = 0; break; } }
        int sd = jfind(&d, dk, "seed"); w->nseed = 0; { const char *lo = cs_str(&d, dk, "lock_family_decks"); w->lock_others = lo && (!_stricmp(lo, "true") || !strcmp(lo, "1")); }
        if (sd >= 0) { for (int c = d.n[sd].first; c >= 0 && w->nseed < 16; c = d.n[c].next) { char why[120]; if (d.n[c].t != J_STR || !d.n[c].str) { logf_("weaponslot: item %s: deck.seed entry is not a string - dropped", it->id); continue; }
            if (!dl_name_check(d.n[c].str, why, sizeof why)) { logf_("weaponslot: item %s: deck.seed entry '%.80s' REJECTED: %s", it->id, d.n[c].str, why); continue; }
            snprintf(w->seed[w->nseed++], DL_ID_BUF, "%s", d.n[c].str); } }
        else for (int k = 0; k < it->nmoves && w->nseed < 16; k++) if (strcmp(it->moves[k], "321")) snprintf(w->seed[w->nseed++], DL_ID_BUF, "%s", it->moves[k]);
    }
    jdoc_free(&d);
    if (w->excl && !g_o_famchk) { g_o_famchk = (unsigned char (*)(unsigned char, void *, unsigned char, unsigned char))CA->hook_symbol(CA, "UAttackComponent::IsAttackCompatibleWithWeaponFamily", (void *)items_famchk_det); logf_("content: family-check hook -> %p", (void *)g_o_famchk); }
    if (!g_ws_hooked) {
        g_ws_hooked = 1;
        g_o_slotunl = (unsigned char (*)(void *, unsigned char, uint64_t *, unsigned char))CA->hook_symbol(CA, "UStatsComponent::BPF_IsComboSlotUnlocked", (void *)ws_slotunl_det);
        g_o_writelevels = (void (*)(void *))CA->hook_symbol(CA, "UThePlainesGameInstance::WriteLevelsToSave", (void *)ws_writelevels_det); g_o_launchsave = (void (*)(void *, void *, unsigned char))CA->hook_symbol(CA, "UThePlainesGameInstance::LaunchSave", (void *)ws_launchsave_det); g_o_savecombo = (void (*)(void *, uint64_t, unsigned char))CA->hook_symbol(CA, "AFightingPlayerController::SaveCombo", (void *)ws_savecombo_det);
        g_o_ticksave = (void (*)(void *))CA->hook_symbol(CA, "UThePlainesGameInstance::TickSaveLoad", (void *)ws_ticksave_det);
        logf_("weaponslot: hooks slot-unlock %p, save %p / %p / save-completed %p%s; weapon event %d, in-hand event %d, style heal event %d, menu event %d, deck watch timer %d", (void *)g_o_slotunl, (void *)g_o_writelevels, (void *)g_o_launchsave, (void *)g_o_ticksave, g_o_ticksave ? "" : " (MISSING: the live deck comes back on the old 1.5 s timer)",
            CA->on_weapon_changed(CA, ws_weapon_changed, NULL), CA->on_handled_weapon(CA, ws_handled, NULL), CA->on_style_changed(CA, ws_style_heal, NULL), CA->on_menu(CA, ws_menu, NULL), CA->set_timer(CA, 0.25f, 1, ws_deck_watch, NULL));
        ws_kind_hooks();
    }
    { int nl = 0; for (int k = 0; k < w->rng.count; k++) nl += w->rng.has[k] != 0;
      logf_("weaponslot: item %s: family %d, decks %s..%s (%d, %d with a layout, %d seed row(s))%s, row label '%s', anim_style %d (while drawn), deflect_style %d, moves_exclusive %d", it->id, it->family, w->has_deck ? w->rng.names[0] : "-", w->has_deck ? w->rng.names[w->rng.count - 1] : "-", w->has_deck ? w->rng.count : 0, nl, w->nseed, w->rng.legacy ? " (legacy \"slot\")" : "", w->rng.label, w->anim_style, w->deflect_style, w->excl); }
}

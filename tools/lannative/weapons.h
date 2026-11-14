// weapons.h - pure logic of the "weapons" content kind. No OS / engine calls, so it can be tested offline.
// A weapon KIND is a weapon type on one of the three compiled base families (there is no real 4th family). It ties decks, animations and moves together:
//   Mods\<mod>\content\weapons\<file>.json   { "weapons": [ {
//     "id": "Staff", "label": "Staff",                        label = UI identity of the kind's own menu entries (deck row, Attacks page entry)
//     "base_family": "Sword",                                 BareHands | Sword | WarGloves: the family the kind lives on (its items must be of it)
//     "icon_file": "staff_family.png" | "icon": "/Game/..."   family icon of the kind's own menu entries (optional)
//     "deck": { "first_slot": 5, "count": 4, ... },           the kind's deck range in the base family's list (decklayout.h dl_parse_range)
//     "anim_set": "Staff", "anim_style": 7, "deflect_style": 6,    animations while drawn (animsets.inc / animstyle.inc)
//     "moves": ["367", ...], "moves_exclusive": true,        attack rows of the kind (runtime rows from the mod's rows.json, "learn": "always")
//     "damage": { "special_ratio": 0 },                       share of "special" (CUT) damage, the rest is base (BLUNT): the gear row's m_fAttackSpecialRatio(+Dirty/Gold)
//     "grip": { "socket": "RightHandSocket", "capsule": { "half_height": 94.6, "radius": 5, "offset": 0.9 } },   hit capsule = gear row values; socket logged only
//     "defense": { ... } } ] }                                placeholder (deflect defence): parsed, logged, not used
// Items reference a kind with "weapon": "<id>": the kind's moves are added to the item's, the damage / grip values to the item's gear_row "set" (item values win),
// the deck / animation / exclusivity fields come from the kind. The per-item fields still work without "weapon" (logged as deprecated).
#ifndef WEAPONS_H
#define WEAPONS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "decklayout.h"                                     // rowsjson.h JDoc + dl_parse_range

#define WP_MOVES_MAX 128
#define WP_GS_MAX 8
typedef struct {
    char id[64], label[64], icon[300], icon_file[260], socket[64];
    int family, has_deck, has_defense, nmoves, ngs;
    char moves[WP_MOVES_MAX][DL_ID_BUF];
    char gsk[WP_GS_MAX][64], gsv[WP_GS_MAX][32];          // gear row values derived from "damage" / "grip"
    float mesh_off[3], mesh_rot[3]; int has_mesh_off, has_mesh_rot;   // "grip.mesh_offset" / "grip.mesh_rotation": the weapon mesh in the hand (cm along the socket axes / degrees p,y,r), added to the stock transform
    int allow_sword, hand_switch;                         // "allow_sword_moves": vanilla Sword rows usable in the kind's decks (one-way); "hand_switch": "mirror" = the weapon goes to the LEFT hand for a mirrored attack
} WpKind;

static const char *const WP_FAMILY[3] = { "BareHands", "Sword", "WarGloves" };
static int wp_family_of(const char *s) { for (int i = 0; s && i < 3; i++) if (!strcmp(s, WP_FAMILY[i])) return i; return -1; }
static const char *wp_str(const JDoc *d, int o, const char *k) { int c = o >= 0 ? jfind(d, o, k) : -1; return c >= 0 && (d->n[c].t == J_STR || d->n[c].t == J_NUM || d->n[c].t == J_BOOL) ? d->n[c].str : NULL; }
// a number field of object o: 1 = present and inside [lo, hi] (text kept as given), 0 = absent, -1 = present but bad
static int wp_num(const JDoc *d, int o, const char *k, double lo, double hi, char *text, size_t cap) {
    int c = o >= 0 ? jfind(d, o, k) : -1; if (c < 0) return 0;
    if (d->n[c].t != J_NUM || !d->n[c].str) return -1; char *e; double v = strtod(d->n[c].str, &e); if (*e || v < lo || v > hi) return -1;
    snprintf(text, cap, "%s", d->n[c].str); return 1;
}
// a 3-number array field: 1 = present and every value inside [lo, hi], 0 = absent, -1 = bad
static int wp_vec3(const JDoc *d, int o, const char *k, double lo, double hi, float out[3]) {
    int c = o >= 0 ? jfind(d, o, k) : -1; if (c < 0) return 0; if (d->n[c].t != J_ARR) return -1; int i = 0;
    for (int e = d->n[c].first; e >= 0; e = d->n[e].next, i++) { if (i >= 3 || d->n[e].t != J_NUM || !d->n[e].str) return -1; char *x; double v = strtod(d->n[e].str, &x); if (*x || v < lo || v > hi) return -1; out[i] = (float)v; }
    return i == 3 ? 1 : -1;
}
static int wp_gs_add(WpKind *k, const char *key, const char *val) { if (k->ngs >= WP_GS_MAX) return 0; snprintf(k->gsk[k->ngs], 64, "%s", key); snprintf(k->gsv[k->ngs], 32, "%s", val); k->ngs++; return 1; }

// parse + validate one entry (the object at node o of d; o = 0 for a single-entry document). 1 = ok, 0 = invalid (err says why)
static int wp_parse_node(const JDoc *d, int o, WpKind *k, char *err, size_t cap) {
#define WP_FAIL(...) do { if (err && cap) snprintf(err, cap, __VA_ARGS__); return 0; } while (0)
    memset(k, 0, sizeof *k); if (err && cap) err[0] = 0; char t[32];
    if (o < 0 || d->n[o].t != J_OBJ) WP_FAIL("a weapon kind must be an object");
    const char *v = wp_str(d, o, "id"); if (!v || !v[0] || strlen(v) >= sizeof k->id) WP_FAIL("needs \"id\" (1..63 characters)"); snprintf(k->id, sizeof k->id, "%s", v);
    v = wp_str(d, o, "label"); snprintf(k->label, sizeof k->label, "%s", v && v[0] ? v : k->id);
    v = wp_str(d, o, "base_family"); k->family = wp_family_of(v); if (k->family < 0) WP_FAIL("needs \"base_family\": BareHands, Sword or WarGloves (got '%s')", v ? v : "nothing");
    if ((v = wp_str(d, o, "icon"))) { if (v[0] != '/' || !strchr(v, '.')) WP_FAIL("\"icon\" must be an object path like /Game/UI/X.X"); snprintf(k->icon, sizeof k->icon, "%s", v); }
    if ((v = wp_str(d, o, "icon_file"))) { if (strstr(v, "..") || strchr(v, ':') || v[0] == '\\' || v[0] == '/') WP_FAIL("\"icon_file\" must be a file inside the mod folder"); snprintf(k->icon_file, sizeof k->icon_file, "%s", v); }
    int dk = jfind(d, o, "deck"); if (dk >= 0) { static DlRange r; char re[200]; if (dl_parse_range(d, dk, &r, re, sizeof re)) WP_FAIL("\"deck\": %s", re); k->has_deck = 1; }
    if (wp_num(d, o, "anim_style", 0, 63, t, sizeof t) < 0) WP_FAIL("\"anim_style\" must be a number 0..63");
    if (wp_num(d, o, "deflect_style", 0, 63, t, sizeof t) < 0) WP_FAIL("\"deflect_style\" must be a number 0..63");
    int mv = jfind(d, o, "moves");
    if (mv >= 0) { if (d->n[mv].t != J_ARR) WP_FAIL("\"moves\" must be an array of attack row names");
        for (int c = d->n[mv].first; c >= 0; c = d->n[c].next) { char why[120]; if (d->n[c].t != J_STR || !d->n[c].str) WP_FAIL("\"moves\": every entry must be a row name (string)"); if (!dl_name_check(d->n[c].str, why, sizeof why)) WP_FAIL("\"moves\": entry '%.60s' REJECTED: %s", d->n[c].str, why);
            int dup = 0; for (int i = 0; i < k->nmoves; i++) if (!strcmp(k->moves[i], d->n[c].str)) dup = 1;
            if (!dup) { if (k->nmoves >= WP_MOVES_MAX) WP_FAIL("\"moves\": more than %d rows", WP_MOVES_MAX); snprintf(k->moves[k->nmoves++], DL_ID_BUF, "%s", d->n[c].str); } } }
    int dm = jfind(d, o, "damage");
    if (dm >= 0) { if (d->n[dm].t != J_OBJ) WP_FAIL("\"damage\" must be an object");
        int r = wp_num(d, dm, "special_ratio", 0, 1, t, sizeof t); if (r < 0) WP_FAIL("\"damage.special_ratio\" must be a number 0..1 (0 = all blunt, 1 = all cut)");
        if (r > 0) { wp_gs_add(k, "m_fAttackSpecialRatio", t); wp_gs_add(k, "m_fAttackSpecialRatioDirty", t); wp_gs_add(k, "m_fAttackSpecialRatioGold", t); } }
    int gp = jfind(d, o, "grip");
    if (gp >= 0) { if (d->n[gp].t != J_OBJ) WP_FAIL("\"grip\" must be an object");
        if ((v = wp_str(d, gp, "socket"))) snprintf(k->socket, sizeof k->socket, "%s", v);
        int cp = jfind(d, gp, "capsule");
        if (cp >= 0) { if (d->n[cp].t != J_OBJ) WP_FAIL("\"grip.capsule\" must be an object");
            static const char *const ck[3] = { "half_height", "radius", "offset" }, *const gk[3] = { "m_fWeaponCollisionCapsuleHalfHeight", "m_fWeaponCollisionCapsuleRadius", "m_fWeaponCollisionCapsuleOffset" };
            for (int i = 0; i < 3; i++) { int r = wp_num(d, cp, ck[i], i == 2 ? 0 : 0.1, 1000, t, sizeof t); if (r < 0) WP_FAIL("\"grip.capsule.%s\" must be a number %s..1000 (the engine clamps a negative offset to 0)", ck[i], i == 2 ? "0" : "0.1"); if (r > 0) wp_gs_add(k, gk[i], t); } } }
    if (gp >= 0) { int r = wp_vec3(d, gp, "mesh_offset", -50, 50, k->mesh_off); if (r < 0) WP_FAIL("\"grip.mesh_offset\" must be [x, y, z] in cm, each -50..50 (socket frame, +z = along the blade / shaft)"); k->has_mesh_off = r > 0;
        r = wp_vec3(d, gp, "mesh_rotation", -180, 180, k->mesh_rot); if (r < 0) WP_FAIL("\"grip.mesh_rotation\" must be [pitch, yaw, roll] in degrees, each -180..180"); k->has_mesh_rot = r > 0; }
    { int c = jfind(d, o, "allow_sword_moves"); if (c >= 0) { if (d->n[c].t != J_BOOL) WP_FAIL("\"allow_sword_moves\" must be true or false"); k->allow_sword = !strcmp(d->n[c].str, "True"); }
      if (k->allow_sword && k->family != 1) WP_FAIL("\"allow_sword_moves\" needs \"base_family\": \"Sword\""); }
    if ((v = wp_str(d, o, "hand_switch"))) { if (!strcmp(v, "mirror")) k->hand_switch = 1; else if (!strcmp(v, "off")) k->hand_switch = 0; else WP_FAIL("\"hand_switch\" must be \"mirror\" or \"off\""); }
    int df = jfind(d, o, "defense"); if (df >= 0) { if (d->n[df].t != J_OBJ) WP_FAIL("\"defense\" must be an object (reserved, not used)"); k->has_defense = 1; }
    return 1;
#undef WP_FAIL
}
static int wp_parse(const char *json, WpKind *k, char *err, size_t cap) {
    JDoc d; if (jdoc_parse(&d, json) != 0) { if (err && cap) snprintf(err, cap, "%s", d.err); jdoc_free(&d); memset(k, 0, sizeof *k); return 0; }
    int ok = wp_parse_node(&d, 0, k, err, cap); jdoc_free(&d); return ok;
}
// item merge: the kind's moves after the item's own (no duplicates); returns the new count (stops at cap)
static int wp_merge_moves(char (*dst)[DL_ID_BUF], int n, int cap, const WpKind *k) {
    for (int i = 0; i < k->nmoves && n < cap; i++) { int dup = 0; for (int j = 0; j < n; j++) if (!strcmp(dst[j], k->moves[i])) dup = 1; if (!dup) snprintf(dst[n++], DL_ID_BUF, "%s", k->moves[i]); }
    return n;
}
// item merge: the kind's gear row values for keys the item does not set itself (item values win); returns the new count, *added = how many were added
static int wp_merge_gear(char (*keys)[64], char (*vals)[200], int n, int cap, const WpKind *k, int *added) {
    int a = 0;
    for (int i = 0; i < k->ngs && n < cap; i++) { int have = 0; for (int j = 0; j < n; j++) if (!strcmp(keys[j], k->gsk[i])) have = 1; if (!have) { snprintf(keys[n], 64, "%s", k->gsk[i]); snprintf(vals[n], 200, "%s", k->gsv[i]); n++; a++; } }
    if (added) *added = a; return n;
}
// impact sound switches the game sets for a hit (decoded from BP_TPSCharacter.SetAttackingWeaponMatSFXSwitch and BPAttackComponent's ubergraph): the attack
// row's weapon slot (EWeaponSlot: 0/1 = the weapon hands, 2..5 = legs / arms), the family of the weapon in the attacker's hand (-1 = none) and the surface of that slot
// (ABaseCharacter::BPF_GetSurfaceTypeFromWeaponSlot = the gear row's m_eSurfaceType; EPhysicalSurface 11 = "Wood" in DefaultEngine.ini). Event: Human_Foley_Fight_Hit.
static void wp_sound(int slot, int family, int surface, const char **attack_type, const char **hit_mat, const char **weapon_mat) {
    static const char *const hit[15] = { "Fabric", "Fabric", "Fabric", "Essence", "Fabric", "Flesh", "Fabric", "Fabric", "Metal", "Fabric", "Fabric", "Wood", "Leather", "Metal", "None" };
    static const char *const wea[15] = { "Metal", "Metal", "Metal", "Metal", "Metal", "Metal", "None", "None", "Metal", "Metal", "None", "Wood", "Metal", "Metal", "None" };
    int s = surface >= 0 && surface < 15 ? surface : 14;
    *attack_type = slot > 5 ? "(unchanged: slot None)" : slot >= 2 || family < 0 ? "Bare_Hands" : family == 1 ? "Sword" : family == 2 ? "Wargloves" : "Bare_Hands";
    *hit_mat = slot > 5 ? "(unchanged)" : hit[s]; *weapon_mat = wea[s];   // the weapon mat is set from slot 0 (the hand weapon) whatever the attack slot
}
// weapon hit sweep densifier: the engine sweeps one sphere per cast boundary (FHitRequest.m_CastBoundaries, 0x18 bytes =
// start xyz -> end xyz = one sample point of the weapon from the previous to the current pose); a weapon has 3 points (-hh, hand, +hh): e.g. ~94 uu apart for a long pole weapon,
// so a target between the hand and a tip is never touched. The points are collinear on the rigid weapon in both poses, so points in between are exact linear
// interpolations of neighbouring casts. in: n casts (6 floats each); out: the same casts with extra ones inserted so no two neighbours (start OR end) are more than
// max_gap apart; returns the new count (n unchanged when no gap exceeds max_gap, or cap would be exceeded).
// group = points per weapon line: the request holds one line of 3 points per weapon (or per sub-step, e.g. 6 = 2 x 3); points are only interpolated
// INSIDE a group, never from the last point of one line to the first of the next.
static int wp_densify(const float *in, int n, int group, float max_gap, float *out, int cap) {
    int m = 0; if (group < 2 || n % group) group = n;
    for (int i = 0; i < n; i++) {
        if (m >= cap) return n;
        memcpy(out + m * 6, in + i * 6, 6 * sizeof(float)); m++;
        if (i + 1 == n || (i + 1) % group == 0) continue;
        const float *a = in + i * 6, *b = in + (i + 1) * 6; float g = 0;
        for (int e = 0; e < 2; e++) { float dx = b[e * 3] - a[e * 3], dy = b[e * 3 + 1] - a[e * 3 + 1], dz = b[e * 3 + 2] - a[e * 3 + 2], d = sqrtf(dx * dx + dy * dy + dz * dz); if (d > g) g = d; }
        int k = max_gap > 0 ? (int)ceilf(g / max_gap) : 1;                                        // k segments -> k-1 extra points
        for (int j = 1; j < k; j++) { if (m >= cap) return n; float t = (float)j / (float)k; for (int c = 0; c < 6; c++) out[m * 6 + c] = a[c] + (b[c] - a[c]) * t; m++; }
    }
    return m;
}
// mirrored hand strikes: OrderAttack::HitRequestComputeInfo / HitRequestComputeNextInfoAndSend swap the strike slot of a
// MIRRORED attack with GetMirrorWeaponSlot (RightHand <-> LeftHand ...) and return when the character's weapon actor of that slot is NULL. A Sword-family weapon
// (vanilla sword or a weapon kind on Sword) has only the RightHand actor, so a mirrored RightHand strike never makes a hit request. Fix: for the duration of the call,
// the EMPTY hand slot borrows the other hand's actor (same actor, same socket: the sweep follows the weapon in the right hand of the mirrored pose).
// Returns the hand slot to fill (0 LeftHand, 1 RightHand) or -1 = leave the character alone.
// Only for a character holding a weapon KIND item: the vanilla sword keeps its own behaviour (a vanilla deck can play a sword hand row mirrored,
// e.g. 009m, and the game then makes no hit request; left as the game has it). Not mirrored, both hands present (WarGloves, bare-hand limbs) or both empty: -1.
static int wp_mirror_fill(int mirrored, int kind_in_hand, int left_present, int right_present) {
    if (!mirrored || !kind_in_hand || left_present == right_present) return -1;
    return left_present ? 1 : 0;
}

// effects follow the weapon (otherwise a mirrored strike of a one-hand kind weapon hits but never gets the weapon's impact effects / sound / damage type). The game asks
// AFightingCharacter::GetWeaponSlotAttacking (0x363D00: the strike slot, swapped by GetMirrorWeaponSlot for a mirrored attack) and then compares it with the slots of the
// weapon in hand (HandledVisibleWeaponIsAttacking: GenerateImpact, OrderHitted::OnStart, GetCurrentHitDamage, weapon durability) or reads the actor of that slot
// (GetActiveWeapons). A one-hand weapon has only the RightHand slot, so a mirrored hand strike (LeftHand) counts as a bare-handed hit. Rule: a HAND slot whose
// actor is missing while the other hand holds one resolves to that other hand (the weapon), for a character holding a weapon kind. Returns the slot to report.
static int wp_slot_follow(int slot, int left_present, int right_present) {
    if (slot != 0 && slot != 1) return slot;
    int here = slot == 0 ? left_present : right_present, there = slot == 0 ? right_present : left_present;
    return !here && there ? 1 - slot : slot;
}
// hand switch: which hand the weapon of a kind with "hand_switch": "mirror" belongs in for an attack: 1 = LEFT (mirrored attack), 0 = RIGHT; -1 = no switching
static int wp_hand_want(int hand_switch, int mirrored) { return hand_switch ? (mirrored ? 1 : 0) : -1; }
// hand switch BACK to the right hand (0.1 s tick). In a combo the NEXT attack starts before the previous order stops, so the previous attack's
// BPE_AttackEnded can arrive AFTER the new start; acting on that event would move the weapon back to the right hand ~0.2 s into every mirrored follow-up.
// Rule: back only when no attack plays (UAttackComponent::BPF_IsPlayingAttack) on this tick AND the previous one (`quiet` = consecutive not-playing ticks,
// >= 2 = at least 0.1 s); playing < 0 (the call failed) falls back to the event rule (`ended` = an attack end was seen after the last start, 80 ms ago). Returns 1 = attach to the right hand now.
static int wp_hand_back(int left, int playing, int quiet, int ended) {
    if (!left) return 0;
    if (playing > 0) return 0;
    if (playing == 0) return quiet >= 2;
    return ended;
}

// the item fields a kind owns (deprecated on an item; ignored on an item that names a kind)
static const char *const WP_ITEM_OWNED[] = { "deck", "anim_set", "anim_style", "deflect_style", "style", "moves_exclusive", NULL };
#endif

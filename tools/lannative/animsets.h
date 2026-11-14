// animsets.h - pure logic of the "anim_sets" kind: the path table of every style-indexed animation table, the index allocator,
// the padding (copy of a base-style block, deep copy of nested TArrays) and the boot validator. No OS / engine calls, so it can be tested offline.
//
// Why this exists: the engine reads 66 style-indexed TArrays (14 properties on 8 DataAssets) with a style index. All reads check `style < ArrayNum`
// EXCEPT UHitComponent::GetHittedHandPose (HittedDB.m_HandPoses), so an anim set whose index is past a short table = out-of-bounds read in combat code.
// Rule: an index is handed out (anim_style_set) only after every array of every table holds it, checked by as_validate; otherwise the set is refused.
//
// Memory rules:
//  * grown arrays get a NEW buffer from `alloc` (FMemory::Malloc in game); the old buffer is NEVER freed: FidgetDB caches a pointer into it
//    (UFidgetDB::m_ShuffleableAnimArray) and readers on other threads may still hold it. Leaks < 200 KB once per boot.
//  * nested TArrays of a copied block (FAnimContainer.m_SpeedPerFrame, FidgetAnimArray) are deep-copied, never aliased.
//  * publish order data -> max -> num; new indices are used only after as_apply returns, so a reader never sees an index past its buffer.
#ifndef ANIMSETS_H
#define ANIMSETS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

typedef struct { void *data; int32_t num, max; } AsArr;          // UE4 TArray header (16 bytes)

enum { AS_LEAF_AC = 1,      // FAnimContainer 0x20: +0 UAnimSequence*, +8 bool mirror, +0x10 TArray<float> m_SpeedPerFrame (deep copy)
       AS_LEAF_PTR,         // UAnimSequence* (8)
       AS_LEAF_BYTE,        // EHandPoses (1)
       AS_LEAF_PTRARR };    // TArray<UAnimationAsset*> (0x10, deep copy)

#define AS_STOCK      6     // stock styles 0..5: every table ships with 6 entries
#define AS_IDX_MAX    63    // same cap as animstyle.h as_set; 0xFF is the engine's "no style" sentinel
#define AS_MAX_ARR    36    // GuardDB.m_GuardPrepAnimations
#define AS_MAX_SETS   16

typedef struct {
    const char *asset;      // object path for find_object / load_object
    const char *prop;       // property name (reflection check: offset + size)
    uint32_t off;           // property offset in the object (SDK dump)
    uint8_t  narr;          // style TArrays under the property (product of the fixed outer dims), contiguous at 0x10 stride
    uint32_t elem;          // bytes per style element
    uint8_t  leaf;          // AS_LEAF_*
    uint8_t  ndi;           // dims inside one style element (0 = the element is the leaf)
    uint16_t din[4];        // their sizes
    uint32_t sin[4];        // their byte strides
    uint8_t  checked;       // the reader checks style < ArrayNum
    int8_t   gate;          // unchecked table only: index (in AS_TABLES) of the table whose non-null result gates the read; -1 none
    const char *dims;       // full dimension order, style marked
    const char *reader;     // getter (RVA) <- caller that passes the style
} AsTable;

// Offsets / sizes: SDK dump; strides: the getters' index arithmetic. Order = pad order.
// the unchecked table (HandPoses) is padded before its gate (m_Anims) so there is never a moment where the gate is longer.
static const AsTable AS_TABLES[] = {
    { "/Game/DB/Hit/HittedDB.HittedDB", "m_HandPoses", 0x50, 1, 0x90, AS_LEAF_BYTE, 4, {3, 4, 4, 3}, {0x30, 0xC, 3, 1}, 0, 1,
      "[style][family 3][quadrant 4][cardinal 4][height 3] EHandPoses", "UHitComponent::GetHittedHandPose 0x404140 (NO bounds check) <- OrderHitted::OnStart 0x46B850 (only after m_Anims gave a clip, not guarding)" },
    { "/Game/DB/Hit/HittedDB.HittedDB", "m_Anims", 0x30, 1, 0x1200, AS_LEAF_AC, 4, {4, 4, 3, 3}, {0x480, 0x120, 0x60, 0x20}, 1, -1,
      "[style][quadrant 4][cardinal 4][height 3][family 3] FAnimContainer", "UHittedDB::GetAnim 0x402F80 <- UHitComponent::GetHittedAnim 0x404100 <- OrderHitted::OnStart" },
    { "/Game/DB/Hit/HittedDB.HittedDB", "m_AnimsInGuard", 0x40, 1, 0x1200, AS_LEAF_AC, 4, {4, 4, 3, 3}, {0x480, 0x120, 0x60, 0x20}, 1, -1,
      "[style][quadrant 4][cardinal 4][height 3][family 3] FAnimContainer", "UHittedDB::GetAnim 0x402F80 (guarding) <- OrderHitted::OnStart" },
    { "/Game/DB/Movement/ChangeQuadrant/ChangeQuadrantDB.ChangeQuadrantDB", "m_ChangeQuadrantAnims", 0x30, 1, 0x600, AS_LEAF_AC, 3, {3, 4, 4}, {0x200, 0x80, 0x20}, 1, -1,
      "[style][family 3][start quadrant 4][end quadrant 4] FAnimContainer", "UChangeQuadrantDB::GetAnimation 0x438650 <- GetChangeQuadrantAnimation 0x438BC0 <- OrderChangeQuadrant::OnStart, LaunchChangeQuadrant" },
    { "/Game/DB/Cancel/CancelDB.CancelDB", "m_AnimContainerArray", 0x48, 1, 0x180, AS_LEAF_AC, 2, {3, 4}, {0x80, 0x20}, 1, -1,
      "[style][family 3][quadrant 4] FAnimContainer", "UCancelDB::GetAnim 0x32FFE0 <- UAttackComponent::GetCancelAnimation 0x2F71E0 <- OrderCancel::OnStart, LaunchCancelAttack" },
    { "/Game/DB/Movement/BaseMovementDB.BaseMovementDB", "m_MoveTransitionAnimStyle", 0x30, 1, 0x1C80, AS_LEAF_AC, 3, {3, 2, 38}, {0x980, 0x4C0, 0x20}, 1, -1,
      "[style][family 3][sex 2][transition type 38] FAnimContainer", "UBaseMovementDB::GetTransitionAnimation 0x43B8B0 <- UpdateMoveStatusAnimation, NewWantedMoveTransition" },
    { "/Game/DB/Movement/BaseMovementDB.BaseMovementDB", "m_FreeMoveAnimStyle", 0x188, 1, 0x300, AS_LEAF_AC, 3, {4, 3, 2}, {0xC0, 0x40, 0x20}, 1, -1,
      "[style][speed V0-V3 4][family 3][sex 2] FAnimContainer", "UBaseMovementDB::GetFreeMoveAnimation 0x439290 <- ReplaceMovementAnimation 0x448C60" },
    { "/Game/DB/Movement/BaseMovementDB.BaseMovementDB", "m_LockMoveAnimStyle", 0x1C0, 1, 0x480, AS_LEAF_AC, 3, {3, 2, 4}, {0x180, 0xC0, 0x20}, 1, -1,
      "[style][family 3][sex 2][quadrant idle 4] FAnimContainer (+4 POD FBlendSpaceContainer at +0x80 of each 0xC0 block)", "UFightingMovementComponent::ReplaceMovementAnimation 0x448C60 <- SwitchMovementAnimations, Disarm, InitStyle, ServerUpdateMovementAnimation" },
    { "/Game/DB/Weapons/WeaponActionDB.WeaponActionDB", "m_LockMoveAnims", 0x30, 4, 0x180, AS_LEAF_AC, 2, {3, 4}, {0x80, 0x20}, 1, -1,
      "[action Draw,Sheathe,PickUp,Drop 4 fixed][style][family 3][quadrant 4] FAnimContainer", "UAttackComponent::GetDrawSheatheAnim 0x2F7AA0 <- OrderWeaponAction (real style), PickWeapon / AbortPickWeaponAttempt (anim index)" },
    { "/Game/DB/Movement/Dodge/DodgeDB.DodgeDB", "m_Anims", 0x30, 4, 0x600, AS_LEAF_AC, 3, {4, 4, 3}, {0x180, 0x60, 0x20}, 1, -1,
      "[weight 4 fixed][style][quadrant 4][cardinal 4][family 3] FAnimContainer", "UDodgeDB::GetAnim 0x438590 <- GetDodgeAnim 0x438F70 <- OrderDodge::OnStart" },
    { "/Game/DB/Movement/Dodge/DodgeDB.DodgeDB", "m_HandPoses", 0xC0, 1, 3, AS_LEAF_BYTE, 1, {3}, {1}, 1, -1,
      "[style][family 3] EHandPoses", "UFightingMovementComponent::GetDodgeHandPose 0x439190 (checked: 5 = no pose)" },
    { "/Game/DB/Fidget/FidgetDB.FidgetDB", "m_Anims", 0x38, 1, 0xC0, AS_LEAF_PTRARR, 2, {3, 4}, {0x40, 0x10}, 1, -1,
      "[style][family 3][quadrant 4] TArray<UAnimationAsset*>", "UFidgetDB::BPF_GetAnim 0x2EA8D0 (caches a pointer into the buffer) <- UPlayerAnim::UpdateFidgets" },
    { "/Game/DB/Guard/GuardDB.GuardDB", "m_GuardAnimations", 0x30, 12, 8, AS_LEAF_PTR, 0, {0}, {0}, 1, -1,
      "[family 3][quadrant 4 fixed][style] UAnimSequence* (style innermost)", "UGuardDB::GetGuardAnim 0x331920 / UDefenseComponent::GetGuardAnim 0x331900 <- OrderGuard::InitGuardAnim (style from the guard payload)" },
    { "/Game/DB/Guard/GuardDB.GuardDB", "m_GuardPrepAnimations", 0xF0, 36, 8, AS_LEAF_PTR, 0, {0}, {0}, 1, -1,
      "[family 3][quadrant 4][height 3 fixed][style] UAnimSequence* (style innermost)", "UGuardDB::GetGuardPrepAnim 0x331A70 <- OrderGuard::OnUpdate, UpdateGuardPrep" },
};
#define AS_NTABLES ((int)(sizeof AS_TABLES / sizeof AS_TABLES[0]))

static AsArr *as_arr(const AsTable *t, void *obj, int a) { return (AsArr *)((uint8_t *)obj + t->off + 16u * (unsigned)a); }
static int as_leaf_size(int leaf) { return leaf == AS_LEAF_AC ? 0x20 : leaf == AS_LEAF_PTR ? 8 : leaf == AS_LEAF_BYTE ? 1 : 0x10; }
static int as_leaves(const AsTable *t) { int n = 1; for (int d = 0; d < t->ndi; d++) n *= t->din[d]; return n; }
// byte offset of leaf number k (row-major over din) inside one element
static uint32_t as_leaf_off(const AsTable *t, int k) { uint32_t o = 0; for (int d = t->ndi - 1; d >= 0; d--) { o += (uint32_t)(k % t->din[d]) * t->sin[d]; k /= t->din[d]; } return o; }
// the nested TArray of a leaf (AC: m_SpeedPerFrame, PTRARR: the leaf itself), NULL for POD leaves; esz = its element size
static AsArr *as_inner(const AsTable *t, uint8_t *leafp, int *esz) {
    if (t->leaf == AS_LEAF_AC) { *esz = 4; return (AsArr *)(leafp + 0x10); }
    if (t->leaf == AS_LEAF_PTRARR) { *esz = 8; return (AsArr *)leafp; }
    return NULL;
}
// reflection check at boot: the engine's property offset / size must match the table (a different game build refuses everything)
static int as_check_layout(const AsTable *t, int refl_off, int refl_size) { return refl_off == (int)t->off && refl_size == 16 * t->narr; }

// byte offset of one slot inside the style TArray buffer of array `a`: idx = one index per inner dim. -1 = bad index
static long as_slot_off(const AsTable *t, int style, const int *idx) {
    if (style < 0 || style > AS_IDX_MAX) return -1;
    long o = (long)style * t->elem;
    for (int d = 0; d < t->ndi; d++) { if (idx[d] < 0 || idx[d] >= t->din[d]) return -1; o += (long)idx[d] * t->sin[d]; }
    return o;
}

// ---------------------------------------------------------------- allocator
typedef struct {
    char id[64];
    int fixed;              // >= 0: a real selectable style that already has its DefenseComponent.m_FightingStyles entry (SixthStyle = 6); -1 = hidden, allocate
    int fallback;           // stock style 0..5 whose block pads the new index
    int index;              // out: the table index, -1 if refused
    int ok;                 // out
    char why[120];          // out: refusal reason
} AsSet;

// base = max(AS_STOCK, m_FightingStyles.Num): hidden sets start past every real style. Returns the number of accepted sets.
static int as_allocate(AsSet *s, int n, int base) {
    if (base < AS_STOCK) base = AS_STOCK;
    int used[AS_IDX_MAX + 1] = {0}, acc = 0;
    for (int i = 0; i < n; i++) { s[i].index = -1; s[i].ok = 0; s[i].why[0] = 0; }
    for (int pass = 0; pass < 2; pass++)              // fixed first, so auto never steals a real style's index
        for (int i = 0; i < n; i++) {
            AsSet *x = &s[i];
            if ((pass == 0) != (x->fixed >= 0)) continue;
            if (x->fallback < 0 || x->fallback >= AS_STOCK) { snprintf(x->why, sizeof x->why, "fallback_style %d: must be a stock style 0..5", x->fallback); continue; }
            if (x->fixed >= 0) {
                if (x->fixed < AS_STOCK || x->fixed >= base) { snprintf(x->why, sizeof x->why, "style %d: a fixed index must be a real added style %d..%d", x->fixed, AS_STOCK, base - 1); continue; }
                if (used[x->fixed]) { snprintf(x->why, sizeof x->why, "style %d is already taken by another anim set", x->fixed); continue; }
                x->index = x->fixed;
            } else {
                int k = base; while (k <= AS_IDX_MAX && used[k]) k++;
                if (k > AS_IDX_MAX) { snprintf(x->why, sizeof x->why, "no free index (max %d)", AS_IDX_MAX); continue; }
                x->index = k;
            }
            used[x->index] = 1; x->ok = 1; acc++;
        }
    return acc;
}
// table length needed by the accepted sets (0 = none) and the source block of every index in [AS_STOCK, need): the owner's fallback, gaps use style 0
static int as_need(const AsSet *s, int n, int *src) {
    int need = 0;
    for (int i = 0; i <= AS_IDX_MAX; i++) src[i] = i < AS_STOCK ? i : 0;
    for (int i = 0; i < n; i++) if (s[i].ok) { src[s[i].index] = s[i].fallback; if (s[i].index + 1 > need) need = s[i].index + 1; }
    return need;
}

// ---------------------------------------------------------------- validator
// shape of one array: num in [AS_STOCK, AS_IDX_MAX+1], max >= num, data set, nested TArrays of every existing block sane (catches a wrong offset /
// element size: garbage reads as num < 0, max < num or data NULL with num > 0). need > 0: also num >= need.
static int as_check_arr(const AsTable *t, AsArr *a, int need, char *err, int cap, const char *what) {
    if (a->num < AS_STOCK || a->num > AS_IDX_MAX + 1 || a->max < a->num || !a->data) { snprintf(err, cap, "%s: bad array header num %d max %d data %p", what, a->num, a->max, a->data); return 0; }
    if (need > 0 && a->num < need) { snprintf(err, cap, "%s: %d entries, the anim sets need %d", what, a->num, need); return 0; }
    int nl = as_leaves(t);
    for (int s = 0; s < a->num; s++) for (int k = 0; k < nl; k++) {
        int esz; AsArr *in = as_inner(t, (uint8_t *)a->data + (size_t)s * t->elem + as_leaf_off(t, k), &esz);
        if (in && (in->num < 0 || in->num > 4096 || in->max < in->num || (in->num > 0 && !in->data))) {
            snprintf(err, cap, "%s: block %d leaf %d nested array num %d max %d data %p (layout mismatch?)", what, s, k, in->num, in->max, in->data); return 0;
        }
    }
    return 1;
}
// every table (objs[i] = the object of AS_TABLES[i], NULL = not loaded). need > 0: every array holds index need-1 and the gate rule holds. Returns the error count; err = the first one.
static int as_validate(void *const *objs, int need, char *err, int cap) {
    int bad = 0; char e[200], what[160];
    if (cap > 0) err[0] = 0;
    for (int i = 0; i < AS_NTABLES; i++) {
        const AsTable *t = &AS_TABLES[i];
        if (!objs[i]) { snprintf(e, sizeof e, "%s: object not loaded", t->asset); if (!bad++ && cap > 0) snprintf(err, cap, "%s", e); continue; }
        for (int a = 0; a < t->narr; a++) {
            snprintf(what, sizeof what, "%s.%s[%d]", t->asset, t->prop, a);
            if (!as_check_arr(t, as_arr(t, objs[i], a), need, e, sizeof e, what)) { if (!bad++ && cap > 0) snprintf(err, cap, "%s", e); }
        }
        // gate rule: an unchecked read is safe only while its table is at least as long as the gate that lets the read happen (need 0 = the stock-shape
        // check before padding: as_apply repairs a pak-grown gate by padding the unchecked table, so the rule applies after padding)
        if (need > 0 && t->gate >= 0 && objs[t->gate]) {
            const AsTable *g = &AS_TABLES[t->gate];
            int n = as_arr(t, objs[i], 0)->num, gn = as_arr(g, objs[t->gate], 0)->num;
            if (n < gn) { snprintf(e, sizeof e, "%s: unchecked %s has %d entries, its gate %s has %d", t->asset, t->prop, n, g->prop, gn); if (!bad++ && cap > 0) snprintf(err, cap, "%s", e); }
        }
    }
    return bad;
}

// ---------------------------------------------------------------- padding
typedef void *(*AsAlloc)(size_t);
// grows one array to `need` entries, entry i (old num <= i < need) = deep copy of block src[i]. Returns blocks added, -1 on failure (array unchanged).
static int as_pad_arr(const AsTable *t, AsArr *a, int need, const int *src, AsAlloc alloc, char *err, int cap) {
    if (a->num >= need) return 0;
    for (int i = a->num; i < need; i++) if (src[i] < 0 || src[i] >= a->num) { snprintf(err, cap, "%s.%s: source block %d missing", t->asset, t->prop, src[i]); return -1; }
    uint8_t *nb = (uint8_t *)alloc((size_t)need * t->elem);
    if (!nb) { snprintf(err, cap, "%s.%s: out of memory (%d x %u)", t->asset, t->prop, need, t->elem); return -1; }
    memcpy(nb, a->data, (size_t)a->num * t->elem);           // existing blocks move as they are (their nested arrays have one owner: the new buffer)
    int nl = as_leaves(t);
    for (int i = a->num; i < need; i++) {
        uint8_t *dst = nb + (size_t)i * t->elem;
        memcpy(dst, (uint8_t *)a->data + (size_t)src[i] * t->elem, t->elem);
        for (int k = 0; k < nl; k++) {
            int esz; AsArr *in = as_inner(t, dst + as_leaf_off(t, k), &esz);
            if (!in) continue;
            if (in->num <= 0) { in->data = NULL; in->num = 0; in->max = 0; continue; }
            void *d = alloc((size_t)in->num * esz);
            if (!d) { snprintf(err, cap, "%s.%s: out of memory (nested)", t->asset, t->prop); return -1; }   // partial nested copies leak on this path (boot time only)
            memcpy(d, in->data, (size_t)in->num * esz); in->data = d; in->max = in->num;
        }
    }
    int added = need - a->num;
    *(void *volatile *)&a->data = nb;                         // publish: data, max, then num (old buffer stays valid: never freed)
    *(volatile int32_t *)&a->max = need;
    *(volatile int32_t *)&a->num = need;
    return added;
}

typedef void (*AsLog)(const char *line);
// the whole boot step: allocate, validate the stock shape, pad every array, validate the result. On any failure every set is refused (ok 0,
// index -1) so no override can point past a table; arrays already padded keep their extra (unused) blocks. Returns the accepted set count.
static int as_apply(void *const *objs, AsSet *s, int n, int base, AsAlloc alloc, AsLog log) {
    char err[240], line[320]; int src[AS_IDX_MAX + 1];
    int acc = as_allocate(s, n, base);
    for (int i = 0; i < n; i++) {
        if (s[i].ok) snprintf(line, sizeof line, "animsets: %s -> index %d (fallback style %d%s)", s[i].id, s[i].index, s[i].fallback, s[i].fixed >= 0 ? ", real style" : "");
        else snprintf(line, sizeof line, "animsets: %s REFUSED: %s", s[i].id, s[i].why);
        if (log) log(line);
    }
    int need = as_need(s, n, src);
    if (!acc || !need) return acc;
    const char *fail = NULL;
    if (as_validate(objs, 0, err, sizeof err)) fail = "stock tables";
    int grown = 0, blocks = 0;
    for (int i = 0; !fail && i < AS_NTABLES; i++)
        for (int a = 0; a < AS_TABLES[i].narr; a++) {
            const AsTable *t = &AS_TABLES[i];
            AsArr *arr = as_arr(t, objs[i], a);
            int target = need;                                  // an unchecked table also covers its gate when a pak made the gate longer
            if (t->gate >= 0) { int gn = as_arr(&AS_TABLES[t->gate], objs[t->gate], 0)->num; if (gn > target) target = gn; }
            if (arr->num > need && log) { snprintf(line, sizeof line, "animsets: %s.%s[%d] already has %d entries (a superset pak is installed): its blocks >= %d are reused", t->asset, t->prop, a, arr->num, AS_STOCK); log(line); }
            int r = as_pad_arr(t, arr, target, src, alloc, err, sizeof err);
            if (r < 0) { fail = "padding"; break; }
            if (r > 0) { grown++; blocks += r; }
        }
    if (!fail && as_validate(objs, need, err, sizeof err)) fail = "padded tables";
    if (fail) {
        snprintf(line, sizeof line, "animsets: %s invalid, ALL %d anim set(s) refused (vanilla animations stay): %s", fail, acc, err);
        if (log) log(line);
        for (int i = 0; i < n; i++) if (s[i].ok) { s[i].ok = 0; s[i].index = -1; snprintf(s[i].why, sizeof s[i].why, "%s invalid", fail); }
        return 0;
    }
    snprintf(line, sizeof line, "animsets: %d array(s) grown by %d block(s) to %d entries; %d anim set(s) ready", grown, blocks, need, acc);
    if (log) log(line);
    return acc;
}

// ---------------------------------------------------------------- slots of the "anim_sets" kind (clip wiring, animsets.inc)
// One JSON slot entry = a table ("HittedDB.m_Anims": asset short name + property), an array of the property (-1 = all: DodgeDB weights) and one index per
// inner dimension of a style element (-1 = all). Expanded at the set's index into (array, byte offset) pairs.
typedef struct { int t, a, ix[4]; } AsWire;
static int as_table_find(const char *name) {
    const char *dot = name ? strchr(name, '.') : NULL; if (!dot) return -1;
    for (int i = 0; i < AS_NTABLES; i++) {
        const char *base = strrchr(AS_TABLES[i].asset, '.'); size_t n = (size_t)(dot - name);   // "/Game/DB/Hit/HittedDB.HittedDB" -> "HittedDB"
        if (base && strlen(base + 1) == n && !strncmp(base + 1, name, n) && !strcmp(AS_TABLES[i].prop, dot + 1)) return i;
    }
    return -1;
}
static int as_wire_check(const AsWire *w, char *err, int cap) {
    if (w->t < 0 || w->t >= AS_NTABLES) { snprintf(err, cap, "unknown table"); return 0; }
    const AsTable *t = &AS_TABLES[w->t];
    if (w->a < -1 || w->a >= t->narr) { snprintf(err, cap, "%s: array %d outside 0..%d", t->prop, w->a, t->narr - 1); return 0; }
    for (int d = 0; d < t->ndi; d++) if (w->ix[d] < -1 || w->ix[d] >= t->din[d]) { snprintf(err, cap, "%s: index %d of dimension %d outside 0..%d", t->prop, w->ix[d], d, t->din[d] - 1); return 0; }
    if (t->leaf != AS_LEAF_AC && t->leaf != AS_LEAF_PTR) { snprintf(err, cap, "%s holds no clip pointers (hand poses / clip lists are not wired)", t->prop); return 0; }
    return 1;
}
// all slots of w at `style`; returns the count (arrs / offs filled up to cap), -1 if w is bad
static int as_wire_slots(const AsWire *w, int style, int *arrs, long *offs, int cap) {
    char e[120]; if (!as_wire_check(w, e, sizeof e)) return -1;
    const AsTable *t = &AS_TABLES[w->t]; int n = 0, a0 = w->a < 0 ? 0 : w->a, a1 = w->a < 0 ? t->narr - 1 : w->a, tot = 1;
    for (int d = 0; d < t->ndi; d++) if (w->ix[d] < 0) tot *= t->din[d];
    for (int a = a0; a <= a1; a++)
        for (int k = 0; k < tot; k++) {
            int ix[4] = {0}, r = k;
            for (int d = t->ndi - 1; d >= 0; d--) { if (w->ix[d] < 0) { ix[d] = r % t->din[d]; r /= t->din[d]; } else ix[d] = w->ix[d]; }
            long o = as_slot_off(t, style, ix); if (o < 0) return -1;
            if (n < cap) { arrs[n] = a; offs[n] = o; } n++;
        }
    return n;
}
#endif

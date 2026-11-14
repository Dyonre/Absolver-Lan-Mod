#ifndef LANNATIVE_DECKLAYOUT_H
#define LANNATIVE_DECKLAYOUT_H
// Item field deck.layout (weaponslot.inc): a stance-valid deck, per STANCE row (the row a character in that stance attacks from; same labels as the deck editor) up to 3 chained row ids plus 1 alternate per row.
//   "layout": { "FrontRight": ["390"], "FrontLeft": ["389","370","367"], "BackRight": ["372","373m","377"], "BackLeft": ["384","379m","369m"],
//               "alts": { "FrontRight": "375m", "FrontLeft": "368", "BackRight": "371", "BackLeft": "374m" } }
// Row ids are attack row NAMES, numeric ("390") or named ("Cleaver_THDS_FR", "BladeMod_X"): 1..DL_ID_MAX (48) characters of A-Z a-z 0-9 _ . - ; a longer or otherwise bad name is an error that is logged (never dropped silently).
// Mirror marker: a trailing ":m" works on ANY id ("Cleaver_THDS_FR:m", "375:m"); for an all-digit id the short form "375m" also works. A named row's own trailing 'm' ("Cleaver_Slam") is therefore never taken for the marker,
// and a lone "m" is refused as ambiguous. Mirror semantics: an id with the marker is placed mirrored (its native start stance is the mirror of the stance it is played from: FR<->FL, BR<->BL),
// which is the bMirror argument of UAttackComponent::BPF_SetComboAttack. The live component's row array for stance S is EQuadrantTypes S (no mirroring there: the row
// of stance 0 = FL starts from a player in stance 0); the left/right mirror of the save's combat-deck row fields is only how the save fields / deck editor label the rows.
// Pure code (no OS / engine calls).
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "rowsjson.h"

#define DL_ROWS 4
#define DL_PER_ROW 3
#define DL_ID_MAX 48                                          // longest row name (ids in item "moves", weapon-kind "moves", deck "seed" and layouts); the buffers are DL_ID_BUF
#define DL_ID_BUF (DL_ID_MAX + 1)
typedef struct { int n[DL_ROWS]; char mv[DL_ROWS][DL_PER_ROW][DL_ID_BUF]; unsigned char mir[DL_ROWS][DL_PER_ROW]; char alt[DL_ROWS][DL_ID_BUF]; unsigned char altmir[DL_ROWS]; int total; } DlLayout;
static const char *const DL_ROW_NAME[DL_ROWS] = { "FrontRight", "FrontLeft", "BackRight", "BackLeft" };      // stance rows
static int dl_api_quadrant(int row) { static const int q[DL_ROWS] = { 1, 0, 2, 3 }; return row >= 0 && row < DL_ROWS ? q[row] : -1; }   // EQuadrantTypes FL0 FR1 BR2 BL3: the live deck row of stance S is array S
// a valid row name: 1..DL_ID_MAX characters of [A-Za-z0-9_.-]. 0 = bad, why (when given) says what is wrong (the callers log it).
static int dl_name_check(const char *s, char *why, int cap) {
    size_t n = s ? strlen(s) : 0;
    if (why && cap) why[0] = 0;
    if (n < 1) { if (why && cap) snprintf(why, (size_t)cap, "empty row name"); return 0; }
    if (n > DL_ID_MAX) { if (why && cap) snprintf(why, (size_t)cap, "row name too long (%d characters, the limit is %d)", (int)n, DL_ID_MAX); return 0; }
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; if (!(isalnum(c) || c == '_' || c == '.' || c == '-')) { if (why && cap) snprintf(why, (size_t)cap, "row name has a character that is not A-Z a-z 0-9 _ . - (at %d)", (int)i + 1); return 0; } }
    return 1;
}
// "375m" / "375:m" / "Cleaver_X:m" -> id without the marker, mirrored; "Cleaver_Slam" stays a plain name. 0 = bad (why says what).
static int dl_id(const char *s, char *id, unsigned char *mir, char *why, int cap) {
    size_t n = s ? strlen(s) : 0; *mir = 0; if (why && cap) why[0] = 0;
    if (n >= 3 && s[n - 2] == ':' && (s[n - 1] == 'm' || s[n - 1] == 'M')) { *mir = 1; n -= 2; }
    else if (n >= 2 && (s[n - 1] == 'm' || s[n - 1] == 'M')) { size_t i = 0; while (i < n - 1 && isdigit((unsigned char)s[i])) i++; if (i == n - 1) { *mir = 1; n--; } }   // digits + m (short form)
    else if (n == 1 && (s[0] == 'm' || s[0] == 'M')) { if (why && cap) snprintf(why, (size_t)cap, "a lone 'm' is ambiguous (mirror marker without a row)"); return 0; }
    if (n > DL_ID_MAX) { if (why && cap) snprintf(why, (size_t)cap, "row name too long (%d characters, the limit is %d)", (int)n, DL_ID_MAX); return 0; }
    char tmp[DL_ID_BUF]; memcpy(tmp, s, n); tmp[n] = 0; if (!dl_name_check(tmp, why, cap)) return 0;
    memcpy(id, tmp, n + 1); return 1;
}
static int dl_dup(const DlLayout *L, const char *id) {
    for (int r = 0; r < DL_ROWS; r++) { for (int k = 0; k < L->n[r]; k++) if (!strcmp(L->mv[r][k], id)) return 1; if (L->alt[r][0] && !strcmp(L->alt[r], id)) return 1; }
    return 0;
}
// obj = the "layout" object node of d. 0 = ok (L filled), -1 = error (err says what). Unknown keys, >3 moves per row, duplicate ids (a move is never repeated inside a deck), a bad id, and a layout without a single move are errors.
static int dl_parse(const JDoc *d, int obj, DlLayout *L, char *err, int cap) {
    memset(L, 0, sizeof *L); if (err && cap) err[0] = 0;
#define DL_FAIL(...) do { if (err && cap) snprintf(err, (size_t)cap, __VA_ARGS__); return -1; } while (0)
    if (obj < 0 || d->n[obj].t != J_OBJ) DL_FAIL("layout must be an object");
    for (int c = d->n[obj].first; c >= 0; c = d->n[c].next) {
        const char *k = d->n[c].key ? d->n[c].key : ""; int row = -1; for (int r = 0; r < DL_ROWS; r++) if (!strcmp(k, DL_ROW_NAME[r])) row = r;
        if (row >= 0) {
            if (d->n[c].t != J_ARR) DL_FAIL("layout.%s must be an array of row ids", k);
            for (int e = d->n[c].first; e >= 0; e = d->n[e].next) {
                char id[DL_ID_BUF], why[100]; unsigned char m; if (d->n[e].t != J_STR || !dl_id(d->n[e].str, id, &m, why, sizeof why)) DL_FAIL("layout.%s: bad row id '%.60s': %s (a row name; optional mirror marker ':m')", k, d->n[e].str ? d->n[e].str : "?", d->n[e].t != J_STR ? "not a string" : why);
                if (L->n[row] >= DL_PER_ROW) DL_FAIL("layout.%s: more than %d moves", k, DL_PER_ROW);
                if (dl_dup(L, id)) DL_FAIL("layout: row %s used twice", id);
                int i = L->n[row]++; snprintf(L->mv[row][i], DL_ID_BUF, "%s", id); L->mir[row][i] = m; L->total++;
            }
        } else if (!strcmp(k, "alts")) {
            if (d->n[c].t != J_OBJ) DL_FAIL("layout.alts must be an object");
            for (int e = d->n[c].first; e >= 0; e = d->n[e].next) {
                const char *ak = d->n[e].key ? d->n[e].key : ""; int ar = -1; for (int r = 0; r < DL_ROWS; r++) if (!strcmp(ak, DL_ROW_NAME[r])) ar = r;
                if (ar < 0) DL_FAIL("layout.alts: unknown row '%s'", ak);
                char id[DL_ID_BUF], why[100]; unsigned char m; if (d->n[e].t != J_STR || !dl_id(d->n[e].str, id, &m, why, sizeof why)) DL_FAIL("layout.alts.%s: bad row id '%.60s': %s", ak, d->n[e].str ? d->n[e].str : "?", d->n[e].t != J_STR ? "not a string" : why);
                if (L->alt[ar][0]) DL_FAIL("layout.alts.%s: twice", ak);
                if (dl_dup(L, id)) DL_FAIL("layout: row %s used twice", id);
                snprintf(L->alt[ar], DL_ID_BUF, "%s", id); L->altmir[ar] = m;
            }
        } else DL_FAIL("layout: unknown key '%s'", k);
    }
    if (!L->total) DL_FAIL("layout holds no move");
#undef DL_FAIL
    return 0;
}
// ----- the item's deck range (item field "deck", weaponslot.inc): which named decks of the family list belong to the item and what seeds them.
//   new:    "deck": { "first_slot": 5, "count": 4, "label": "Staff", "layout": {...}, "layouts": [null, {...}, {...}, {...}] }  -> decks "5".."8" (list entries 5-8)
//   legacy: "deck": { "slot": "4", "layout": {...} }                                                                            -> one deck named "4"
//   neither slot nor first_slot: first_slot 5, count 1. "layouts"[k] seeds deck k of the range; null / missing entries use "layout" (or "seed" when there is no layout).
#define DL_MAX_DECKS 8
typedef struct { int first, count, legacy; char names[DL_MAX_DECKS][16]; char label[64]; int has[DL_MAX_DECKS]; DlLayout lay[DL_MAX_DECKS]; } DlRange;   // has[k]: 1 own layout, 2 = "layout" fallback, 0 none
static int dl_int(const JDoc *d, int node, int *out) {                // J_NUM or a J_STR of digits
    if (node < 0 || (d->n[node].t != J_NUM && d->n[node].t != J_STR) || !d->n[node].str || !d->n[node].str[0]) return 0;
    for (const char *p = d->n[node].str; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    if (strlen(d->n[node].str) > 3) return 0; *out = atoi(d->n[node].str); return 1;
}
static int dl_parse_range(const JDoc *d, int deck, DlRange *R, char *err, int cap) {
    memset(R, 0, sizeof *R); if (err && cap) err[0] = 0; char le[160];
#define DR_FAIL(...) do { if (err && cap) snprintf(err, (size_t)cap, __VA_ARGS__); return -1; } while (0)
    if (deck < 0 || d->n[deck].t != J_OBJ) DR_FAIL("deck must be an object");
    int sl = jfind(d, deck, "slot"), fs = jfind(d, deck, "first_slot"), ct = jfind(d, deck, "count"), lb = jfind(d, deck, "label");
    if (sl >= 0 && (fs >= 0 || ct >= 0)) DR_FAIL("deck: use either \"slot\" (one deck) or \"first_slot\" + \"count\", not both");
    if (sl >= 0) {
        const char *s = (d->n[sl].t == J_STR || d->n[sl].t == J_NUM) ? d->n[sl].str : NULL;
        if (!s || !s[0] || strlen(s) >= 16) DR_FAIL("deck.slot must be a deck name like \"4\"");
        R->legacy = 1; R->count = 1; snprintf(R->names[0], 16, "%s", s); if (!dl_int(d, sl, &R->first)) R->first = 0;
    } else {
        R->first = 5; R->count = 1;
        if (fs >= 0 && !dl_int(d, fs, &R->first)) DR_FAIL("deck.first_slot must be a number");
        if (ct >= 0 && !dl_int(d, ct, &R->count)) DR_FAIL("deck.count must be a number");
        if (R->first < 2 || R->first > 99) DR_FAIL("deck.first_slot %d out of range (2..99; deck 1 is the family's default deck)", R->first);
        if (R->count < 1 || R->count > DL_MAX_DECKS) DR_FAIL("deck.count %d out of range (1..%d)", R->count, DL_MAX_DECKS);
        if (R->first + R->count - 1 > 99) DR_FAIL("deck range %d..%d ends past 99", R->first, R->first + R->count - 1);
        for (int k = 0; k < R->count; k++) snprintf(R->names[k], 16, "%d", R->first + k);
    }
    if (lb >= 0 && (d->n[lb].t != J_STR || !d->n[lb].str)) DR_FAIL("deck.label must be a string");
    snprintf(R->label, sizeof R->label, "%s", lb >= 0 && d->n[lb].str[0] ? d->n[lb].str : "Staff");
    int ly = jfind(d, deck, "layout"), lys = jfind(d, deck, "layouts"); DlLayout base; int has_base = 0; memset(&base, 0, sizeof base);
    if (ly >= 0) { if (dl_parse(d, ly, &base, le, sizeof le)) DR_FAIL("deck.layout: %s", le); has_base = 1; }
    if (lys >= 0) {
        if (d->n[lys].t != J_ARR) DR_FAIL("deck.layouts must be an array (entry k seeds deck k of the range, null = \"layout\")");
        int k = 0; for (int e = d->n[lys].first; e >= 0; e = d->n[e].next, k++) {
            if (k >= R->count) DR_FAIL("deck.layouts has more entries than the range has decks (%d)", R->count);
            if (d->n[e].t == J_NULL) continue;
            if (dl_parse(d, e, &R->lay[k], le, sizeof le)) DR_FAIL("deck.layouts[%d]: %s", k, le);
            R->has[k] = 1;
        }
    }
    for (int k = 0; k < R->count; k++) if (!R->has[k] && has_base) { R->lay[k] = base; R->has[k] = 2; }
#undef DR_FAIL
    return 0;
}
// index of a deck name in the range, -1 = not one of the item's decks
static int dl_range_find(const DlRange *R, const char *name) { for (int k = 0; name && k < R->count; k++) if (!strcmp(R->names[k], name)) return k; return -1; }

// stance rows without a move cannot start a chain from that stance: names them (for the log). Returns their count.
static int dl_empty_rows(const DlLayout *L, char *out, int cap) {
    int e = 0; if (cap) out[0] = 0;
    for (int r = 0; r < DL_ROWS; r++) if (!L->n[r]) { e++; size_t u = strlen(out); snprintf(out + u, (size_t)cap - u, "%s%s", u ? "," : "", DL_ROW_NAME[r]); }
    return e;
}
#endif

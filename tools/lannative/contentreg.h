#ifndef LANNATIVE_CONTENTREG_H
#define LANNATIVE_CONTENTREG_H
// Content registry, pure part: entry extraction from a content file, conflict resolution, compact JSON writer.
// Files live in Mods\<mod>\content\<kind>\*.json. A file is one entry object (has "id"), an array of entries, or { "<kind>": [ entries ] }.
#include <string.h>
#include <stdio.h>
#include "rowsjson.h"

#define CR_ID_MAX 64
#define CR_KIND_MAX 32
typedef struct { char kind[CR_KIND_MAX], id[CR_ID_MAX]; int pos; int file; int shadowed; int winner; } CrEntry;      // pos = the mod's load position (higher = later = wins)

static int cr_kind_ok(const char *k) { size_t n = strlen(k); if (!n || n >= CR_KIND_MAX) return 0; for (; *k; k++) if (!((*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_')) return 0; return 1; }
static int cr_id_ok(const char *s) { size_t n = strlen(s); if (!n || n >= CR_ID_MAX) return 0; for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_')) return 0; return 1; }

// entries in load order (earlier mods first, files in name order). For each (kind, id) only the LAST entry stays; the others get shadowed = 1 and winner = index of the winner.
// Returns the number of shadowed entries.
static int cr_resolve(CrEntry *e, int n) {
    int shadowed = 0;
    for (int i = 0; i < n; i++) { e[i].shadowed = 0; e[i].winner = i; }
    for (int i = 0; i < n; i++) for (int j = n - 1; j > i; j--)
        if (!_stricmp(e[i].kind, e[j].kind) && !strcmp(e[i].id, e[j].id)) { e[i].shadowed = 1; e[i].winner = j; shadowed++; break; }
    return shadowed;
}

// entry nodes of one parsed content file: appends node indices to out[], returns the count (the caller checks each entry's "id")
static int cr_file_entries(const JDoc *d, const char *kind, int *out, int max) {
    int n = 0, root = 0; if (d->count < 1) return 0;
    if (d->n[root].t == J_OBJ) {
        int c = jfind(d, root, kind);
        if (c >= 0 && d->n[c].t == J_ARR) root = c; else { if (n < max) out[n++] = root; return n; }
    }
    if (d->n[root].t == J_ARR) for (int e = d->n[root].first; e >= 0 && n < max; e = d->n[e].next) if (d->n[e].t == J_OBJ) out[n++] = e;
    return n;
}

// compact JSON text of one node (for handlers registered by plugins); returns the length or -1 if cap is too small
static int jdoc_write(const JDoc *d, int node, char *out, int cap) {
    const JNode *nd = &d->n[node]; int w = 0;
#define PUT(c) do { if (w + 1 >= cap) return -1; out[w++] = (c); } while (0)
    if (nd->t == J_OBJ || nd->t == J_ARR) {
        PUT(nd->t == J_OBJ ? '{' : '[');
        for (int c = nd->first; c >= 0; c = d->n[c].next) {
            if (c != nd->first) PUT(',');
            if (nd->t == J_OBJ) { PUT('"'); for (const char *k = d->n[c].key; k && *k; k++) { if (*k == '"' || *k == '\\') PUT('\\'); PUT(*k); } PUT('"'); PUT(':'); }
            int r = jdoc_write(d, c, out + w, cap - w); if (r < 0) return -1; w += r;
        }
        PUT(nd->t == J_OBJ ? '}' : ']');
    } else if (nd->t == J_STR) {
        PUT('"'); for (const char *s = nd->str; s && *s; s++) { unsigned char ch = (unsigned char)*s; if (ch == '"' || ch == '\\') { PUT('\\'); PUT((char)ch); } else if (ch == '\n') { PUT('\\'); PUT('n'); } else if (ch == '\r') { PUT('\\'); PUT('r'); } else if (ch == '\t') { PUT('\\'); PUT('t'); } else if (ch < 0x20) { PUT(' '); } else PUT((char)ch); } PUT('"');
    } else if (nd->t == J_BOOL) { const char *s = nd->str && !strcmp(nd->str, "True") ? "true" : "false"; for (; *s; s++) PUT(*s); }
    else if (nd->t == J_NUM) { for (const char *s = nd->str; s && *s; s++) PUT(*s); }
    else { const char *s = "null"; for (; *s; s++) PUT(*s); }
#undef PUT
    out[w] = 0; return w;
}
// ----- hot reload: how an entry of a fresh scan relates to the scan before it. `old` holds the entries that were valid and not shadowed last time (json = the compact text that was applied).
typedef struct { char kind[CR_KIND_MAX], id[CR_ID_MAX], mod[96]; const char *json; int applied; } CrSnap;
enum { CRR_SAME = 0, CRR_CHANGED, CRR_NEW, CRR_RETRY };       // identical and applied / applied before but different / not there before / was there but never applied (retry)
static int cr_snap_find(const CrSnap *list, int n, const CrSnap *e) {
    for (int i = 0; i < n; i++) if (!_stricmp(list[i].kind, e->kind) && !strcmp(list[i].id, e->id) && !strcmp(list[i].mod, e->mod)) return i;
    return -1;
}
static int cr_reload_class(const CrSnap *old, int nold, const CrSnap *e) {
    int i = cr_snap_find(old, nold, e); if (i < 0) return CRR_NEW;
    if (!old[i].applied) return CRR_RETRY;
    return old[i].json && e->json && !strcmp(old[i].json, e->json) ? CRR_SAME : CRR_CHANGED;
}
// indices of the old entries that were applied and are not in the new scan any more (they stay in the game until a restart)
static int cr_reload_removed(const CrSnap *old, int nold, const CrSnap *neu, int nnew, int *out, int max) {
    int n = 0; for (int i = 0; i < nold && n < max; i++) if (old[i].applied && cr_snap_find(neu, nnew, &old[i]) < 0) out[n++] = i;
    return n;
}
#endif

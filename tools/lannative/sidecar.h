// sidecar.h - pure logic of the per-character metadata store. No engine calls.
// One text file per character: Binaries\Win64\LanNative\state\<GUID>.txt, lines "key=value" (CR LF, '#' = comment), keyed by the save's
// m_SavedData.m_FightingCharacterGuid, so renaming the character keeps its state. METADATA only (which kind deck was used last, ...): decks, moves and
// everything the game owns stay in the game's save. Keys: "kind.<item id>.<what>" (kindstate.h), "family.<n>.deck", "p.<plugin>.<key>" (API
// sidecar_get / sidecar_set), "legacy.<what>" (migrated from the name-keyed kv store), "name" (last character name seen, for humans), "migrated".
#ifndef SIDECAR_H
#define SIDECAR_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { char *k, *v; } ScKv;
typedef struct { ScKv *a; int n, cap; } ScDoc;                       // grows as needed (no fixed cap)

static void sc_free(ScDoc *d) { for (int i = 0; i < d->n; i++) { free(d->a[i].k); free(d->a[i].v); } free(d->a); memset(d, 0, sizeof *d); }
static int sc_key_ok(const char *k) { return k && *k && !strpbrk(k, "=\r\n") && k[0] != '#'; }
static int sc_find(const ScDoc *d, const char *k) { for (int i = 0; i < d->n; i++) if (!strcmp(d->a[i].k, k)) return i; return -1; }
static const char *sc_get(const ScDoc *d, const char *k) { int i = k ? sc_find(d, k) : -1; return i >= 0 ? d->a[i].v : NULL; }
static char *sc_dup(const char *s) { size_t n = strlen(s) + 1; char *r = (char *)malloc(n); if (r) { memcpy(r, s, n); for (char *c = r; *c; c++) if (*c == '\r' || *c == '\n') *c = ' '; } return r; }
// value NULL removes. Returns 1 = changed, 0 = already so, -1 = bad key / out of memory
static int sc_set(ScDoc *d, const char *k, const char *v) {
    if (!sc_key_ok(k)) return -1; int i = sc_find(d, k);
    if (!v) { if (i < 0) return 0; free(d->a[i].k); free(d->a[i].v); d->a[i] = d->a[--d->n]; return 1; }
    if (i >= 0) { if (!strcmp(d->a[i].v, v)) return 0; char *nv = sc_dup(v); if (!nv) return -1; free(d->a[i].v); d->a[i].v = nv; return 1; }
    if (d->n == d->cap) { int nc = d->cap ? d->cap * 2 : 16; ScKv *na = (ScKv *)realloc(d->a, (size_t)nc * sizeof *na); if (!na) return -1; d->a = na; d->cap = nc; }
    char *nk = sc_dup(k), *nv = sc_dup(v); if (!nk || !nv) { free(nk); free(nv); return -1; }
    d->a[d->n].k = nk; d->a[d->n].v = nv; d->n++; return 1;
}
// parse "key=value" lines (any length; later duplicates win). Returns the number of keys, -1 out of memory
static int sc_parse(ScDoc *d, const char *text) {
    sc_free(d); if (!text) return 0;
    for (const char *p = text; *p; ) {
        const char *e = p; while (*e && *e != '\n') e++; size_t L = (size_t)(e - p); while (L && (p[L - 1] == '\r' || p[L - 1] == '\n')) L--;
        char *line = (char *)malloc(L + 1); if (!line) return -1; memcpy(line, p, L); line[L] = 0;
        char *eq = strchr(line, '='); if (L && line[0] != '#' && eq && eq != line) { *eq = 0; if (sc_set(d, line, eq + 1) < 0) { free(line); return -1; } }
        free(line); p = *e ? e + 1 : e;
    }
    return d->n;
}
// the whole document as text (malloc'd, caller frees): a header comment, then the keys sorted (stable diffs, easy to read)
static int sc_cmp(const void *a, const void *b) { return strcmp(((const ScKv *)a)->k, ((const ScKv *)b)->k); }
static char *sc_render(ScDoc *d, const char *header) {
    qsort(d->a, (size_t)d->n, sizeof *d->a, sc_cmp);
    size_t n = (header ? strlen(header) : 0) + 8; for (int i = 0; i < d->n; i++) n += strlen(d->a[i].k) + strlen(d->a[i].v) + 4;
    char *out = (char *)malloc(n), *w = out; if (!out) return NULL; *w = 0;
    if (header) w += sprintf(w, "# %s\r\n", header);
    for (int i = 0; i < d->n; i++) w += sprintf(w, "%s=%s\r\n", d->a[i].k, d->a[i].v);
    return out;
}
// FGuid {A,B,C,D} as 32 upper-case hex digits (UE's FGuid::ToString default). Returns 0 for the all-zero guid (not assigned yet), else 1
static int sc_guid_text(const uint32_t g[4], char out[33]) { snprintf(out, 33, "%08X%08X%08X%08X", g[0], g[1], g[2], g[3]); return (g[0] | g[1] | g[2] | g[3]) != 0; }

// ----- migration of the name-keyed kv store (LanNative\data\Content.txt, the loader's own kv file): "<name>.staff_deck", "<name>.sword_deck",
// "<name>.style" -> "legacy.<what>" in this character's document (readers try the new per-kind key first, then the legacy one). Runs once per document: it sets
// "migrated" (to the name it came from, or "-" when nothing was found). `name` = the sanitised name the kv store uses (spaces, '=', CR, LF -> '_').
// Returns the number of keys taken; `removed` (may be NULL) receives the old keys to delete from the kv file, at most `max_removed`, each < 160 chars.
static const char *const SC_LEGACY[] = { "staff_deck", "sword_deck", "style", NULL };
static int sc_migrate(ScDoc *d, const char *old_text, const char *name, char (*removed)[160], int max_removed, int *nremoved) {
    if (nremoved) *nremoved = 0; if (sc_get(d, "migrated")) return 0;
    int took = 0; ScDoc old = { 0 }; if (old_text && name && *name) sc_parse(&old, old_text);
    for (int i = 0; SC_LEGACY[i] && name && *name; i++) {
        char k[160], nk[64]; snprintf(k, sizeof k, "%s.%s", name, SC_LEGACY[i]); const char *v = sc_get(&old, k); if (!v || !*v) continue;
        snprintf(nk, sizeof nk, "legacy.%s", SC_LEGACY[i]); if (!sc_get(d, nk) && sc_set(d, nk, v) > 0) took++;
        if (removed && nremoved && *nremoved < max_removed) snprintf(removed[(*nremoved)++], 160, "%s", k);
    }
    sc_free(&old); sc_set(d, "migrated", took ? name : "-");
    return took;
}
// the kv store's key form of a character name
static void sc_old_name(const char *in, char *out, int cap) { snprintf(out, (size_t)cap, "%s", in && *in ? in : "_"); for (char *c = out; *c; c++) if (*c == '=' || *c == '\r' || *c == '\n' || *c == ' ') *c = '_'; }
#endif

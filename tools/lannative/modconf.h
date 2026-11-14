#ifndef LANNATIVE_MODCONF_H
#define LANNATIVE_MODCONF_H
// Pure mod-loader logic (no Windows calls): load order file, conflict detection between mods, mod-set fingerprint, read orders.
// Load order convention: LAST line wins. Lines: "ModFolder", "-ModFolder" (disabled), "# comment".
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modpak.h"

#define MODS_MAX 256
#define MOD_NAME_MAX 96
typedef struct { char name[MOD_NAME_MAX]; int enabled; } ModEntry;
typedef struct { int count; ModEntry mods[MODS_MAX]; } ModList;

static int modlist_find(const ModList *l, const char *name) {
    for (int i = 0; i < l->count; i++) if (!_stricmp(l->mods[i].name, name)) return i;
    return -1;
}
// Parses loadorder.txt text. Duplicate names keep the later line; names with path characters are ignored.
static void modlist_parse(ModList *l, const char *text) {
    l->count = 0;
    const char *p = text;
    while (*p) {
        const char *e = strpbrk(p, "\r\n"); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[256]; if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n); line[n] = 0;
        p += e ? n + 1 : n; if (e && *p == '\n' && e[0] == '\r') p++;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        char *end = s + strlen(s); while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*s || *s == '#') continue;
        int enabled = 1; if (*s == '-') { enabled = 0; s++; while (*s == ' ') s++; }
        if (!*s || strlen(s) >= MOD_NAME_MAX || strpbrk(s, "\\/:*?\"<>|")) continue;
        int at = modlist_find(l, s);
        if (at >= 0) { memmove(&l->mods[at], &l->mods[at + 1], (size_t)(l->count - at - 1) * sizeof(ModEntry)); l->count--; }
        if (l->count < MODS_MAX) { strcpy(l->mods[l->count].name, s); l->mods[l->count].enabled = enabled; l->count++; }
    }
}
// Keeps the file's order, drops listed mods whose folder is gone, appends newly found folders (enabled) at the end = highest priority.
// Returns the number of folders appended.
static int modlist_sync(ModList *l, const char **found, int nfound) {
    int w = 0;
    for (int i = 0; i < l->count; i++) {
        int present = 0; for (int j = 0; j < nfound; j++) if (!_stricmp(found[j], l->mods[i].name)) present = 1;
        if (present) l->mods[w++] = l->mods[i];
    }
    l->count = w;
    int added = 0;
    for (int j = 0; j < nfound; j++)
        if (modlist_find(l, found[j]) < 0 && l->count < MODS_MAX && strlen(found[j]) < MOD_NAME_MAX) {
            strcpy(l->mods[l->count].name, found[j]); l->mods[l->count].enabled = 1; l->count++; added++;
        }
    return added;
}
static int modlist_render(const ModList *l, char *out, size_t cap) {
    int n = snprintf(out, cap, "# Absolver mod load order. Last line wins when two mods change the same file.\r\n"
                               "# Put a - in front of a name to disable that mod. New mod folders are added at the end automatically.\r\n");
    for (int i = 0; i < l->count && n >= 0 && (size_t)n < cap; i++) n += snprintf(out + n, cap - n, "%s%s\r\n", l->mods[i].enabled ? "" : "-", l->mods[i].name);
    return n;
}

// Read order the engine uses for a mod pak: far above every stock value (<= 4) and above the "_P" bonuses (+100 each patch level).
// position = index among ENABLED mods (0 = lowest priority); pak = index of the pak inside the mod (paks sorted by name, later wins).
static uint32_t mod_read_order(int position, int pak) { return 10000u + 1000u * (uint32_t)position + (uint32_t)(pak < 999 ? pak : 999); }

typedef struct { const PakEntry *entries; int count; int mod; } ModFiles;          // one pak's list, tagged with its mod's position
typedef struct { const PakEntry *e; int mod; } FileRef;
static int fileref_cmp(const void *a, const void *b) {
    const FileRef *x = a, *y = b; int c = _stricmp(x->e->path, y->e->path);
    return c ? c : (x->mod - y->mod);
}
typedef struct { char path[512]; int winner, loser; int same_content; } Conflict;
// Finds files shipped by two different mods. Reports (path, winner = highest mod position, one loser) for each extra mod, up to max.
// same_content = 1 when the file bytes are identical (SHA-1 match): harmless, reported separately.
// Different-content conflicts are stored FIRST (pass 0), identical ones after them (pass 1): a long list of identical files (hundreds of shared animations) must never push the
// dangerous ones past `max`. *ndiff (optional) = ALL different-content conflicts, stored or not.
static int mods_conflicts_ex(const ModFiles *sets, int nsets, Conflict *out, int max, int *ndiff) {
    size_t total = 0; for (int i = 0; i < nsets; i++) total += (size_t)sets[i].count;
    if (ndiff) *ndiff = 0;
    if (!total) return 0;
    FileRef *refs = malloc(total * sizeof *refs); if (!refs) return 0;
    size_t n = 0;
    for (int i = 0; i < nsets; i++) for (int j = 0; j < sets[i].count; j++) { refs[n].e = &sets[i].entries[j]; refs[n].mod = sets[i].mod; n++; }
    qsort(refs, n, sizeof *refs, fileref_cmp);
    int found = 0, stored = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n;) {
            size_t j = i + 1; while (j < n && !_stricmp(refs[j].e->path, refs[i].e->path)) j++;
            for (size_t k = i; k + 1 < j; k++) {
                if (refs[k].mod == refs[k + 1].mod) continue;       // same mod twice (two paks): not a mod-vs-mod conflict
                int same = !memcmp(refs[k].e->sha1, refs[k + 1].e->sha1, 20);
                if (pass == 0) { if (!same && ndiff) (*ndiff)++; }
                if (pass == (same ? 1 : 0)) {
                    if (stored < max) {
                        strcpy(out[stored].path, refs[k].e->path); out[stored].loser = refs[k].mod; out[stored].winner = refs[j - 1].mod; out[stored].same_content = same; stored++;
                    }
                    found++;
                }
            }
            i = j;
        }
    }
    free(refs); return found;
}
static int mods_conflicts(const ModFiles *sets, int nsets, Conflict *out, int max) { return mods_conflicts_ex(sets, nsets, out, max, NULL); }


// An UE asset is several files (X.uasset + X.uexp [+ X.ubulk, X.umap ...]). The engine serves each FILE from the highest mod that has it, so when two mods
// each ship part of the same asset the game gets a mix of two versions: "Serial size mismatch" and a crash at load. A split asset = served by more
// than one mod. These are the dangerous conflicts.
typedef struct { char base[512]; int mods[4]; int nmods; } SplitAsset;
static void asset_base(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path); char *dot = strrchr(out, '.'); char *sl = strrchr(out, '/');
    if (dot && (!sl || dot > sl)) *dot = 0;
}
static int mods_split_assets(const ModFiles *sets, int nsets, SplitAsset *out, int max) {
    size_t total = 0; for (int i = 0; i < nsets; i++) total += (size_t)sets[i].count;
    if (!total) return 0;
    FileRef *refs = malloc(total * sizeof *refs); if (!refs) return 0;
    size_t n = 0;
    for (int i = 0; i < nsets; i++) for (int j = 0; j < sets[i].count; j++) {
        const char *ext = strrchr(sets[i].entries[j].path, '.');
        if (ext && !_stricmp(ext, ".bak")) continue;           // editor backups are never loaded as assets (and would split the grouping)
        refs[n].e = &sets[i].entries[j]; refs[n].mod = sets[i].mod; n++;
    }
    qsort(refs, n, sizeof *refs, fileref_cmp);               // sorted by path, then mod ascending: the last of each path run is the serving mod
    int found = 0; char base[512], cur[512] = "";
    int serving[8]; int ns = 0; int contributors[8]; int nc = 0;
    for (size_t i = 0; i <= n; i++) {
        int boundary = (i == n);
        if (!boundary) { asset_base(refs[i].e->path, base, sizeof base); if (strcmp(base, cur) && cur[0]) boundary = 1; }
        if (boundary) {
            int distinct = 0; for (int a = 0; a < ns; a++) { int seen = 0; for (int b = 0; b < a; b++) if (serving[b] == serving[a]) seen = 1; if (!seen) distinct++; }
            if (distinct > 1) {                                // different mods serve different files of the same asset
                if (found < max) {
                    strcpy(out[found].base, cur); out[found].nmods = 0;
                    for (int a = 0; a < ns && out[found].nmods < 4; a++) { int seen = 0; for (int b = 0; b < out[found].nmods; b++) if (out[found].mods[b] == serving[a]) seen = 1; if (!seen) out[found].mods[out[found].nmods++] = serving[a]; }
                }
                found++;
            }
            ns = 0; nc = 0; if (i == n) break;
        }
        strcpy(cur, base);
        // last ref of this path run = highest mod
        if (i + 1 == n || _stricmp(refs[i + 1].e->path, refs[i].e->path)) {
            if (ns < 8) serving[ns++] = refs[i].mod;
        }
        (void)contributors; (void)nc;
    }
    free(refs); return found;
}

// mod.json (optional, in the mod's folder): {"affects_gameplay": false} marks a cosmetic-only mod (textures, skins). Such mods are left out of the LAN mod check.
// Anything else, a missing file, or an unreadable value counts as gameplay: the safe default.
static int mod_declares_cosmetic(const char *json) {
    if (!json) return 0;
    const char *k = strstr(json, "\"affects_gameplay\""); if (!k) return 0;
    k += strlen("\"affects_gameplay\""); while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (*k != ':') return 0; k++; while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    return _strnicmp(k, "false", 5) == 0;
}

// FNV-1a 64 over enabled mods in order: name + every pak's index hash. Equal fingerprints = same mods, same order, same content.
static uint64_t fnv64(uint64_t h, const void *p, size_t n) { const unsigned char *b = p; while (n--) { h ^= *b++; h *= 0x100000001B3ull; } return h; }
static uint64_t mods_fingerprint_begin(void) { return 0xCBF29CE484222325ull; }
static uint64_t mods_fingerprint_add(uint64_t h, const char *name, const unsigned char *index_hash20) {
    h = fnv64(h, name, strlen(name) + 1); return fnv64(h, index_hash20, 20);
}
#endif

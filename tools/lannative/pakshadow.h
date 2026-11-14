#ifndef LANNATIVE_PAKSHADOW_H
#define LANNATIVE_PAKSHADOW_H
// Pak shadowing: when two mounted paks hold the SAME asset path with DIFFERENT content, the pak with the higher read order is the only one the
// engine reads for that file. Typical trap: one mod's pak with a smaller copy of the attacks table shadows another mod's pak that holds a superset of those rows.
// Pure code (no engine calls). `sets` must be in READ ORDER (index = mount order, later = higher read order); ModFiles.mod is ignored here
// (two paks of one mod can shadow each other too). Also holds the one-line helper that formats the warning shown in mods.txt, the log and the F2 page.
#include "modconf.h"

#define PSH_MAX_PAIRS 24
typedef struct { int loser, winner;             // indices into `sets`
                 int ndiff, nsame;               // files of the loser that the winner replaces with different / identical content
                 char first_diff[512];           // first different path (sorted), for the message
                 char diff_paths[3][512]; } ShadowPair;
typedef struct { const PakEntry *e; int set; } ShRef;
static int shref_cmp(const void *a, const void *b) { const ShRef *x = a, *y = b; int c = _stricmp(x->e->path, y->e->path); return c ? c : (x->set - y->set); }

// Returns the number of (loser, winner) pak pairs with at least one DIFFERENT file (stored in out up to max, the rest is only counted). Pairs with only identical files are not reported.
static int pak_shadow_pairs(const ModFiles *sets, int nsets, ShadowPair *out, int max) {
    size_t total = 0; for (int i = 0; i < nsets; i++) total += (size_t)sets[i].count;
    if (!total) return 0;
    ShRef *refs = malloc(total * sizeof *refs); if (!refs) return 0;
    size_t n = 0;
    for (int i = 0; i < nsets; i++) for (int j = 0; j < sets[i].count; j++) { refs[n].e = &sets[i].entries[j]; refs[n].set = i; n++; }
    qsort(refs, n, sizeof *refs, shref_cmp);
    ShadowPair *tmp = calloc((size_t)nsets * (size_t)nsets > 4096 ? 4096 : (size_t)nsets * (size_t)nsets + 1, sizeof *tmp); int nt = 0, cap = nsets * nsets > 4096 ? 4096 : nsets * nsets;
    if (!tmp) { free(refs); return 0; }
    for (size_t i = 0; i < n;) {
        size_t j = i + 1; while (j < n && !_stricmp(refs[j].e->path, refs[i].e->path)) j++;
        const ShRef *win = &refs[j - 1];                        // highest read order = the file the engine serves
        for (size_t k = i; k + 1 < j; k++) {
            const ShRef *lo = &refs[k]; if (lo->set == win->set) continue;
            int same = !memcmp(lo->e->sha1, win->e->sha1, 20), p;
            for (p = 0; p < nt; p++) if (tmp[p].loser == lo->set && tmp[p].winner == win->set) break;
            if (p == nt) { if (nt >= cap) continue; memset(&tmp[nt], 0, sizeof tmp[nt]); tmp[nt].loser = lo->set; tmp[nt].winner = win->set; nt++; }
            if (same) tmp[p].nsame++;
            else { if (tmp[p].ndiff < 3) snprintf(tmp[p].diff_paths[tmp[p].ndiff], sizeof tmp[p].diff_paths[0], "%s", lo->e->path); if (!tmp[p].ndiff) snprintf(tmp[p].first_diff, sizeof tmp[p].first_diff, "%s", lo->e->path); tmp[p].ndiff++; }
        }
        i = j;
    }
    int found = 0;
    for (int p = 0; p < nt; p++) if (tmp[p].ndiff > 0) { if (found < max) out[found] = tmp[p]; found++; }
    free(tmp); free(refs); return found;
}

// "ModB/zzzModB.pak (read order 16000) shadows ModA/zzzModA.pak (10001): 2 file(s) differ, e.g. Absolver/Content/DB/Attacks/attacks.uasset - only the first one is read"
static void pak_shadow_text(const ShadowPair *p, const char *loser_name, unsigned loser_order, const char *winner_name, unsigned winner_order, char *out, size_t cap) {
    snprintf(out, cap, "%s (read order %u) shadows %s (%u): %d file(s) differ, e.g. %s - only the first one is read",
             winner_name, winner_order, loser_name, loser_order, p->ndiff, p->first_diff);
}
#endif

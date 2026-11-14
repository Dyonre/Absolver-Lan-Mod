#ifndef LANNATIVE_MANIFEST_H
#define LANNATIVE_MANIFEST_H
// Mod manifest (mod.json) and dependency / load-order resolution. Pure code.
//   { "id": "MyStyle", "name": "My Style", "version": "0.1.0", "api": 2, "requires": [ { "id": "ItemLoader", "min": "0.1.0" } ], "after": ["Other"], "incompatible_with": ["X"], "affects_gameplay": true }
// "requires" entries may also be plain strings; a list key given as one value ("requires": "Lib") is a one-element list. Each list keeps MF_LIST_MAX entries (more: a warning).
// A mod without mod.json (or without "id") gets its folder name as id.
// Resolution: mods that are off, need a newer API, share an id with an earlier mod, declare incompatible_with a mod that ends up on, lack a required (enabled, itself not skipped,
// new enough) mod, or are part of a requires cycle are SKIPPED with a reason; the rest are ordered topologically (requires + after), ties broken by the incoming order
// (= loadorder.txt, which the loader keeps alphabetical for new folders). "after" is only an order hint: an after loop is broken with a warning (MfResult.warn), nothing is skipped.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "modconf.h"
#include "rowsjson.h"

#define MF_LIST_MAX 8
typedef struct { char id[MOD_NAME_MAX]; char min[24]; } MfReq;
typedef struct {
    char id[MOD_NAME_MAX], name[96], version[24];
    int api;                                   // minimum AbsApi version; 0 = not stated
    int nreq; MfReq req[MF_LIST_MAX];
    int nafter; char after[MF_LIST_MAX][MOD_NAME_MAX];
    int ninc; char inc[MF_LIST_MAX][MOD_NAME_MAX];   // "incompatible_with": mod ids
    int nsup; char sup[MF_LIST_MAX][MOD_NAME_MAX];   // "supersedes": pak file names (".pak" optional) of OTHER mods that this mod replaces; those paks are not mounted while this mod loads and ships a pak
    int affects_gameplay;                      // default 1
    int has_file;                              // a mod.json was read
} Manifest;

static void manifest_default(Manifest *m, const char *folder) {
    memset(m, 0, sizeof *m); snprintf(m->id, sizeof m->id, "%s", folder); snprintf(m->name, sizeof m->name, "%s", folder); strcpy(m->version, "0"); m->affects_gameplay = 1;
}
static int mf_id_ok(const char *s) { size_t n = strlen(s); if (!n || n >= MOD_NAME_MAX) return 0; for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_' || *s == '-' || *s == '.')) return 0; return 1; }
// numeric dotted versions ("0.1.0", "1.2"): missing parts are 0, non-numeric characters are separators. <0, 0, >0
static int ver_cmp(const char *a, const char *b) {
    for (int i = 0; i < 6; i++) {
        long x = 0, y = 0;
        while (*a && (*a < '0' || *a > '9')) a++; while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
        while (*b && (*b < '0' || *b > '9')) b++; while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
static const char *mf_str(const JDoc *d, int obj, const char *key) { int c = jfind(d, obj, key); return c >= 0 && (d->n[c].t == J_STR || d->n[c].t == J_NUM) ? d->n[c].str : NULL; }
// entries of a list key c: the elements of an array, or the value itself (a single value is a one-element list; null = empty)
static int mf_first(const JDoc *d, int c) { return c < 0 || d->n[c].t == J_NULL ? -1 : d->n[c].t == J_ARR ? d->n[c].first : c; }
static int mf_next(const JDoc *d, int c, int e) { return e == c ? -1 : d->n[e].next; }
static void mf_dropped(const char *key, int n, char *err, size_t cap, int *rc) {
    if (!n) return;
    if (err && !err[0]) snprintf(err, cap, "mod.json: '%s' has more than %d entries, the last %d %s ignored", key, MF_LIST_MAX, n, n == 1 ? "was" : "were");
    if (!*rc) *rc = 1;
}
// 0 = ok (m filled; unknown keys are fine). 1 = read, but err holds a warning (entries past MF_LIST_MAX were ignored).
// On a parse problem returns -1, err says why, and m holds the folder defaults so the mod still loads as if it had no manifest.
static int manifest_parse(const char *json, const char *folder, Manifest *m, char *err, size_t cap) {
    manifest_default(m, folder); if (err && cap) err[0] = 0; if (!json) return 0;
    JDoc d; if (jdoc_parse(&d, json) != 0) { if (err) snprintf(err, cap, "mod.json: %s", d.err); jdoc_free(&d); return -1; }
    int rc = 0;
    if (d.n[0].t != J_OBJ) { if (err) snprintf(err, cap, "mod.json is not an object"); jdoc_free(&d); return -1; }
    m->has_file = 1; const char *s;
    if ((s = mf_str(&d, 0, "id"))) { if (mf_id_ok(s)) snprintf(m->id, sizeof m->id, "%s", s); else { if (err) snprintf(err, cap, "mod.json: id '%.40s' must be letters, digits, _ - . (folder name used)", s); rc = -1; } }
    if ((s = mf_str(&d, 0, "name"))) snprintf(m->name, sizeof m->name, "%s", s); else snprintf(m->name, sizeof m->name, "%s", m->id);
    if ((s = mf_str(&d, 0, "version"))) snprintf(m->version, sizeof m->version, "%s", s);
    if ((s = mf_str(&d, 0, "api"))) m->api = atoi(s);
    int c = jfind(&d, 0, "affects_gameplay"); if (c >= 0 && d.n[c].t == J_BOOL && d.n[c].str && !strcmp(d.n[c].str, "False")) m->affects_gameplay = 0;
    int drop = 0;
    c = jfind(&d, 0, "requires");
    for (int e = mf_first(&d, c); e >= 0; e = mf_next(&d, c, e)) {
        const char *id = d.n[e].t == J_STR ? d.n[e].str : d.n[e].t == J_OBJ ? mf_str(&d, e, "id") : NULL, *mn = d.n[e].t == J_OBJ ? mf_str(&d, e, "min") : NULL;
        if (!id || !mf_id_ok(id)) { if (err && !err[0]) snprintf(err, cap, "mod.json: a 'requires' entry without a valid id was ignored"); rc = -1; continue; }
        if (m->nreq == MF_LIST_MAX) { drop++; continue; }
        snprintf(m->req[m->nreq].id, MOD_NAME_MAX, "%s", id); snprintf(m->req[m->nreq].min, sizeof m->req[0].min, "%s", mn ? mn : ""); m->nreq++;
    }
    mf_dropped("requires", drop, err, cap, &rc); drop = 0;
    c = jfind(&d, 0, "after");
    for (int e = mf_first(&d, c); e >= 0; e = mf_next(&d, c, e))
        if (d.n[e].t == J_STR && mf_id_ok(d.n[e].str)) { if (m->nafter == MF_LIST_MAX) drop++; else snprintf(m->after[m->nafter++], MOD_NAME_MAX, "%s", d.n[e].str); }
    mf_dropped("after", drop, err, cap, &rc); drop = 0;
    c = jfind(&d, 0, "incompatible_with");
    for (int e = mf_first(&d, c); e >= 0; e = mf_next(&d, c, e))
        if (d.n[e].t == J_STR && mf_id_ok(d.n[e].str)) { if (m->ninc == MF_LIST_MAX) drop++; else snprintf(m->inc[m->ninc++], MOD_NAME_MAX, "%s", d.n[e].str); }
    mf_dropped("incompatible_with", drop, err, cap, &rc); drop = 0;
    c = jfind(&d, 0, "supersedes");
    for (int e = mf_first(&d, c); e >= 0; e = mf_next(&d, c, e))
        if (d.n[e].t == J_STR && d.n[e].str[0] && strlen(d.n[e].str) < MOD_NAME_MAX && !strpbrk(d.n[e].str, "/\\")) { if (m->nsup == MF_LIST_MAX) drop++; else snprintf(m->sup[m->nsup++], MOD_NAME_MAX, "%s", d.n[e].str); }
    mf_dropped("supersedes", drop, err, cap, &rc);
    jdoc_free(&d); return rc;
}

enum { MF_OK = 0, MF_OFF = 1, MF_SKIPPED = 2 };
#define MF_WARN_MAX 4
typedef struct { int status[MODS_MAX]; char reason[MODS_MAX][180]; int order[MODS_MAX]; int norder; int nwarn; char warn[MF_WARN_MAX][200]; } MfResult;   // nwarn counts all warnings, the first MF_WARN_MAX are kept
static int mf_find_id(const Manifest *mf, int n, const char *id) { for (int i = 0; i < n; i++) if (!_stricmp(mf[i].id, id)) return i; return -1; }
static int mf_lists_inc(const Manifest *m, const char *id) { for (int q = 0; q < m->ninc; q++) if (!_stricmp(m->inc[q], id)) return 1; return 0; }
// edges between mods by index (-1 = not installed, -2 = the mod itself): what i requires, what i loads after, and the incompatible_with entries that count for i
typedef struct { int nreq[MODS_MAX], req[MODS_MAX][MF_LIST_MAX], naft[MODS_MAX], aft[MODS_MAX][MF_LIST_MAX], ninc[MODS_MAX], inc[MODS_MAX][MF_LIST_MAX]; } MfGraph;
// can `from` reach `to` through mods with open[] set, along requires edges (plus after or incompatible_with edges when asked)?
static int mf_reach(const MfGraph *g, const unsigned char *open, int from, int to, int use_aft, int use_inc) {
    unsigned char seen[MODS_MAX] = {0}; int st[MODS_MAX], sp = 0; st[sp++] = from; seen[from] = 1;
    while (sp) {
        int v = st[--sp];
        for (int t = 0; t < 3; t++) {
            if ((t == 1 && !use_aft) || (t == 2 && !use_inc)) continue;
            int cnt = t == 0 ? g->nreq[v] : t == 1 ? g->naft[v] : g->ninc[v]; const int *e = t == 0 ? g->req[v] : t == 1 ? g->aft[v] : g->inc[v];
            for (int k = 0; k < cnt; k++) { int j = e[k]; if (j < 0 || !open[j]) continue; if (j == to) return 1; if (!seen[j]) { seen[j] = 1; st[sp++] = j; } }
        }
    }
    return 0;
}
// mf[i] / enabled[i] in incoming order. our_api = the loader's API version. After the call out->order lists the indices to load, in load order.
static void mf_resolve(const Manifest *mf, const int *enabled, int n, int our_api, MfResult *out) {
    memset(out, 0, sizeof *out); if (n > MODS_MAX) n = MODS_MAX;
    for (int i = 0; i < n; i++) out->status[i] = enabled[i] ? MF_OK : MF_OFF;
    for (int i = 0; i < n; i++) {
        if (out->status[i] != MF_OK) continue;
        if (mf[i].api > our_api) { out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "needs mod API %d, this loader has %d", mf[i].api, our_api); continue; }
        for (int j = 0; j < i; j++) if (out->status[j] == MF_OK && !_stricmp(mf[j].id, mf[i].id)) { out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "id '%s' is already used by the mod in folder #%d", mf[i].id, j + 1); break; }
    }
    static MfGraph g; unsigned char open[MODS_MAX] = {0}, on[MODS_MAX] = {0};
    for (int i = 0; i < n; i++) {
        g.nreq[i] = g.naft[i] = g.ninc[i] = 0;
        for (int r = 0; r < mf[i].nreq; r++) { int j = mf_find_id(mf, n, mf[i].req[r].id); g.req[i][g.nreq[i]++] = j == i ? -2 : j; }
        for (int r = 0; r < mf[i].nafter; r++) { int j = mf_find_id(mf, n, mf[i].after[r]); g.aft[i][g.naft[i]++] = j == i ? -2 : j; }
        for (int r = 0; r < mf[i].ninc; r++) {                              // both list each other: only the later mod in the incoming order counts the pair (it is the one skipped)
            int j = mf_find_id(mf, n, mf[i].inc[r]); if (j > i && mf_lists_inc(&mf[j], mf[i].id)) j = -1;
            g.inc[i][g.ninc[i]++] = j == i ? -2 : j;
        }
        open[i] = out->status[i] == MF_OK;
    }
    // Decide every open mod: ON once everything it requires is ON and no mod it is incompatible with can still end up ON; SKIPPED once something it requires
    // is missing / off / skipped / too old, or a mod it is incompatible with is ON. So only mods that end up ON exclude others.
    for (;;) {
        int changed = 0;
        for (int i = 0; i < n; i++) {
            if (!open[i]) continue;
            int wait = 0, bad = -1; const char *why = NULL; char buf[180];
            for (int r = 0; r < mf[i].nreq && !why; r++) {
                int j = g.req[i][r]; if (j == -2) continue;
                bad = r;
                if (j < 0) why = "is not installed";
                else if (out->status[j] == MF_OFF) why = "is switched off";
                else if (out->status[j] == MF_SKIPPED) why = "was skipped itself";
                else if (mf[i].req[r].min[0] && ver_cmp(mf[j].version, mf[i].req[r].min) < 0) { snprintf(buf, sizeof buf, "needs version %s or newer (found %s)", mf[i].req[r].min, mf[j].version); why = buf; }
                else if (!on[j]) wait = 1;
            }
            if (why) { open[i] = 0; out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "requires %.60s which %s", mf[i].req[bad].id, why); changed = 1; continue; }
            int hit = -1;
            for (int k = 0; k < g.ninc[i] && hit < 0; k++) { int j = g.inc[i][k]; if (j < 0 || out->status[j] != MF_OK) continue; if (on[j]) hit = j; else wait = 1; }
            if (hit >= 0) { open[i] = 0; out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "its mod.json says it is incompatible with %.60s, which is on", mf[hit].id); changed = 1; continue; }
            if (!wait) { open[i] = 0; on[i] = 1; changed = 1; }
        }
        if (changed) continue;
        int left = 0; for (int i = 0; i < n; i++) left += open[i]; if (!left) break;
        // Stuck: every open mod waits for another open one. Members of a requires cycle are skipped; mods that only require a member get "requires X which was skipped itself" next round.
        unsigned char cyc[MODS_MAX] = {0}; int ncyc = 0;
        for (int i = 0; i < n; i++) if (open[i] && mf_reach(&g, open, i, i, 0, 0)) { cyc[i] = 1; ncyc++; }
        for (int i = 0; i < n; i++) if (cyc[i]) { open[i] = 0; out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "dependency cycle (its requires lead back to it)"); }
        if (ncyc) continue;
        // No requires cycle: incompatible_with entries form a loop (A excludes B excludes C excludes A). Skip the latest mod in such a loop.
        int pick = -1, pj = -1;
        for (int i = n - 1; i >= 0 && pick < 0; i--) if (open[i]) for (int k = 0; k < g.ninc[i] && pick < 0; k++) { int j = g.inc[i][k]; if (j >= 0 && open[j] && mf_reach(&g, open, j, i, 0, 1)) { pick = i; pj = j; } }
        if (pick < 0) for (int i = n - 1; i >= 0 && pick < 0; i--) if (open[i]) pick = i;   // not reachable by construction; guarantees progress
        open[pick] = 0; out->status[pick] = MF_SKIPPED;
        snprintf(out->reason[pick], sizeof out->reason[pick], "its mod.json says it is incompatible with %.60s (incompatible_with entries form a loop; the later mod is skipped)", pj >= 0 ? mf[pj].id : "another mod");
    }
    // Order the ON mods: Kahn, always the lowest incoming index among the ready mods. Requires among ON mods have no cycle (skipped above), so a stall comes from "after":
    // the lowest mod whose requires are placed and whose after entries lead back to it loads now, with a warning naming both mods.
    int placed[MODS_MAX] = {0}, left = 0; for (int i = 0; i < n; i++) if (out->status[i] == MF_OK) left++;
    while (left) {
        int pick = -1, loopj = -1;
        for (int i = 0; i < n && pick < 0; i++) {
            if (out->status[i] != MF_OK || placed[i]) continue; int ready = 1;
            for (int r = 0; r < g.nreq[i] && ready; r++) { int j = g.req[i][r]; if (j >= 0 && out->status[j] == MF_OK && !placed[j]) ready = 0; }
            for (int r = 0; r < g.naft[i] && ready; r++) { int j = g.aft[i][r]; if (j >= 0 && out->status[j] == MF_OK && !placed[j]) ready = 0; }
            if (ready) pick = i;
        }
        if (pick < 0) {
            unsigned char un[MODS_MAX] = {0}; for (int i = 0; i < n; i++) un[i] = out->status[i] == MF_OK && !placed[i];
            int first = -1;
            for (int i = 0; i < n && pick < 0; i++) {
                if (!un[i]) continue; int reqok = 1;
                for (int r = 0; r < g.nreq[i] && reqok; r++) { int j = g.req[i][r]; if (j >= 0 && un[j]) reqok = 0; }
                if (!reqok) continue; if (first < 0) first = i;
                for (int r = 0; r < g.naft[i] && pick < 0; r++) { int j = g.aft[i][r]; if (j >= 0 && un[j] && mf_reach(&g, un, j, i, 1, 0)) { pick = i; loopj = j; } }
            }
            if (pick < 0) pick = first;                                     // a mod with placed requires always exists; first is the fallback
            if (pick < 0) break;
            if (out->nwarn < MF_WARN_MAX) snprintf(out->warn[out->nwarn], sizeof out->warn[0], "the 'after' entries of %.60s and %.60s form a loop: %.60s loads first", mf[pick].id, loopj >= 0 ? mf[loopj].id : "other mods", mf[pick].id);
            out->nwarn++;
        }
        placed[pick] = 1; out->order[out->norder++] = pick; left--;
    }
}

// Pak supersedes: mod J "supersedes" the pak name P when mf[J].sup lists P. A pak P of mod I (I != J) is then NOT mounted when J is in the load order
// (status MF_OK) and ships at least one pak itself (has_pak[J]): the typical case is a mod that ships a superset of another mod's attacks-table pak. Names compare case-insensitively
// with or without ".pak". If both mods list P (each supersedes the other's copy) the one that comes LATER in the load order keeps its pak. Returns the superseding mod's index or -1.
static int mf_name_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !_stricmp(a + la - 4, ".pak")) la -= 4;
    if (lb > 4 && !_stricmp(b + lb - 4, ".pak")) lb -= 4;
    return la == lb && !_strnicmp(a, b, la);
}
static int mf_pak_superseded(const Manifest *mf, const MfResult *res, const int *has_pak, int n, int self, const char *pak) {
    int pos[MODS_MAX]; for (int i = 0; i < n; i++) pos[i] = -1;
    for (int k = 0; k < res->norder; k++) pos[res->order[k]] = k;
    for (int j = 0; j < n; j++) {
        if (j == self || res->status[j] != MF_OK || !has_pak[j]) continue;
        int lists = 0; for (int s = 0; s < mf[j].nsup; s++) if (mf_name_eq(mf[j].sup[s], pak)) lists = 1;
        if (!lists) continue;
        int mutual = 0; for (int s = 0; s < mf[self].nsup; s++) if (mf_name_eq(mf[self].sup[s], pak)) mutual = 1;
        if (mutual && pos[self] > pos[j]) continue;                  // both claim it: the later mod in the load order keeps its pak
        return j;
    }
    return -1;
}
#endif

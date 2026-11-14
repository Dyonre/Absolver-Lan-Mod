#ifndef LANNATIVE_MANIFEST_H
#define LANNATIVE_MANIFEST_H
// Mod manifest (mod.json) and dependency / load-order resolution. Pure code.
//   { "id": "MyStyle", "name": "My Style", "version": "0.1.0", "api": 2, "requires": [ { "id": "ItemLoader", "min": "0.1.0" } ], "after": ["Other"], "affects_gameplay": true }
// "requires" / "after" entries may also be plain strings. A mod without mod.json (or without "id") gets its folder name as id.
// Resolution: mods that are off, need a newer API, share an id with an earlier mod, or lack a required (enabled, itself not skipped, new enough) mod are SKIPPED with a reason;
// the rest are ordered topologically (requires + after), ties broken by the incoming order (= loadorder.txt, which the loader keeps alphabetical for new folders).
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
// 0 = ok (m filled; unknown keys are fine). On a parse problem returns -1, err says why, and m holds the folder defaults so the mod still loads as if it had no manifest.
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
    c = jfind(&d, 0, "requires");
    if (c >= 0 && d.n[c].t == J_ARR) for (int e = d.n[c].first; e >= 0 && m->nreq < MF_LIST_MAX; e = d.n[e].next) {
        const char *id = d.n[e].t == J_STR ? d.n[e].str : d.n[e].t == J_OBJ ? mf_str(&d, e, "id") : NULL, *mn = d.n[e].t == J_OBJ ? mf_str(&d, e, "min") : NULL;
        if (!id || !mf_id_ok(id)) { if (err && !err[0]) snprintf(err, cap, "mod.json: a 'requires' entry without a valid id was ignored"); rc = -1; continue; }
        snprintf(m->req[m->nreq].id, MOD_NAME_MAX, "%s", id); snprintf(m->req[m->nreq].min, sizeof m->req[0].min, "%s", mn ? mn : ""); m->nreq++;
    }
    c = jfind(&d, 0, "after");
    if (c >= 0 && d.n[c].t == J_ARR) for (int e = d.n[c].first; e >= 0 && m->nafter < MF_LIST_MAX; e = d.n[e].next)
        if (d.n[e].t == J_STR && mf_id_ok(d.n[e].str)) snprintf(m->after[m->nafter++], MOD_NAME_MAX, "%s", d.n[e].str);
    c = jfind(&d, 0, "supersedes");
    if (c >= 0 && d.n[c].t == J_ARR) for (int e = d.n[c].first; e >= 0 && m->nsup < MF_LIST_MAX; e = d.n[e].next)
        if (d.n[e].t == J_STR && d.n[e].str[0] && strlen(d.n[e].str) < MOD_NAME_MAX && !strpbrk(d.n[e].str, "/\\")) snprintf(m->sup[m->nsup++], MOD_NAME_MAX, "%s", d.n[e].str);
    jdoc_free(&d); return rc;
}

enum { MF_OK = 0, MF_OFF = 1, MF_SKIPPED = 2 };
typedef struct { int status[MODS_MAX]; char reason[MODS_MAX][180]; int order[MODS_MAX]; int norder; } MfResult;
static int mf_find_id(const Manifest *mf, int n, const char *id) { for (int i = 0; i < n; i++) if (!_stricmp(mf[i].id, id)) return i; return -1; }
// mf[i] / enabled[i] in incoming order. our_api = the loader's API version. After the call out->order lists the indices to load, in load order.
static void mf_resolve(const Manifest *mf, const int *enabled, int n, int our_api, MfResult *out) {
    memset(out, 0, sizeof *out); if (n > MODS_MAX) n = MODS_MAX;
    for (int i = 0; i < n; i++) out->status[i] = enabled[i] ? MF_OK : MF_OFF;
    for (int i = 0; i < n; i++) {
        if (out->status[i] != MF_OK) continue;
        if (mf[i].api > our_api) { out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "needs mod API %d, this loader has %d", mf[i].api, our_api); continue; }
        for (int j = 0; j < i; j++) if (out->status[j] == MF_OK && !_stricmp(mf[j].id, mf[i].id)) { out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "id '%s' is already used by the mod in folder #%d", mf[i].id, j + 1); break; }
    }
    for (int changed = 1; changed;) {                                       // dependants of skipped mods are skipped too
        changed = 0;
        for (int i = 0; i < n; i++) {
            if (out->status[i] != MF_OK) continue;
            for (int r = 0; r < mf[i].nreq && out->status[i] == MF_OK; r++) {
                int j = mf_find_id(mf, n, mf[i].req[r].id); const char *why = NULL; char buf[180];
                if (j < 0) why = "is not installed";
                else if (j == i) continue;
                else if (out->status[j] == MF_OFF) why = "is switched off";
                else if (out->status[j] == MF_SKIPPED) why = "was skipped itself";
                else if (mf[i].req[r].min[0] && ver_cmp(mf[j].version, mf[i].req[r].min) < 0) { snprintf(buf, sizeof buf, "needs version %s or newer (found %s)", mf[i].req[r].min, mf[j].version); why = buf; }
                if (why) { out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "requires %.60s which %s", mf[i].req[r].id, why); changed = 1; }
            }
        }
    }
    int placed[MODS_MAX] = {0}, left = 0; for (int i = 0; i < n; i++) if (out->status[i] == MF_OK) left++;
    while (left) {                                                          // Kahn, always the lowest incoming index among the ready mods
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++) {
            if (out->status[i] != MF_OK || placed[i]) continue; int ready = 1;
            for (int r = 0; r < mf[i].nreq && ready; r++) { int j = mf_find_id(mf, n, mf[i].req[r].id); if (j >= 0 && j != i && out->status[j] == MF_OK && !placed[j]) ready = 0; }
            for (int r = 0; r < mf[i].nafter && ready; r++) { int j = mf_find_id(mf, n, mf[i].after[r]); if (j >= 0 && j != i && out->status[j] == MF_OK && !placed[j]) ready = 0; }
            if (ready) pick = i;
        }
        if (pick < 0) { for (int i = 0; i < n; i++) if (out->status[i] == MF_OK && !placed[i]) { out->status[i] = MF_SKIPPED; snprintf(out->reason[i], sizeof out->reason[i], "dependency cycle (requires / after)"); } break; }
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

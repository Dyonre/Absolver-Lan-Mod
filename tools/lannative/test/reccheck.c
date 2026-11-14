// reccheck <Mods folder> [--dump] [--alone] : offline rows.json / records validator + resolved-record dump, with the SAME code as the in-game loader
// (manifest.h order and dependencies, records.h flats / templates, rowsjson.h edits and conflicts, weapons.h kinds). Read-only. Exit 0 = clean, 1 = warnings or errors, 2 = usage.
// Not checked offline: property names / value types against the game's structs (the loader does that at apply time: LanNative\rows.txt).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../AbsolverSE.h"   /* ABS_API_VERSION */
#include "../manifest.h"
#include "../records.h"
#include "../weapons.h"
#include "../modpak.h"

// weapon kinds of every enabled mod (content\weapons\*.json, parsed by the loader's wp_parse_node): rules checked for every kind
typedef struct { WpKind k; DlRange r; int has_r; const char *mod; } KindRec;
static int each_json(const char *dir, void (*fn)(const char *path, void *ctx), void *ctx) {
    char pat[1200]; snprintf(pat, sizeof pat, "%s\\*.json", dir); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    do { char p[1200]; snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName); fn(p, ctx); } while (FindNextFileA(h, &fd)); FindClose(h); return 1;
}
static char *slurp(const char *path);
typedef struct { KindRec *a; int n, cap, problems; const char *mod; } KindCtx;
static void kinds_file(const char *path, void *v) {
    KindCtx *c = v; char *t = slurp(path); JDoc d; if (!t) return;
    if (jdoc_parse(&d, t) != 0) { printf("[ERROR] %s: %s\n", path, d.err); c->problems++; jdoc_free(&d); free(t); return; } free(t);
    int arr = jfind(&d, 0, "weapons");
    if (arr < 0 || d.n[arr].t != J_ARR) { printf("[ERROR] %s: needs \"weapons\": [ ... ]\n", path); c->problems++; jdoc_free(&d); return; }
    for (int e = d.n[arr].first; e >= 0; e = d.n[e].next) {
        if (c->n >= c->cap) { printf("[ERROR] more than %d weapon kinds\n", c->cap); c->problems++; break; }
        KindRec *r = &c->a[c->n]; char err[300];
        if (!wp_parse_node(&d, e, &r->k, err, sizeof err)) { printf("[ERROR] %s: weapon kind rejected by the loader: %s\n", path, err); c->problems++; continue; }
        int dk = jfind(&d, e, "deck"); r->has_r = dk >= 0 && dl_parse_range(&d, dk, &r->r, err, sizeof err) == 0; r->mod = c->mod; c->n++;
    }
    jdoc_free(&d);
}
typedef struct { const KindRec *a; int n, problems; } ItemCtx;
static void items_file(const char *path, void *v) {
    ItemCtx *c = v; char *t = slurp(path); JDoc d; if (!t) return;
    if (jdoc_parse(&d, t) != 0) { jdoc_free(&d); free(t); return; } free(t);   // items.json syntax is the content plugin's job; only the kind reference is checked here
    int arr = jfind(&d, 0, "items");
    for (int e = arr >= 0 && d.n[arr].t == J_ARR ? d.n[arr].first : -1; e >= 0; e = d.n[e].next) {
        const char *w = wp_str(&d, e, "weapon"), *id = wp_str(&d, e, "id"); if (!w) continue; int ok = 0;
        for (int i = 0; i < c->n; i++) if (!strcmp(c->a[i].k.id, w)) ok = 1;
        if (!ok) { printf("[ERROR] %s: item %s names weapon kind '%s', which no enabled mod defines\n", path, id ? id : "?", w); c->problems++; }
    }
    jdoc_free(&d);
}
static int check_kinds(const char *mods, const ModList *list, const MfResult *res, const RowOps *ops) {
    static KindRec kr[64]; KindCtx kc = { kr, 0, 64, 0, NULL }; char dir[1200];
    for (int q = 0; q < res->norder; q++) { kc.mod = list->mods[res->order[q]].name; snprintf(dir, sizeof dir, "%s\\%s\\content\\weapons", mods, kc.mod); each_json(dir, kinds_file, &kc); }
    ItemCtx ic = { kr, kc.n, 0 };
    for (int q = 0; q < res->norder; q++) { snprintf(dir, sizeof dir, "%s\\%s\\content\\items", mods, list->mods[res->order[q]].name); each_json(dir, items_file, &ic); }
    int p = kc.problems + ic.problems;
    for (int i = 0; i < kc.n; i++) { const KindRec *a = &kr[i];
        // decks: the loader turns off the later kind's decks when two kinds of one family share boxes (weaponslot.inc); boxes 1-4 are the family's own decks
        if (a->has_r && !a->r.legacy) {
            if (a->r.first <= 4) { printf("[WARN] %s: weapon kind %s decks %d-%d use the family's own boxes 1-4\n", a->mod, a->k.id, a->r.first, a->r.first + a->r.count - 1); p++; }
            for (int j = i + 1; j < kc.n; j++) { const KindRec *b = &kr[j];
                if (!b->has_r || b->r.legacy || b->k.family != a->k.family || !strcmp(a->k.id, b->k.id)) continue;
                if (a->r.first < b->r.first + b->r.count && b->r.first < a->r.first + a->r.count) { printf("[ERROR] weapon kinds %s (%s, decks %d-%d) and %s (%s, decks %d-%d) share deck boxes on family %s: the loader turns the later one's decks off\n",
                    a->k.id, a->mod, a->r.first, a->r.first + a->r.count - 1, b->k.id, b->mod, b->r.first, b->r.first + b->r.count - 1, WP_FAMILY[a->k.family]); p++; } } }
        // seeded layouts may only use the kind's own moves (vanilla Sword rows too when allow_sword_moves)
        if (a->has_r && !a->k.allow_sword) for (int d = 0; d < a->r.count; d++) { if (!a->r.has[d]) continue; const DlLayout *L = &a->r.lay[d];
            for (int row = 0; row < DL_ROWS; row++) for (int x = 0; x <= L->n[row]; x++) { const char *id = x < L->n[row] ? L->mv[row][x] : L->alt[row]; if (!id[0]) continue; int in = 0;
                for (int m = 0; m < a->k.nmoves; m++) if (!strcmp(a->k.moves[m], id)) in = 1;
                if (!in) { printf("[ERROR] %s: weapon kind %s deck %s layout %s uses row %s, which is not in the kind's moves\n", a->mod, a->k.id, a->r.names[d], DL_ROW_NAME[row], id); p++; } } }
        // a kind move that is a NEW row of the kind's own mod must be learned by everyone, or no character owns it (rows another mod adds are learned that mod's own way, e.g. through saves)
        for (int m = 0; m < a->k.nmoves; m++) for (int o = 0; o < ops->count; o++) { const RowOp *op = &ops->a[o];
            if (op->is_add && !op->learn && !strcmp(op->table, "attacks") && !strcmp(op->row, a->k.moves[m]) && !strcmp(list->mods[res->order[op->mod]].name, a->mod)) { printf("[ERROR] %s: weapon kind %s move %s is a new attacks row without \"learn\": \"always\"\n", a->mod, a->k.id, op->row); p++; } }
        printf("[ok] %s: weapon kind %s (%s, %d moves%s)\n", a->mod, a->k.id, WP_FAMILY[a->k.family], a->k.nmoves, a->has_r ? "" : ", no decks");
    }
    return p;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8L << 20)) { fclose(f); return NULL; } char *t = malloc((size_t)n + 1); if (!t) { fclose(f); return NULL; }
    size_t r = fread(t, 1, (size_t)n, f); fclose(f); t[r] = 0; return t;
}
static int cmp(const void *a, const void *b) { return _stricmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: reccheck <Mods folder> [--dump] [--alone]"); return 2; }
    int dump = 0, alone = 0, problems = 0; for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--dump")) dump = 1; else if (!strcmp(argv[i], "--alone")) alone = 1; }   // --alone: one mod packed on its own, its "requires" are not checked
    char pat[1200]; snprintf(pat, sizeof pat, "%s\\*", argv[1]); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("cannot open folder %s\n", argv[1]); return 2; }
    char *names[MODS_MAX]; int nn = 0;
    do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && nn < MODS_MAX) {
        if (strlen(fd.cFileName) >= MOD_NAME_MAX) { printf("[SKIPPED] %.40s...: folder name exceeds the %d-character limit\n", fd.cFileName, MOD_NAME_MAX - 1); problems++; }
        else names[nn++] = _strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h); qsort(names, nn, sizeof *names, cmp);
    static ModList list; snprintf(pat, sizeof pat, "%s\\loadorder.txt", argv[1]); char *lo = slurp(pat); modlist_parse(&list, lo ? lo : ""); free(lo);
    modlist_sync(&list, (const char **)names, nn);
    static Manifest mfs[MODS_MAX]; int en[MODS_MAX]; static MfResult res;
    for (int m = 0; m < list.count; m++) { snprintf(pat, sizeof pat, "%s\\%s\\mod.json", argv[1], list.mods[m].name); char *t = slurp(pat), err[200] = "";
        if (manifest_parse(t, list.mods[m].name, &mfs[m], err, sizeof err) != 0) { printf("[WARN] %s/mod.json: %s\n", list.mods[m].name, err); problems++; } free(t); en[m] = list.mods[m].enabled; if (alone) mfs[m].nreq = 0; }
    mf_resolve(mfs, en, list.count, ABS_API_VERSION, &res);
    printf("load order:"); for (int k = 0; k < res.norder; k++) printf(" %s", list.mods[res.order[k]].name); printf("\n");
    for (int m = 0; m < list.count; m++) if (res.status[m] == MF_SKIPPED) { printf("[SKIPPED] %s: %s\n", list.mods[m].name, res.reason[m]); problems++; }
    // pass 1 (records) and pass 2 (expand + edits), exactly like modglue.inc
    static RecCtx rc; static RowOps ops; static ObjOps objs; static JDoc docs[MODS_MAX]; int have[MODS_MAX] = {0}; const char *posname[MODS_MAX];
    for (int k = 0; k < res.norder; k++) {
        int m = res.order[k]; posname[k] = list.mods[m].name; snprintf(pat, sizeof pat, "%s\\%s\\rows.json", argv[1], list.mods[m].name); char *t = slurp(pat);
        { WIN32_FIND_DATAA pf; HANDLE ph; snprintf(pat, sizeof pat, "%s\\%s\\*.pak", argv[1], list.mods[m].name); ph = FindFirstFileA(pat, &pf);       // same pre-mount check as the loader
          if (ph != INVALID_HANDLE_VALUE) { do if (!(pf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { char pp[1400]; snprintf(pp, sizeof pp, "%s\\%s\\%s", argv[1], list.mods[m].name, pf.cFileName); FILE *pk = fopen(pp, "rb");
              int pok = pk ? modpak_footer_ok(pk) : 0; if (pok != 1) { printf("[ERROR] %s/%s: not a valid pak file (%s)\n", list.mods[m].name, pf.cFileName, !pk ? "cannot be read" : pok < 0 ? "index outside the file" : "bad footer"); problems++; } if (pk) fclose(pk); } while (FindNextFileA(ph, &pf)); FindClose(ph); } }
        if (!t) { snprintf(pat, sizeof pat, "%s\\%s\\rows.json", argv[1], list.mods[m].name); WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExA(pat, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (8u << 20))) { printf("[ERROR] %s/rows.json is larger than 8 MB and was not read\n", list.mods[m].name); problems++; }
            continue; }
        if (jdoc_parse(&docs[k], t) != 0) { printf("[ERROR] %s/rows.json: %s\n", list.mods[m].name, docs[k].err); problems++; free(t); continue; }
        free(t); have[k] = 1; RowsWarn w; memset(&w, 0, sizeof w); rec_collect(&rc, &docs[k], list.mods[m].name, &w);
        for (int i = 0; i < w.n && i < 8; i++) printf("[WARN] %s/rows.json: %s\n", list.mods[m].name, w.line[i]); problems += w.n;
    }
    for (int k = 0; k < res.norder; k++) {
        if (!have[k]) continue; RowsWarn w; memset(&w, 0, sizeof w); char err[200] = "";
        int defs_only = docs[k].n[0].t == J_OBJ && (jfind(&docs[k], 0, "flats") >= 0 || jfind(&docs[k], 0, "templates") >= 0);
        if (rec_uses(&docs[k])) rec_expand(&rc, &docs[k], posname[k], &w);
        int n = rows_from_json_w(&docs[k], k, &ops, &objs, err, sizeof err, &w);
        for (int i = 0; i < w.n && i < 8; i++) printf("[WARN] %s/rows.json: %s\n", posname[k], w.line[i]); problems += w.n;
        if (n <= 0 && defs_only && !err[0]) printf("[ok] %s/rows.json: flats / templates only\n", posname[k]);
        else if (n <= 0) { printf("[ERROR] %s/rows.json IGNORED by the loader: %s\n", posname[k], err[0] ? err : "no edits"); problems++; }
        else printf("[ok] %s/rows.json: %d edit(s)\n", posname[k], n);
        jdoc_free(&docs[k]);
    }
    problems += check_kinds(argv[1], &list, &res, &ops);
    char rep[8192]; rec_report(&rc, rep, sizeof rep); printf("\n%s", rep);
    static RowConflict cf[512]; int nc = rows_conflicts(&ops, cf, 512);
    printf("\nfields set by more than one mod (the later wins): %d\n", nc);
    for (int i = 0; i < nc && i < 512; i++) printf("  %s.%s.%s: %s then %s%s\n", cf[i].table, cf[i].row, cf[i].prop, posname[cf[i].first_mod], posname[cf[i].last_mod], cf[i].same ? " (same value)" : "");
    if (dump) {   // resolved records: the value each field ends with, and which mod set it
        printf("\nresolved edits (table.row.field = value  [mod]):\n");
        for (int i = 0; i < ops.count; i++) {
            const RowOp *o = &ops.a[i]; int last = 1;
            for (int j = i + 1; j < ops.count && last; j++) if (!ops.a[j].is_add && !o->is_add && !strcmp(ops.a[j].table, o->table) && !strcmp(ops.a[j].row, o->row) && !strcmp(ops.a[j].prop, o->prop)) last = 0;
            if (!last) continue;
            if (o->is_add) printf("  %s.%s = new row, copy of %s%s  [%s]\n", o->table, o->row, o->src, o->learn ? ", learned by everyone" : "", posname[o->mod]);
            else printf("  %s.%s.%s = %s  [%s]\n", o->table, o->row, o->prop, o->text, posname[o->mod]);
        }
        for (int i = 0; i < objs.count; i++) printf("  %s: %s %s = %s  [%s]\n", objs.a[i].object, objs.a[i].kind == 0 ? "set" : objs.a[i].kind == 1 ? "append" : "insert", objs.a[i].path, objs.a[i].value, posname[objs.a[i].mod]);
    }
    printf("\n%s: %d problem(s)\n", problems ? "NOT CLEAN" : "clean", problems);
    return problems ? 1 : 0;
}

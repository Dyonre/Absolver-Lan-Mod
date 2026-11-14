// content.inc - content registry of the script extender (included by lannative.c after secore.inc). Guide: docs\MODDING-GUIDE.md section 10b.
// A mod ships JSON in  Mods\<mod>\content\<kind>\*.json  (no plugin.dll of its own). At boot the mod loader lists the files in load order (content_scan_mod); at engine ready (title screen, before the
// save is loaded) and again right before a save resolves its items, every entry is validated by its kind's handler, duplicate ids are resolved by load order (later wins) and the winners are applied.
// Built-in kinds: "items" (runtime copies of stock DataAssets under /Game/Mods/<mod id>/, granted / equipped / sold), "styles" (extra Meditation > Combat Style buttons) and the others
// registered in content_init (maps, powers, npcs, bots, ranks, bot_trials, poise, clones, anim_sets, weapons). Plugins add kinds with
// AbsApi::register_content_kind. The loader's own plugin "Content" (plugins.inc, only present when some mod ships content) owns the built-in handlers. Pure logic (no game calls): contentreg.h, manifest.h.
typedef struct { int pos; char folder[MOD_NAME_MAX], modid[MOD_NAME_MAX], kind[CR_KIND_MAX], rel[160]; wchar_t path[MAX_PATH]; } CFile;
static CFile g_cf[256]; static int g_cf_n;
static int g_reloading;                                               // 1 while content_reload() applies entries: handlers update an existing entry instead of ignoring it
static int g_reload_blocked;                                          // entries of this reload that cannot be changed without a restart
static const AbsApi *CA;                                              // the loader's own plugin API ("Content")

typedef struct { char kind[CR_KIND_MAX]; int owner; int (*validate)(const char *, char *, int, void *); int (*apply)(const char *, const char *, const char *, void *); void *user; } CKind;
static CKind g_ck[16]; static int g_ck_n;
typedef struct { CrEntry cr; int kind, valid, applied, shadowed; char *json; char modid[MOD_NAME_MAX], rel[160]; } CEntry;
static CEntry g_ce[512]; static int g_ce_n; static int g_collected;
static char g_cconf[24][160]; static int g_cconf_n, g_c_invalid_n, g_c_nfiles, g_c_inuse;   // for the F2 Mods page: conflict lines, invalid entries, files, entries in use

static int content_file_count(void) { return g_cf_n; }
static void content_boot_reset(void) { g_cf_n = 0; }
static void cs_sanitize(const char *in, char *out, int cap) { int n = 0; for (; *in && n + 1 < cap; in++) out[n++] = ((*in >= '0' && *in <= '9') || (*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') || *in == '_') ? *in : '_'; out[n] = 0; }

// ----- boot: list the files of one mod (called in load order by the mod loader); returns the file count and a content hash for the LAN mod check
static int content_scan_mod(int pos, const char *folder, const char *modid, const wchar_t *dir, uint64_t *hash) {
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\content\\*", dir); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); int added = 0; *hash = 0xCBF29CE484222325ull;
    if (h == INVALID_HANDLE_VALUE) return 0;
    char *kinds[32]; int nk = 0;
    do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && nk < 32) { char u[200]; w_to_u8(fd.cFileName, u, sizeof u); kinds[nk++] = _strdup(u); } } while (FindNextFileW(h, &fd));
    FindClose(h); qsort(kinds, nk, sizeof *kinds, cmp_cstr);
    for (int k = 0; k < nk; k++) {
        if (!cr_kind_ok(kinds[k])) { logf_("content: %s: folder content\\%s ignored (a kind is lower case letters, digits, _)", folder, kinds[k]); free(kinds[k]); continue; }
        wchar_t wk[100], p2[MAX_PATH]; u8_to_w(kinds[k], wk, 100); swprintf(p2, MAX_PATH, L"%ls\\content\\%ls\\*.json", dir, wk);
        char *files[128]; int nf = 0; HANDLE h2 = FindFirstFileW(p2, &fd);
        if (h2 != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nf < 128) { char u[300]; w_to_u8(fd.cFileName, u, sizeof u); files[nf++] = _strdup(u); } } while (FindNextFileW(h2, &fd)); FindClose(h2); }
        qsort(files, nf, sizeof *files, cmp_cstr);
        for (int f = 0; f < nf; f++) {
            if (g_cf_n >= 256) { logf_("content: too many content files (256), %s ignored", files[f]); free(files[f]); continue; }
            CFile *c = &g_cf[g_cf_n]; memset(c, 0, sizeof *c); c->pos = pos; snprintf(c->folder, sizeof c->folder, "%s", folder); snprintf(c->modid, sizeof c->modid, "%s", modid); snprintf(c->kind, sizeof c->kind, "%s", kinds[k]);
            snprintf(c->rel, sizeof c->rel, "content/%s/%s", kinds[k], files[f]); wchar_t wf[300]; u8_to_w(files[f], wf, 300); swprintf(c->path, MAX_PATH, L"%ls\\content\\%ls\\%ls", dir, wk, wf);
            char *t = mods_read_text(c->path, 4u << 20); if (t) { *hash = fnv64(*hash, c->rel, strlen(c->rel)); *hash = fnv64(*hash, t, strlen(t)); free(t); }
            g_cf_n++; added++; free(files[f]);
        }
        free(kinds[k]);
    }
    return added;
}

// ----- kinds
static int ck_find(const char *kind) { for (int i = 0; i < g_ck_n; i++) if (!_stricmp(g_ck[i].kind, kind)) return i; return -1; }
static int ck_add(int owner, const char *kind, int (*v)(const char *, char *, int, void *), int (*a)(const char *, const char *, const char *, void *), void *user) {
    if (!kind || !cr_kind_ok(kind) || !v || !a || g_ck_n >= 16) return 1;
    if (ck_find(kind) >= 0) return 2;
    CKind *k = &g_ck[g_ck_n++]; memset(k, 0, sizeof *k); snprintf(k->kind, sizeof k->kind, "%s", kind); k->owner = owner; k->validate = v; k->apply = a; k->user = user; return 0;
}
static int api_p_register_content_kind(const AbsApi *self, const char *kind, int (*validate)(const char *, char *, int, void *), int (*apply)(const char *, const char *, const char *, void *), void *user) {
    int p = plug_of(self), rc = p < 0 ? 1 : ck_add(p, kind, validate, apply, user);
    logf_("plugin %s: register_content_kind '%s' -> %d%s", plug_name_of(p), kind ? kind : "?", rc, rc == 2 ? " (that kind already has a handler)" : "");
    return rc;
}

// ----- one pass over all content: collect + validate + resolve once, then apply what is not applied yet
static void content_collect(void) {
    g_collected = 1; int invalid = 0, nfiles = 0; g_cconf_n = 0;
    for (int fi = 0; fi < g_cf_n && g_ce_n < 512; fi++) {
        const CFile *f = &g_cf[fi]; int ki = ck_find(f->kind); nfiles++;
        if (ki < 0) { logf_("content: %s %s: unknown kind '%s' (no handler registered) - skipped", f->folder, f->rel, f->kind); continue; }
        char *t = mods_read_text(f->path, 4u << 20); if (!t) { logf_("content: %s %s: cannot read the file", f->folder, f->rel); continue; }
        JDoc d; if (jdoc_parse(&d, t) != 0) { logf_("content: %s %s: JSON error: %s - file skipped", f->folder, f->rel, d.err); jdoc_free(&d); free(t); invalid++; continue; }
        int nodes[256], n = cr_file_entries(&d, f->kind, nodes, 256);
        for (int i = 0; i < n && g_ce_n < 512; i++) {
            int ic = jfind(&d, nodes[i], "id"); const char *id = ic >= 0 && d.n[ic].t == J_STR ? d.n[ic].str : NULL;
            if (!id || !cr_id_ok(id)) { logf_("content: %s %s: entry %d has no valid \"id\" (letters, digits, _; at most %d) - skipped", f->folder, f->rel, i + 1, CR_ID_MAX - 1); invalid++; continue; }
            char *js = malloc(16384); if (!js) continue;
            if (jdoc_write(&d, nodes[i], js, 16384) < 0) { logf_("content: %s %s: entry '%s' is larger than 16 KB - skipped", f->folder, id, f->rel); free(js); invalid++; continue; }
            CEntry *e = &g_ce[g_ce_n]; memset(e, 0, sizeof *e); snprintf(e->cr.kind, sizeof e->cr.kind, "%s", f->kind); snprintf(e->cr.id, sizeof e->cr.id, "%s", id); e->cr.pos = f->pos; e->cr.file = fi;
            e->kind = ki; e->json = js; snprintf(e->modid, sizeof e->modid, "%s", f->modid); snprintf(e->rel, sizeof e->rel, "%s/%s", f->folder, f->rel);
            char err[200] = "no reason given"; int ok = 0; CKind *k = &g_ck[ki];
            __try { ok = k->validate(js, err, sizeof err, k->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(k->owner, "content validate", GetExceptionCode()); snprintf(err, sizeof err, "validator crashed"); ok = 0; }
            if (!ok) { logf_("content: %s %s '%s' INVALID: %s - skipped", f->kind, e->rel, id, err); invalid++; free(js); e->json = NULL; continue; }
            e->valid = 1; g_ce_n++;
        }
        jdoc_free(&d); free(t);
    }
    if (g_ce_n >= 512) logf_("content: LIMIT: 512 entries read - further entries are ignored");
    // conflicts: the later entry in load order wins (resolve over the valid entries only)
    static CrEntry tmp[512]; static int map[512]; int nt = 0;
    for (int i = 0; i < g_ce_n; i++) if (g_ce[i].valid) { tmp[nt] = g_ce[i].cr; map[nt++] = i; }
    int nconf = cr_resolve(tmp, nt);
    for (int i = 0; i < nt; i++) if (tmp[i].shadowed) {
        CEntry *lo = &g_ce[map[i]], *wi = &g_ce[map[tmp[i].winner]]; lo->shadowed = 1;
        logf_("content: CONFLICT %s '%s': %s is overridden by %s (later in the load order wins)", lo->cr.kind, lo->cr.id, lo->rel, wi->rel);
        if (g_cconf_n < 24) { snprintf(g_cconf[g_cconf_n++], sizeof g_cconf[0], "%s '%s': %s is overridden by %s", lo->cr.kind, lo->cr.id, lo->rel, wi->rel); }
    }
    int per[16] = {0}; for (int i = 0; i < g_ce_n; i++) if (g_ce[i].valid && !g_ce[i].shadowed) per[g_ce[i].kind]++;
    char sum[400] = ""; size_t w = 0; for (int k = 0; k < g_ck_n; k++) if (per[k]) w += snprintf(sum + w, sizeof sum - w, "%s%s %d", w ? ", " : "", g_ck[k].kind, per[k]);
    g_c_invalid_n = invalid; g_c_nfiles = nfiles; g_c_inuse = nt - nconf;
    logf_("content: %d file(s), %d entr%s in use (%s); %d conflict(s), %d invalid", nfiles, nt - nconf, nt - nconf == 1 ? "y" : "ies", w ? sum : "none", nconf, invalid);
}
static char g_ce_dir[MAX_PATH * 3];                                  // UTF-8 folder of the mod whose entry is being applied (for files next to mod.json, e.g. "icon_file")
static void content_set_dir(const wchar_t *json_path) {
    wchar_t d[MAX_PATH]; wcsncpy(d, json_path, MAX_PATH - 1); d[MAX_PATH - 1] = 0; wchar_t *c = wcsstr(d, L"\\content\\"); if (c) *c = 0;
    WideCharToMultiByte(CP_UTF8, 0, d, -1, g_ce_dir, sizeof g_ce_dir, NULL, NULL);
}
static void content_process(const char *why) {
    if (!g_cf_n) return;
    if (!g_collected) content_collect();
    int done = 0, retry = 0;
    for (int i = 0; i < g_ce_n; i++) {
        CEntry *e = &g_ce[i]; if (!e->valid || e->shadowed || e->applied) continue; CKind *k = &g_ck[e->kind]; int rc = 1;
        content_set_dir(g_cf[e->cr.file].path);
        __try { rc = k->apply(e->modid, e->cr.id, e->json, k->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(k->owner, "content apply", GetExceptionCode()); rc = 99; e->applied = 1; }
        if (rc == 0) { e->applied = 1; done++; } else retry++;
    }
    logf_("content: pass '%s': %d applied, %d to retry", why, done, retry);
}

// ----- JSON field helpers for the built-in handlers
static const char *cs_str(const JDoc *d, int o, const char *k) { int c = jfind(d, o, k); return c >= 0 && (d->n[c].t == J_STR || d->n[c].t == J_NUM || d->n[c].t == J_BOOL) ? d->n[c].str : NULL; }
static void cs_quote_clean(const char *in, char *out, int cap) { int n = 0; for (; *in && n + 1 < cap; in++) out[n++] = (*in == '"' || *in == '\\') ? '\'' : *in; out[n] = 0; }   // text inside INVTEXT("...")
static int cs_true(const JDoc *d, int o, const char *k) { const char *v = cs_str(d, o, k); return v && (!_stricmp(v, "true") || !strcmp(v, "1")); }
static void cs_warn_unknown(const JDoc *d, const char *what, const char *id, const char *const *known) {
    for (int c = d->n[0].first; c >= 0; c = d->n[c].next) { int ok = 0; for (int k = 0; known[k]; k++) if (d->n[c].key && !strcmp(d->n[c].key, known[k])) ok = 1; if (!ok) logf_("content: %s '%s': unknown field '%s' (ignored)", what, id, d->n[c].key ? d->n[c].key : "?"); }
}

// ================================================================ kind "items"
#include "decklayout.h"                                           // DL_ID_BUF (48-character row names), dl_name_check
#define CI_MOVES_MAX 128                                                // "moves" per item
typedef struct {
    char mod[MOD_NAME_MAX], id[CR_ID_MAX], base[300], name[200], desc[400], icon[300], shop[64], shop_copy[64], path[400], row[160], icon_file[MAX_PATH * 3];
    int nprops; char pk[16][64], pv[16][8192];   /* 8192: a 21-dye m_Materials prop needs ~5-8 KB */ int price, grant, equip; void *obj; int shop_done, granted, have_desc, gtries, gwhy;   // granted = done for this character (grantpoll.h); gtries / gwhy = poll attempts / last reason
    char gtable[200], gcopy[64]; int ngs; char gsk[16][64], gsv[16][200]; int gear_done;   // optional own gear row ("gear_row"), e.g. a weapon's WeaponDataTable row
    uint64_t icon_hash; int icon_gen;                                                       // icon_file bytes (FNV) and how many times its texture was re-made (hot reload)
    int nmoves, family, moves_ok; char moves[CI_MOVES_MAX][DL_ID_BUF]; void *move_rows[CI_MOVES_MAX];                  // optional "moves": attack rows allowed in this weapon's family deck only while it is equipped
    char weapon[64];                                                                        // "weapon": the weapon kind (content kind "weapons", weapons.inc) this item is
} CItem;
#define CI_ITEMS_MAX 128
static CItem g_items[CI_ITEMS_MAX]; static int g_items_n;
static void grants_arm(const char *why);

static int items_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    static const char *const known[] = { "id", "base", "name", "description", "icon", "props", "shop", "shop_copy_of", "price", "grant", "equip", "gear_row", "moves", "icon_file", "moves_exclusive", "deck", "style", "anim_style", "deflect_style", "weapon", NULL };
    const char *base = cs_str(&d, 0, "base"), *id = cs_str(&d, 0, "id"), *shop = cs_str(&d, 0, "shop"); int ok = 1, g = jfind(&d, 0, "gear_row");
    if (!base || base[0] != '/' || !strchr(base, '.')) { snprintf(err, cap, "needs \"base\": the stock asset to copy, like /Game/DB/Equipment/Masks/X.X"); ok = 0; }
    else if (shop && !cs_str(&d, 0, "shop_copy_of")) { snprintf(err, cap, "\"shop\" needs \"shop_copy_of\" (an existing row of that table to clone)"); ok = 0; }
    else if (jfind(&d, 0, "props") >= 0 && d.n[jfind(&d, 0, "props")].t != J_OBJ) { snprintf(err, cap, "\"props\" must be an object"); ok = 0; }
    else if (g >= 0 && (d.n[g].t != J_OBJ || !cs_str(&d, g, "table") || !cs_str(&d, g, "copy_of") || (jfind(&d, g, "set") >= 0 && d.n[jfind(&d, g, "set")].t != J_OBJ))) { snprintf(err, cap, "\"gear_row\" must be an object with \"table\" (like WeaponDataTable), \"copy_of\" (a row of it) and optional \"set\" (an object)"); ok = 0; }
    else if (jfind(&d, 0, "moves") >= 0 && d.n[jfind(&d, 0, "moves")].t != J_ARR) { snprintf(err, cap, "\"moves\" must be an array of attack row names, like [\"321\"]"); ok = 0; }
    if (ok) cs_warn_unknown(&d, "items", id ? id : "?", known);
    jdoc_free(&d); return ok;
}
// created at engine ready (BEFORE the save resolves its item paths), copy of the stock asset, own name / icon / props, optional shop row
static void items_replace_icons(void *o, const char *icon) {
    static char in[32768], out[32768]; int n = CA->get_property_text(CA, o, "m_Materials", in, sizeof in); if (n <= 0) return;
    char *p = in, *q = out; const char *key = "m_InventoryIcon="; int reps = 0;
    while (*p && q - out < (int)sizeof out - 600) {
        if (!strncmp(p, key, strlen(key))) { q += sprintf(q, "%s%s", key, icon); p += strlen(key); while (*p && *p != ',' && *p != ')') p++; reps++; }
        else *q++ = *p++;
    }
    *q = 0; if (reps) logf_("content: item icon: %d m_Materials icon(s) replaced -> %d", reps, CA->set_property_text(CA, o, "m_Materials", out));
}
// "moves": UAttackComponent::IsAttackCompatibleWithWeaponFamily (static, 0x300CA0; called by the deck editor's FilterAttack, BPF_ValidateComboAttack, AddAttackToDeckStruct,
// BPF_GetAttackDataDamage; not by combat) answers for the item's family from "is the item equipped by the local player" for the bound rows; every other call is unchanged.
#define CT_ATTACKS "/Game/DB/Attacks/attacks.attacks"
static unsigned char (*g_o_famchk)(unsigned char fam, void *row, unsigned char mirror, unsigned char exclusive);
static void *g_eq_weapon; static ULONGLONG g_eq_at;
static void *items_equipped_weapon(void) {                       // local player's UInventoryComponent::m_EquippedWeapon, cached for 200 ms (the editor asks once per row)
    ULONGLONG now = GetTickCount64(); if (now - g_eq_at < 200) return g_eq_weapon;
    g_eq_at = now; g_eq_weapon = NULL; void *pc = CA->get_local_player_controller(CA), *pawn = NULL, *f = pc ? CA->find_function(CA, pc, "K2_GetPawn") : NULL;
    if (f) CA->call_function_params(CA, pc, f, NULL, &pawn);
    void **ic = pawn ? (void **)CA->prop_ptr(CA, pawn, "m_InventoryComponent") : NULL; void **w = ic && *ic ? (void **)CA->prop_ptr(CA, *ic, "m_EquippedWeapon") : NULL;
    if (w) g_eq_weapon = *w; return g_eq_weapon;
}
static void items_resolve_moves(CItem *it) {
    if (it->moves_ok || !it->nmoves || !CA->find_object(CA, CT_ATTACKS)) return;
    int n = 0; for (int i = 0; i < it->nmoves; i++) { it->move_rows[i] = CA->table_row(CA, CT_ATTACKS, it->moves[i]); if (it->move_rows[i]) n++; else logf_("content: item %s: move row '%s' not in the attacks table - ignored", it->id, it->moves[i]); }
    it->moves_ok = 1; logf_("content: item %s: %d of %d move row(s) bound to family %d (allowed there only while the item is equipped)", it->id, n, it->nmoves, it->family);
}
static int ws_famchk(unsigned char fam, void *row, unsigned char *r);    // weaponslot.inc
static unsigned char items_famchk_det(unsigned char fam, void *row, unsigned char mirror, unsigned char exclusive) {
    static int logged; { unsigned char wr; if (ws_famchk(fam, row, &wr)) return wr; }
    for (int i = 0; i < g_items_n; i++) {
        CItem *it = &g_items[i]; if (!it->nmoves || !it->obj || fam != it->family) continue;
        items_resolve_moves(it);
        for (int k = 0; k < it->nmoves; k++) if (it->move_rows[k] == row) {
            unsigned char r = items_equipped_weapon() == it->obj;
            if (logged < 40) { logged++; logf_("content: family check: row %s, family %d, mirror %d -> %d (%s equipped: %d)", it->moves[k], fam, mirror, r, it->id, r); }
            return r;
        }
    }
    return g_o_famchk(fam, row, mirror, exclusive);
}
#include "equiprule.h"
#include "weapons.inc"                                                // kind "weapons" (items.weapon, UI identity of the kind)
#include "weaponslot.inc"
#include "learn.inc"                                                  // rows.json adds with "learn": "always" count as learned (BPF_GetAttackLearningState detour)
static int items_parse(CItem *it, const char *mod, const char *id, const char *json) {
    JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    memset(it, 0, sizeof *it); snprintf(it->mod, sizeof it->mod, "%s", mod); snprintf(it->id, sizeof it->id, "%s", id);
    const char *v; char ms[MOD_NAME_MAX]; cs_sanitize(mod, ms, sizeof ms);
    if ((v = cs_str(&d, 0, "base"))) snprintf(it->base, sizeof it->base, "%s", v);
    if ((v = cs_str(&d, 0, "name"))) snprintf(it->name, sizeof it->name, "%s", v);
    if ((v = cs_str(&d, 0, "description"))) { snprintf(it->desc, sizeof it->desc, "%s", v); it->have_desc = 1; }
    if ((v = cs_str(&d, 0, "icon"))) snprintf(it->icon, sizeof it->icon, "%s", v);
    if ((v = cs_str(&d, 0, "icon_file"))) {                       // an image next to mod.json: made into a runtime texture /Game/Mods/<mod>/<id>_Icon (API 2.5 texture_from_file)
        if (strstr(v, "..") || strchr(v, ':')) logf_("content: item %s: icon_file '%s' must be a file inside the mod folder - ignored", id, v);
        else snprintf(it->icon_file, sizeof it->icon_file, "%s\\%s", g_ce_dir, v);
    }
    if ((v = cs_str(&d, 0, "shop"))) snprintf(it->shop, sizeof it->shop, "%s", v);
    if ((v = cs_str(&d, 0, "shop_copy_of"))) snprintf(it->shop_copy, sizeof it->shop_copy, "%s", v);
    if ((v = cs_str(&d, 0, "price"))) it->price = atoi(v);
    it->grant = cs_true(&d, 0, "grant"); it->equip = cs_true(&d, 0, "equip");
    int pc = jfind(&d, 0, "props");
    if (pc >= 0) for (int c = d.n[pc].first; c >= 0 && it->nprops < 16; c = d.n[c].next) if (d.n[c].key && d.n[c].str) { snprintf(it->pk[it->nprops], 64, "%s", d.n[c].key); snprintf(it->pv[it->nprops], sizeof it->pv[0], "%s", d.n[c].str); it->nprops++; }
    int mv = jfind(&d, 0, "moves");
    if (mv >= 0) for (int c = d.n[mv].first; c >= 0 && it->nmoves < CI_MOVES_MAX; c = d.n[c].next) {      // move ids are row names of 1..DL_ID_MAX (48) characters; a rejected one is LOGGED, never dropped silently
        char why[120]; if (d.n[c].t != J_STR || !d.n[c].str) { logf_("content: item %s: \"moves\" entry is not a string - dropped", id); continue; }
        if (!dl_name_check(d.n[c].str, why, sizeof why)) { logf_("content: item %s: \"moves\" entry '%.80s' REJECTED: %s", id, d.n[c].str, why); continue; }
        snprintf(it->moves[it->nmoves++], DL_ID_BUF, "%s", d.n[c].str); }
    if (mv >= 0 && it->nmoves == CI_MOVES_MAX && d.n[mv].first >= 0) { int all = 0; for (int c = d.n[mv].first; c >= 0; c = d.n[c].next) all++; if (all > CI_MOVES_MAX) logf_("content: item %s: %d \"moves\", only the first %d are used (CI_MOVES_MAX)", it->id, all, CI_MOVES_MAX); }
    int g = jfind(&d, 0, "gear_row");
    if (g >= 0) {
        const char *t = cs_str(&d, g, "table");
        if (t[0] == '/') snprintf(it->gtable, sizeof it->gtable, "%s", t); else snprintf(it->gtable, sizeof it->gtable, "/Game/DB/Gear/%s.%s", t, t);
        snprintf(it->gcopy, sizeof it->gcopy, "%s", cs_str(&d, g, "copy_of"));
        int gs = jfind(&d, g, "set");
        if (gs >= 0) for (int c = d.n[gs].first; c >= 0 && it->ngs < 16; c = d.n[c].next) if (d.n[c].key && d.n[c].str) { snprintf(it->gsk[it->ngs], 64, "%s", d.n[c].key); snprintf(it->gsv[it->ngs], 200, "%s", d.n[c].str); it->ngs++; }
    }
    snprintf(it->path, sizeof it->path, "/Game/Mods/%s/%s.%s", ms, id, id); snprintf(it->row, sizeof it->row, "%s_%s", ms, id);
    if ((v = cs_str(&d, 0, "weapon"))) snprintf(it->weapon, sizeof it->weapon, "%s", v);
    jdoc_free(&d);
    wp_item_merge(it);                                                // the weapon kind's moves + damage / grip gear values
    return 1;
}

// the object's texts, icon and properties from the entry (creation, and again on a hot reload)
static uint64_t cs_file_hash(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0; uint64_t h = 0xCBF29CE484222325ull; unsigned char b[8192]; size_t k;
    while ((k = fread(b, 1, sizeof b, f)) > 0) h = fnv64(h, b, k);
    fclose(f); return h ? h : 1;
}
static void items_fill(CItem *it, void *o) {
    const char *id = it->id, *mod = it->mod;
    if (it->name[0]) logf_("content:   %s m_ItemName '%s' -> %d", id, it->name, CA->set_text(CA, o, "m_ItemName", it->name));
    if (it->have_desc) { if (CA->find_property(CA, CA->object_class(CA, o), "m_Description")) logf_("content:   %s m_Description -> %d", id, CA->set_text(CA, o, "m_Description", it->desc)); else logf_("content:   %s has no m_Description (equipment takes it from its gear row) - ignored", id); }
    if (it->icon_file[0]) {
        char tp[300], ms2[MOD_NAME_MAX], gen[16] = ""; cs_sanitize(mod, ms2, sizeof ms2); if (it->icon_gen) snprintf(gen, sizeof gen, "%d", it->icon_gen);
        snprintf(tp, sizeof tp, "/Game/Mods/%s/%s_Icon%s.%s_Icon%s", ms2, id, gen, id, gen); it->icon_hash = cs_file_hash(it->icon_file);
        if (CA->texture_from_file(CA, it->icon_file, tp)) snprintf(it->icon, sizeof it->icon, "%s", tp); else logf_("content: item %s: icon_file %s could not be made into a texture - stock icon kept", id, it->icon_file);
    }
    if (it->icon[0]) { logf_("content:   %s m_InventoryIcon %s -> %d", id, it->icon, CA->set_property_text(CA, o, "m_InventoryIcon", it->icon)); items_replace_icons(o, it->icon); }
    for (int i = 0; i < it->nprops; i++) logf_("content:   %s %s = %s -> %d", id, it->pk[i], it->pv[i], CA->set_property_text(CA, o, it->pk[i], it->pv[i]));
}
// ----- hot reload of an existing item (content_reload): texts, icon, properties, price and gear-row values are re-applied; what was baked in at creation is not
static void items_refresh_rows(CItem *it) {
    char num[32], tx[460], clean[400]; int n = 0;
    if (it->shop_done && it->shop[0]) {
        char table[200]; snprintf(table, sizeof table, "/Game/DB/Shop/%s.%s", it->shop, it->shop); snprintf(num, sizeof num, "%d", it->price);
        logf_("content: reload: item %s: shop price %d -> %d", it->id, it->price, CA->row_set_text(CA, table, it->row, "m_iFragmentPrice", num));
    }
    if (it->gear_done && it->gtable[0]) {
        static const char *const nf[] = { "m_RealName", "m_RealNameDirty", "m_RealNameGold" }, *const df[] = { "m_Description", "m_DescriptionDirty", "m_DescriptionGold" };
        if (it->name[0]) { cs_quote_clean(it->name, clean, sizeof clean); snprintf(tx, sizeof tx, "INVTEXT(\"%s\")", clean); for (int i = 0; i < 3; i++) n += CA->row_set_text(CA, it->gtable, it->row, nf[i], tx); }
        if (it->have_desc) { cs_quote_clean(it->desc, clean, sizeof clean); snprintf(tx, sizeof tx, "INVTEXT(\"%s\")", clean); for (int i = 0; i < 3; i++) n += CA->row_set_text(CA, it->gtable, it->row, df[i], tx); }
        int s2 = 0; for (int i = 0; i < it->ngs; i++) s2 += CA->row_set_text(CA, it->gtable, it->row, it->gsk[i], it->gsv[i]);
        logf_("content: reload: item %s: gear row %s: %d text(s), %d of %d value(s) set", it->id, it->row, n, s2, it->ngs);
    }
}
static int items_reload(CItem *it, const char *mod, const char *id, const char *json) {
    static CItem n; if (!items_parse(&n, mod, id, json)) { logf_("content: reload: item %s/%s: the entry could not be parsed - kept as it was", mod, id); return 0; }
    const char *bad = NULL; int moves_same = n.nmoves == it->nmoves; for (int i = 0; moves_same && i < n.nmoves; i++) if (strcmp(n.moves[i], it->moves[i])) moves_same = 0;
    if (strcmp(n.base, it->base)) bad = "base"; else if (strcmp(n.shop, it->shop) || strcmp(n.shop_copy, it->shop_copy)) bad = "shop / shop_copy_of"; else if (strcmp(n.gtable, it->gtable) || strcmp(n.gcopy, it->gcopy)) bad = "gear_row table / copy_of"; else if (!moves_same) bad = "moves";
    if (bad) { g_reload_blocked++; logf_("content: reload: item %s/%s: '%s' changed - that needs a restart (the object, its shop / gear row or move binding exists already); this entry was not changed", mod, id, bad); return 0; }
    int icon_changed = strcmp(n.icon, it->icon) && !it->icon_file[0];
    if (n.icon_file[0] && it->icon_file[0] && cs_file_hash(n.icon_file) != it->icon_hash) { it->icon_gen++; icon_changed = 1; }     // a new texture object: an existing one cannot be overwritten
    else if (strcmp(n.icon_file, it->icon_file)) { it->icon_gen++; icon_changed = 1; }
    char keep_icon[300]; snprintf(keep_icon, sizeof keep_icon, "%s", it->icon);
    #define RK(f) memcpy(&it->f, &n.f, sizeof n.f)
    RK(name); RK(desc); RK(have_desc); RK(icon_file); RK(nprops); RK(pk); RK(pv); RK(price); RK(grant); RK(equip); RK(ngs); RK(gsk); RK(gsv);
    #undef RK
    if (it->icon_file[0]) { if (!icon_changed) snprintf(it->icon, sizeof it->icon, "%s", keep_icon); } else snprintf(it->icon, sizeof it->icon, "%s", n.icon);
    logf_("content: reload: item %s/%s: name '%s', price %d, %d prop(s)%s", mod, id, it->name, it->price, it->nprops, icon_changed ? ", new icon" : "");
    if (it->obj) { items_fill(it, it->obj); items_refresh_rows(it); }
    if (it->grant && !it->granted) grants_arm("hot reload");
    return 0;
}
static int items_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; CItem *it = NULL;
    for (int i = 0; i < g_items_n; i++) if (!strcmp(g_items[i].mod, mod) && !strcmp(g_items[i].id, id)) it = &g_items[i];
    if (it && g_reloading) return items_reload(it, mod, id, json);
    if (!it) {
        if (g_items_n >= CI_ITEMS_MAX) { logf_("content: items: too many items (%d), %s ignored", CI_ITEMS_MAX, id); return 0; }
        CItem *slot = &g_items[g_items_n]; if (!items_parse(slot, mod, id, json)) return 0;
        it = slot; g_items_n++;
        logf_("content: item %s/%s: base %s, name '%s', icon '%s', shop '%s' price %d, grant %d, equip %d -> %s", mod, id, it->base, it->name, it->icon, it->shop, it->price, it->grant, it->equip, it->path);
    }
    if (!it->obj) {
        void *src = CA->find_object(CA, it->base); if (!src) src = CA->load_object(CA, it->base);
        if (!src) { logf_("content: item %s: base asset %s could not be loaded - NOT created (retried before the save loads)", id, it->base); return 1; }
        void *o = CA->create_asset(CA, src, it->path); if (!o) { logf_("content: item %s: create_asset(%s) failed", id, it->path); return 1; }
        it->obj = o;
        items_fill(it, o);
        char nm[200] = ""; void *np = CA->prop_ptr(CA, o, "m_ItemName"); if (np) CA->text_to_string(CA, np, nm, sizeof nm);
        logf_("content: item %s ready: %p %s, m_ItemName now '%s'", id, o, it->path, nm);
        if (it->nmoves) {
            char fam[40] = ""; CA->get_property_text(CA, o, "m_eWeaponFamily", fam, sizeof fam);
            it->family = !strcmp(fam, "BareHands") ? 0 : !strcmp(fam, "Sword") ? 1 : !strcmp(fam, "WarGloves") ? 2 : -1;
            if (it->family < 0) { logf_("content: item %s: \"moves\" needs a weapon (m_eWeaponFamily is '%s') - ignored", id, fam); it->nmoves = 0; }
            else if (!g_o_famchk) { g_o_famchk = (unsigned char (*)(unsigned char, void *, unsigned char, unsigned char))CA->hook_symbol(CA, "UAttackComponent::IsAttackCompatibleWithWeaponFamily", (void *)items_famchk_det); logf_("content: family-check hook -> %p", (void *)g_o_famchk); if (!g_o_famchk) it->nmoves = 0; }
            items_resolve_moves(it);
        }
        ws_item_ready(it, json);
    }
    if (it->gtable[0] && !it->gear_done) {             // own gear row: stats, weapon capsule, row name / description; the item becomes the row's clean variant
        if (!CA->find_object(CA, it->gtable)) { logf_("content: item %s: gear table %s is not loaded yet", id, it->gtable); return 1; }
        char srcrow[64] = ""; CA->get_property_text(CA, it->obj, "m_GearDataRowName", srcrow, sizeof srcrow);
        if (CA->table_add_row(CA, it->gtable, it->row, it->gcopy)) {
            // Dirty / Gold variants None: UGearData::GetQualityFromDataRow (0x406050) then reads Clean
            int a = CA->row_set_text(CA, it->gtable, it->row, "m_GearDataAsset", it->path), b = CA->row_set_text(CA, it->gtable, it->row, "m_GearDataAssetDirty", "None"), c = CA->row_set_text(CA, it->gtable, it->row, "m_GearDataAssetGold", "None");
            static const char *const nf[] = { "m_RealName", "m_RealNameDirty", "m_RealNameGold" }, *const df[] = { "m_Description", "m_DescriptionDirty", "m_DescriptionGold" };
            char tx[460], clean[400]; int n = 0;
            if (it->name[0]) { cs_quote_clean(it->name, clean, sizeof clean); snprintf(tx, sizeof tx, "INVTEXT(\"%s\")", clean); for (int i = 0; i < 3; i++) n += CA->row_set_text(CA, it->gtable, it->row, nf[i], tx); }
            if (it->have_desc) { cs_quote_clean(it->desc, clean, sizeof clean); snprintf(tx, sizeof tx, "INVTEXT(\"%s\")", clean); for (int i = 0; i < 3; i++) n += CA->row_set_text(CA, it->gtable, it->row, df[i], tx); }
            int s = 0; for (int i = 0; i < it->ngs; i++) s += CA->row_set_text(CA, it->gtable, it->row, it->gsk[i], it->gsv[i]);
            int r = CA->set_property_text(CA, it->obj, "m_GearDataRowName", it->row);
            // UGearData caches its row pointer (m_GearDataRow, +0x88, between m_eEquipmentQuality +0x80 and m_GearDataTable +0x90): drop a cache that still points at the source row
            void **cache = (void **)((uint8_t *)it->obj + 0x88), *srcp = srcrow[0] ? CA->table_row(CA, it->gtable, srcrow) : NULL, *own = CA->table_row(CA, it->gtable, it->row), *before = *cache;
            if (before && before != own) *cache = NULL;
            logf_("content: item %s: gear row %s (copy of %s, source row %s): asset %d, dirty/gold None %d %d, texts %d, set %d/%d, m_GearDataRowName -> %d, row cache %p (source %p, own %p) -> %p", id, it->row, it->gcopy, srcrow, a, b, c, n, s, it->ngs, r, before, srcp, own, *cache);
        }
        it->gear_done = 1;
    }
    if (it->shop[0] && !it->shop_done) {
        char table[200], num[32]; snprintf(table, sizeof table, "/Game/DB/Shop/%s.%s", it->shop, it->shop);
        if (!CA->find_object(CA, table)) { logf_("content: item %s: shop table %s is not loaded yet", id, table); return 1; }
        if (CA->table_add_row(CA, table, it->row, it->shop_copy)) {
            int a = CA->row_set_text(CA, table, it->row, "m_Equipment", it->path);                     // EquipmentShop rows; WeaponShop rows name the item m_Weapon
            if (!a) a = CA->row_set_text(CA, table, it->row, "m_Weapon", it->path) ? 2 : 0;
            snprintf(num, sizeof num, "%d", it->price);
            int b = CA->row_set_text(CA, table, it->row, "m_iFragmentPrice", num), c = CA->row_set_text(CA, table, it->row, "m_iGMLevelRequired", "0"), d2 = CA->row_set_text(CA, table, it->row, "m_iGleamLevelRequired", "0"), e = CA->row_set_text(CA, table, it->row, "m_iPrestigePointPrice", "0");
            logf_("content: item %s: shop row %s.%s: m_Equipment %d, price %d (%d), levels %d %d, crystals %d", id, it->shop, it->row, a, it->price, b, c, d2, e);
        }
        it->shop_done = 1;
    }
    return 0;
}
// Auto-equip rule (equiprule.h): first grant only, one mod item per slot (the first in load order), never over an item the loader did not equip.
static char g_eq_slot[16][48], g_eq_by[16][CR_ID_MAX * 2]; static int g_eq_n;
static void items_try_equip(CItem *it, int idx, int newly) {
    char slot[48]; eq_slot_of(it->base, slot, sizeof slot); const char *owner = NULL;
    for (int k = 0; k < g_eq_n; k++) if (!strcmp(g_eq_slot[k], slot)) owner = g_eq_by[k];
    int other = 0; char oname[96] = "";
    if (!owner && !strcmp(slot, "weapon")) { g_eq_at = 0; void *w = items_equipped_weapon(); if (w && w != it->obj) { other = 1; CA->object_name(CA, w, oname, sizeof oname); } }
    EqVerdict v = eq_decide(newly, owner, other);
    if (v == EQ_YES) {
        logf_("content: equip %s (index %d, slot %s) -> %d", it->id, idx, slot, CA->inventory_equip(CA, idx));
        if (g_eq_n < 16) { snprintf(g_eq_slot[g_eq_n], sizeof g_eq_slot[0], "%s", slot); snprintf(g_eq_by[g_eq_n], sizeof g_eq_by[0], "%s/%s", it->mod, it->id); g_eq_n++; }
        return;
    }
    logf_("content: equip %s skipped (slot %s): %s", it->id, slot, eq_why(v));
    if (v == EQ_SLOT_TAKEN_BY_MOD) mods_warn("equip conflict, slot %s: %s/%s has equip:true but %s already equipped it - only the first is equipped, set equip false in one", slot, it->mod, it->id, owner);
    else if (v == EQ_SLOT_HAS_OTHER_ITEM) mods_warn("%s/%s has equip:true but the %s slot already holds %s (player's or save's choice) - not replaced", it->mod, it->id, slot, oname[0] ? oname : "another item");
}
// grants (docs\PLUGIN-API.md "Grants"): an item with grant:true is pending per character (re-armed when a save loads) until the local player's inventory
// takes it; polled every GP_POLL_S s from every non-title world (re-armed on each on_world_loaded) while anything is pending (grantpoll.h), so a world that
// appears late or a missed on_world_loaded does not lose the grant. An owned item is never added twice.
static GpReady g_gp_ready = { -1, 0 }; static int g_gp_timer, g_gp_world_ok, g_gp_ready_why = GP_NO_PAWN, g_gp_polls;
static void powers_grant_step(int ready, int *pending); static void powers_grant_rearm(void);   // powers.inc
static void items_grant_step(int ready, int *pending) {
    for (int i = 0; i < g_items_n; i++) {
        CItem *it = &g_items[i]; if (!it->grant || it->granted) continue;
        int why = it->obj ? ready : GP_NO_OBJECT, owned = why == GP_OK ? CA->inventory_index(CA, it->obj) : -1, idx = -1; it->gtries++;
        GpVerdict v = gp_decide(why, owned);
        if (v == GPV_OWNED) { it->granted = 1; logf_("content: %s already owned (inventory index %d), not granted again", it->id, owned); if (it->equip) items_try_equip(it, owned, 0); continue; }
        if (v == GPV_ADD) {
            idx = CA->inventory_add(CA, it->obj, 0, 0);
            if (idx >= 0) { it->granted = 1; logf_("content: granted %s -> inventory index %d (attempt %d)", it->id, idx, it->gtries); if (it->equip) items_try_equip(it, idx, 1); continue; }
            why = GP_ADD_FAILED;
        }
        if (gp_log_now(it->gtries, why, it->gwhy)) logf_("content: grant %s: waiting (%s), attempt %d", it->id, gp_why(why), it->gtries);
        it->gwhy = why; (*pending)++;
    }
}
static void grants_poll(void *u) {
    (void)u; g_gp_timer = 0; if (!g_gp_world_ok) { logf_("content: grants: poll stopped (title / no world)"); return; }
    int n = 0, probe = se_inventory_probe(CA, &n), ready = gp_ready(&g_gp_ready, probe, n), pending = 0; g_gp_ready_why = ready; g_gp_polls++;
    items_grant_step(ready, &pending); powers_grant_step(ready, &pending);
    if (pending) g_gp_timer = CA->set_timer_id(CA, GP_POLL_S, 0, grants_poll, NULL);
    else logf_("content: grants done (nothing pending; %d poll(s), inventory %d item(s))", g_gp_polls, n);
}
static void grants_arm(const char *why) {
    if (!g_gp_world_ok || g_gp_timer || !CA) return;
    g_gp_timer = CA->set_timer_id(CA, GP_POLL_S, 0, grants_poll, NULL); g_gp_polls = 0;
    logf_("content: grants: polling every %.1f s (%s) -> timer %d", GP_POLL_S, why, g_gp_timer);
}
static void grants_rearm(void) {                                       // a character save is loading: every grant is pending again for it (owned ones resolve as "already owned")
    for (int i = 0; i < g_items_n; i++) { g_items[i].granted = 0; g_items[i].gtries = 0; g_items[i].gwhy = 0; }
    powers_grant_rearm();
}
static void items_world(const char *map, void *u) {
    (void)u; g_gp_world_ok = map && map[0] && !strstr(map, "/StartMap"); g_gp_ready.last_n = -1; g_gp_ready.stable = 0;
    if (g_gp_world_ok) grants_arm(map);
}
// F2 Mods page: one WARNING line per grant still pending after a few polls in a world (k = 0, 1, ...); 0 = no more lines
static int grants_warn_line(int k, char *out, int cap);

// ================================================================ kind "styles"
typedef struct { char mod[MOD_NAME_MAX], id[CR_ID_MAX], db[300], name[120], icon[300]; int style_id, index, gleam_level; void *btn; } CStyle;   // gleam_level 0 = always unlocked (stylegate.h)
#include "stylegate.h"                                              // sg_unlocked / sg_parse_level (pure, offline test)
static CStyle g_styles[8]; static int g_styles_n, g_styles_hooked; static void *g_style_table;

static int styles_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    static const char *const known[] = { "id", "db", "style_id", "index", "name", "icon", "gleam_level", NULL };
    const char *db = cs_str(&d, 0, "db"), *sid = cs_str(&d, 0, "style_id"), *ix = cs_str(&d, 0, "index"), *id = cs_str(&d, 0, "id"); int ok = 1;
    if (!db || db[0] != '/' || !strchr(db, '.')) { snprintf(err, cap, "needs \"db\": the FightingStyleDB asset, like /Game/DB/FightingStyles/SixthStyle.SixthStyle"); ok = 0; }
    else if (!sid || atoi(sid) < 0 || atoi(sid) > 63) { snprintf(err, cap, "\"style_id\" must be a number from 0 to 63 (0-5 are the stock styles, new ones start at 6)"); ok = 0; }
    else if (!ix || atoi(ix) < 5 || atoi(ix) > 15) { snprintf(err, cap, "\"index\" (progression index = button position) must be a number from 5 to 15"); ok = 0; }
    else if (sg_parse_level(cs_str(&d, 0, "gleam_level")) < 0) { snprintf(err, cap, "\"gleam_level\" must be a whole number from 1 to %d (the Gleam level that unlocks the style, Faejin = 6) or absent", SG_MAX_LEVEL); ok = 0; }
    if (ok) cs_warn_unknown(&d, "styles", id ? id : "?", known);
    jdoc_free(&d); return ok;
}
static int styles_child_count(void *tbl) {
    void *c = CA->find_class(CA, "TableWidget"), *fn = c ? CA->find_function_in_class(CA, c, "BPF_GetChildrenCount") : NULL; int n = -1; if (fn) CA->call_function_params(CA, tbl, fn, NULL, &n); return n;
}
// the vanilla menu logic keeps locked styles hit-test-invisible (3) and the row / table self-hit-test-invisible (4): force our buttons and their ancestors up to the table to Visible (0)
static void styles_force_visible(void *u) {
    (void)u;
    for (int s = 0; s < g_styles_n; s++) {
        void *cur = g_styles[s].btn; if (!cur) continue;
        for (int d = 0; d < 8 && cur && cur != g_style_table; d++) {
            unsigned char *v = (unsigned char *)CA->prop_ptr(CA, cur, "Visibility"); if (v && *v) *v = 0;
            void **slot = (void **)CA->prop_ptr(CA, cur, "Slot"); void **pp = slot && *slot ? (void **)CA->prop_ptr(CA, *slot, "Parent") : NULL; cur = pp ? *pp : NULL;
        }
    }
}
// Gleam level of a stats component through the game's own UFunction (m_CurrentGobalStats is not reflected here, and a raw offset read can return another character's values)
static int styles_gleam_call(void *sc, const char *fn) {
    void *f = sc ? CA->find_function(CA, sc, fn) : NULL; uint8_t mode = 0; int r = -1; void *args[1] = { &mode };
    if (!f || !CA->call_function_params(CA, sc, f, args, &r)) return -1; return r;
}
static int styles_gleam_level(void *sc, int unused) { (void)unused; return styles_gleam_call(sc, "BPF_GetResourcesLevel"); }
static unsigned char (*g_o_unl)(void *, int, int, int);          // UStatsComponent::BPF_IsFightingStyleUnlocked(style id, mode, mentor)
static unsigned char styles_unl_det(void *th, int id, int mode, int mentor) {
    unsigned char r = g_o_unl(th, id, mode, mentor);
    for (int s = 0; s < g_styles_n; s++) if (id == g_styles[s].style_id) {
        int gl = -1;
        if (g_styles[s].gleam_level > 0) gl = styles_gleam_level(th, 0);   // Faejin's unlock compares this value (Progression m_Rewards GleamLvl, UStatsComponent::BPF_GetResourcesLevel)
        int game = r; r = (unsigned char)sg_unlocked(g_styles[s].gleam_level, game, gl);
        if (g_styles[s].gleam_level > 0) { static int calls; if (++calls <= 80) logf_("content: style gate call %d: %s/%s (id %d, mode %d, mentor %d, stats %p) unlocked=%d (game answer %d, Gleam level %d, resources %d, needs %d)", calls, g_styles[s].mod, g_styles[s].id, id, mode, mentor, th, r, game, gl, styles_gleam_call(th, "BPF_GetResources"), g_styles[s].gleam_level); }   // diagnostics: the first 80 answers
    }
    return r;
}
static int (*g_o_byidx)(void *, int);                             // UStatsComponent::BPF_GetFightingStyleByUnlockIndex(progression index); UFightingStyleButton::UpdateFS derives id + DB from it
static int styles_byidx_det(void *th, int idx) {
    int r = g_o_byidx(th, idx);
    for (int s = 0; s < g_styles_n; s++) if (idx == g_styles[s].index) r = g_styles[s].style_id;
    return r;
}
static void styles_add_button(void *tbl, CStyle *st, void *bcls, void *pc) {
    void *db = CA->load_object(CA, st->db); logf_("content: style %s: db=%p (%s) children before=%d", st->id, db, st->db, styles_child_count(tbl)); if (!db) return;
    if (st->name[0]) logf_("content:   style %s m_Name '%s' -> %d", st->id, st->name, CA->set_text(CA, db, "m_Name", st->name));
    if (st->icon[0]) logf_("content:   style %s m_StyleSmallIcon %s -> %d", st->id, st->icon, CA->set_property_text(CA, db, "m_StyleSmallIcon", st->icon));
    void *w = CA->create_widget(CA, bcls, pc); if (!w) { logf_("content: style %s: create_widget failed", st->id); return; }
    int *prog = (int *)CA->prop_ptr(CA, w, "m_iFightingStyleProgressionIndex"), *sid = (int *)CA->prop_ptr(CA, w, "m_iFightingStyleID"); void **dbp = (void **)CA->prop_ptr(CA, w, "m_FightingStyleDB");
    if (prog) *prog = st->index; if (sid) *sid = st->style_id; if (dbp) *dbp = db;
    if (st->gleam_level > 0) {          // the lock icon's "lvl N" label: the vanilla button reads it from its `Reward` variable (an index into Progression.m_Rewards; stylegate.h)
        int rw = sg_reward_for_level(st->gleam_level); unsigned char *rp = (unsigned char *)CA->prop_ptr(CA, w, "Reward");
        if (rw >= 0 && rp) { *rp = (unsigned char)rw; logf_("content: style %s: Reward = %d so the locked button shows 'lvl %d'", st->id, rw, st->gleam_level); }
        else logf_("content: style %s: no 'lvl %d' label (reward row %d, Reward property %p): the locked button shows a bare lock", st->id, st->gleam_level, rw, (void *)rp);
    }
    unsigned char *mv = (unsigned char *)CA->prop_ptr(CA, w, "m_eMouseInputModeVisibility"); if (mv) *mv = 0;        // archetype overrides the vanilla buttons have and a fresh widget lacks
    CA->set_bool(CA, w, "bIsFocusable", 1); CA->set_bool(CA, w, "m_bNotifyPawnChanged", 1);
    void *tc = CA->find_class(CA, "TableWidget"), *add = tc ? CA->find_function_in_class(CA, tc, "BPF_AddItem") : NULL; void *args[1] = { &w };
    int ok = add && CA->call_function_params(CA, tbl, add, args, NULL);
    static const char *ev[] = { "BPE_OnSynchronizeProperties", "BPE_OnDataUpdated" };      // the button already ran Construct with empty data: let its own refresh events rebuild icon / name
    for (int i = 0; i < 2; i++) { void *fn = CA->find_function_in_class(CA, bcls, ev[i]); if (fn) CA->call_function_params(CA, w, fn, NULL, NULL); }
    void *osu = CA->find_function_in_class(CA, bcls, "OnStatsUpdated"); unsigned char mode = 0, *margs[1] = { &mode }; if (osu) CA->call_function_params(CA, w, osu, (void **)margs, NULL);
    st->btn = w; logf_("content: style %s: widget %p BPF_AddItem -> %d, children after=%d", st->id, w, ok, styles_child_count(tbl));
}
static void styles_enter_post(void *menu, void *func, void *params, void *u) {
    (void)func; (void)params; (void)u;
    void **tp = (void **)CA->prop_ptr(CA, menu, "FightingStyleTable"), *tbl = tp ? *tp : NULL;
    logf_("content: EnterState FightingStyle on %p, table=%p (previous %p), %d extra style(s)", menu, tbl, g_style_table, g_styles_n);
    if (!tbl || tbl == g_style_table) return;
    void *bcls = CA->find_class(CA, "BP_FightingStyleButton_C"), *pc = CA->get_local_player_controller(CA); if (!bcls || !pc) return;
    for (int s = 0; s < g_styles_n; s++) { g_styles[s].btn = NULL; styles_add_button(tbl, &g_styles[s], bcls, pc); }
    g_style_table = tbl; styles_force_visible(NULL);
}
static int styles_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    CStyle *st = NULL; for (int i = 0; i < g_styles_n; i++) if (!strcmp(g_styles[i].id, id) && !strcmp(g_styles[i].mod, mod)) st = &g_styles[i];
    if (st && g_reloading) {                                          // hot reload: name and icon can change (written into the style DB; the menu shows them the next time Meditation opens); id, index, db cannot
        const char *nd = cs_str(&d, 0, "db"), *nn = cs_str(&d, 0, "name"), *ni = cs_str(&d, 0, "icon"), *ns = cs_str(&d, 0, "style_id"), *nx = cs_str(&d, 0, "index");
        if ((nd && strcmp(nd, st->db)) || (ns && atoi(ns) != st->style_id) || (nx && atoi(nx) != st->index)) { g_reload_blocked++; logf_("content: reload: style %s/%s: db / style_id / index changed - that needs a restart; this entry was not changed", mod, id); jdoc_free(&d); return 0; }
        void *dbo = CA->load_object(CA, st->db);
        if (dbo && nn && strcmp(nn, st->name)) { snprintf(st->name, sizeof st->name, "%s", nn); logf_("content: reload: style %s name '%s' -> %d", id, st->name, CA->set_text(CA, dbo, "m_Name", st->name)); }
        if (dbo && ni && strcmp(ni, st->icon)) { snprintf(st->icon, sizeof st->icon, "%s", ni); logf_("content: reload: style %s icon %s -> %d", id, st->icon, CA->set_property_text(CA, dbo, "m_StyleSmallIcon", st->icon)); }
        jdoc_free(&d); return 0;
    }
    if (!st && !g_reloading) { const char *ns = cs_str(&d, 0, "style_id"); for (int i = 0; ns && i < g_styles_n; i++) if (g_styles[i].style_id == atoi(ns) && strcmp(g_styles[i].mod, mod)) {   // ONE button per style id: a later mod's entry replaces an earlier mod's
        logf_("content: style %s/%s REPLACES %s/%s (same style id %d: one button)", mod, id, g_styles[i].mod, g_styles[i].id, g_styles[i].style_id); st = &g_styles[i]; memset(st, 0, sizeof *st); break; } }
    if (!st) { if (g_styles_n >= 8) { logf_("content: styles: too many styles (8), %s ignored", id); jdoc_free(&d); return 0; } st = &g_styles[g_styles_n++]; memset(st, 0, sizeof *st); }
    snprintf(st->mod, sizeof st->mod, "%s", mod); snprintf(st->id, sizeof st->id, "%s", id); const char *v;
    if ((v = cs_str(&d, 0, "db"))) snprintf(st->db, sizeof st->db, "%s", v);
    if ((v = cs_str(&d, 0, "name"))) snprintf(st->name, sizeof st->name, "%s", v);
    if ((v = cs_str(&d, 0, "icon"))) snprintf(st->icon, sizeof st->icon, "%s", v);
    st->style_id = atoi(cs_str(&d, 0, "style_id")); st->index = atoi(cs_str(&d, 0, "index")); st->gleam_level = sg_parse_level(cs_str(&d, 0, "gleam_level")); jdoc_free(&d);
    logf_("content: style %s/%s: id %d, progression index %d, db %s, unlock %s", mod, id, st->style_id, st->index, st->db, st->gleam_level > 0 ? "at Gleam level (see gleam_level) or the game's own answer" : "always");
    if (!g_styles_hooked) {
        g_styles_hooked = 1;
        g_o_unl = (unsigned char (*)(void *, int, int, int))CA->hook_symbol(CA, "UStatsComponent::BPF_IsFightingStyleUnlocked", (void *)styles_unl_det);
        g_o_byidx = (int (*)(void *, int))CA->hook_symbol(CA, "UStatsComponent::BPF_GetFightingStyleByUnlockIndex", (void *)styles_byidx_det);
        logf_("content: style hooks %p %p, on_call -> %d, timer -> %d", (void *)g_o_unl, (void *)g_o_byidx, CA->on_call(CA, "BP_CharacterMenu_C", "EnterState FightingStyle", NULL, styles_enter_post, NULL), CA->set_timer(CA, 0.5f, 1, styles_force_visible, NULL));
    }
    return 0;
}

#include "powers.inc"                                                // kind "powers" and the power API; builds on the items code above
#include "npcs.inc"                                                  // kind "npcs" and the NPC API (npc_spawn, on_area, ...)
#include "bots.inc"                                                  // kinds "bots" + "ranks" and the progression API (xp_add, rank_info, trial_reward, ...)
#include "hitpre.inc"                                                // kind "poise" and on_hit_pre / damage_mod / poise_set (API 2.19)
#include "overlayapi.inc"                                            // overlay_info / _register / _unregister / _set_mode / _raw_mouse: plugins draw Dear ImGui through the loader's overlay (API 2.21)
#include "defense_ev.inc"                                            // on_defense (API 2.20): guard / guard break / absorb / parry / deflect / dodge / avoid events
#include "faejinesc.inc"                                             // Faejin gold-loop brakes (window shrink, stamina escalation; default OFF, config fe_*)
#include "inputrec.inc"                                              // input_rec: input + state recorder, default off
#include "botai.inc"                                                 // scripted bot, default off
#include "clone.inc"                                                 // kind "clones": player clones = second local players with their own character, driven by a botai brain
#include "animsets.inc"                                              // kind "anim_sets": runtime animation sets (padded style tables + wired clips)

static int grants_warn_line(int k, char *out, int cap) {
    if (!g_gp_world_ok) return 0;
    for (int i = 0; i < g_items_n; i++) { CItem *it = &g_items[i]; if (it->grant && !it->granted && it->gtries >= 4 && k-- == 0) { snprintf(out, cap, "grant %s/%s NOT done yet: %s (%d tries; content: lines in lannative.log)", it->mod, it->id, gp_why(it->gwhy), it->gtries); return 1; } }
    for (int i = 0; i < g_pw_n; i++) { CPower *p = &g_pw[i]; if (p->grant && !p->granted && p->gtries >= 4 && k-- == 0) { snprintf(out, cap, "power grant %s/%s NOT done yet: %s (%d tries; powers: lines in lannative.log)", p->mod, p->id, gp_why(p->gwhy), p->gtries); return 1; } }
    return 0;
}

// ================================================================ the loader's own plugin
static void content_ready(void *u) { (void)u; content_process("engine ready"); }
static void content_saveload(void *save, void *u) { (void)u; logf_("content: save %p is about to resolve its item paths", save); content_process("before the save resolves its items"); grants_rearm(); }
static void content_init(const AbsApi *a) {
    CA = a; int p = plug_of(a);
    ck_add(p, "maps", ms_validate, maps_apply, NULL);                 // mapscript.inc (+ "menu" travel item: bottrial.inc)
    ck_add(p, "items", items_validate, items_apply, NULL); ck_add(p, "styles", styles_validate, styles_apply, NULL); ck_add(p, "powers", powers_validate, powers_apply, NULL); ck_add(p, "npcs", npcs_validate, npcs_apply, NULL); ck_add(p, "bots", bots_validate, bots_apply, NULL); ck_add(p, "ranks", ranks_validate, ranks_apply, NULL); ck_add(p, "bot_trials", trials_validate, trials_apply, NULL); ck_add(p, "poise", hp_poise_validate, hp_poise_apply, NULL); hp_init(a); de_init(a); fe_init(a); ir_init(a); bai_init(a); ck_add(p, "clones", clones_validate, clones_apply, NULL); cl_init(a);
    ck_add(p, "anim_sets", as_kind_validate, as_kind_apply, NULL);
    ck_add(p, "weapons", wp_kind_validate, wp_kind_apply, NULL);     // weapons.inc
    learn_install();
    a->on_engine_ready(a, content_ready, NULL);
    logf_("content: on_save_loading -> %d, on_world_loaded -> %d", a->on_save_loading(a, content_saveload, NULL), a->on_world_loaded(a, items_world, NULL));
}

// ================================================================ hot reload
// Re-lists Mods\<mod>\content of every mod that loaded, re-validates, and applies what is new or changed (docs\PLUGIN-API.md "Mods page, menu items and hot reload"). Per kind.
//   items   name, description, icon / icon_file, props, price, gear_row.set values, grant: re-applied to the live object (a changed icon_file image becomes a NEW texture object);
//           base, shop, shop_copy_of, gear_row.table / copy_of, moves cannot change (the object / rows exist): logged, entry kept as it was. A NEW item is created like at boot.
//   styles  name and icon are written into the style DB (shown the next time Meditation opens); db, style_id, index cannot change. A NEW style appears when the Meditation menu is built again.
//   other kinds (built-in and plugin)  `apply` is called again for every changed or new entry (make it idempotent); removed entries are only reported.
// Removed entries and removed properties stay in the game until a restart (the old values are not kept). Returns the number of changed + new entries applied, -1 if the registry is not running.
static int content_reload(char *status, int cap) {
    if (!CA) { snprintf(status, cap, "content: not running (no mod shipped content when the game started - restart once)"); return -1; }
    static CrSnap old[512]; static char *old_json[512]; int nold = 0;
    for (int i = 0; i < g_ce_n; i++) {                                                              // what was valid and in use before (json strings are taken over, freed below)
        CEntry *e = &g_ce[i];
        if (e->valid && !e->shadowed && e->json && nold < 512) { CrSnap *o = &old[nold]; snprintf(o->kind, sizeof o->kind, "%s", e->cr.kind); snprintf(o->id, sizeof o->id, "%s", e->cr.id); snprintf(o->mod, sizeof o->mod, "%s", e->modid); o->json = e->json; o->applied = e->applied; old_json[nold++] = e->json; }
        else free(e->json);
        e->json = NULL;
    }
    g_cf_n = 0;
    for (int pos = 0; pos < MODS_MAX; pos++) {                                                        // the mods that loaded, in load order
        int m = -1; for (int i = 0; i < g_mi_n; i++) if (g_mi_pos[i] == pos) m = i;
        if (m < 0) continue; uint64_t h = 0; content_scan_mod(pos, g_mi[m].name, g_mi[m].id, g_mi_dir[m], &h);
    }
    g_ce_n = 0; g_collected = 0; content_collect();
    static CrSnap now_[512]; int nnow = 0; int changed = 0, fresh = 0, removed = 0, retry = 0;
    for (int i = 0; i < g_ce_n; i++) {
        CEntry *e = &g_ce[i]; if (!e->valid || e->shadowed) continue;
        CrSnap *c = &now_[nnow < 512 ? nnow : 511]; snprintf(c->kind, sizeof c->kind, "%s", e->cr.kind); snprintf(c->id, sizeof c->id, "%s", e->cr.id); snprintf(c->mod, sizeof c->mod, "%s", e->modid); c->json = e->json; c->applied = 0; if (nnow < 512) nnow++;
        switch (cr_reload_class(old, nold, c)) {
            case CRR_SAME: e->applied = 1; break;
            case CRR_CHANGED: changed++; logf_("content: reload: %s '%s' (%s) changed", e->cr.kind, e->cr.id, e->rel); break;
            case CRR_RETRY: retry++; break;                                                       // was waiting for an object / table: tried again now
            default: fresh++; logf_("content: reload: %s '%s' (%s) is new", e->cr.kind, e->cr.id, e->rel); break;
        }
    }
    { int gone[64]; int ng = cr_reload_removed(old, nold, now_, nnow, gone, 64); for (int k = 0; k < ng; k++) { removed++; logf_("content: reload: %s '%s' (%s) is gone from disk - it stays in the game until a restart", old[gone[k]].kind, old[gone[k]].id, old[gone[k]].mod); } }
    for (int k = 0; k < nold; k++) free(old_json[k]);
    g_reloading = 1; g_reload_blocked = 0; content_process("hot reload"); g_reloading = 0;
    snprintf(status, cap, "content: %d changed, %d new%s%s%s", changed, fresh, retry ? ", retried some" : "", g_reload_blocked ? "" : "", "");
    if (g_reload_blocked) { size_t n = strlen(status); snprintf(status + n, cap - (int)n, ", %d NEED A RESTART", g_reload_blocked); }
    if (removed) { size_t n = strlen(status); snprintf(status + n, cap - (int)n, ", %d removed (stay until restart)", removed); }
    logf_("%s", status);
    return changed + fresh;
}

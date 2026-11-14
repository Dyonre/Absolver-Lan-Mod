// animsets.inc - kind "anim_sets": an animation set lives at its own index of the 66 style-indexed tables (animsets.h, the pure
// part: path table, allocator, padding, validator). Included by content.inc after the "styles" kind.
//   content\anim_sets\*.json: { "anim_sets": [ { "id": "Staff", "index": "auto" | 6, "fallback_style": 0,
//                                                "slots": [ { "table": "HittedDB.m_Anims", "arr": 0 | "*", "at": ["*", 1, 0, 1], "anim": "/Game/.../Clip.Clip" }, ... ] } ] }
//   "index": "auto" = hidden set (never selectable, never saved: the next free index past every real style); a number = a real style that already has its
//   m_FightingStyles entry. "fallback_style" (0..5) = the stock style whose block the set's index starts as. "at" = one index per inner dimension of a style
//   element ("*" = all), "arr" = the property's array (GuardDB family*4+quadrant, DodgeDB weight). Clips keep the fallback block's m_bMirror / m_SpeedPerFrame.
// Boot (one game-thread tick after the engine-ready content pass, before the first world): find (load) the 8 DB objects, root them, check the reflected
// property offsets / sizes against animsets.h (refuse on mismatch), base = max(6, styles in the character BPs / "styles" kind / the longest table),
// as_apply (allocate, validate, pad with FMemory::Malloc, validate again), then wire the clips and dump the wired slots. Every world load re-checks the
// objects and lengths (re-applies if the engine reloaded a table). An index is handed out (as_set_index) only for an accepted set whose tables hold it;
// animstyle.inc refuses any anim_style_set past the stock styles unless the tables hold that index (as_tables_hold): the unchecked HittedDB.m_HandPoses
// read can never go out of bounds through the loader.
// Config (LanNative\config.txt): anim_sets = on (default) | off (no padding / wiring: items fall back to their numeric anim_style);
// anim_sets_dump = <index>: also log every set's slots at that index;
// anim_sets_check = on: after each world load force a garbage collection and read every wired slot back (diagnostic);
// anim_sets_quit = <seconds>: console "quit" that long after the first world load (diagnostic: the engine must free the padded buffers at exit).
#include "animsets.h"
#define ASD_MAX_SLOTS 128
typedef struct { AsWire w; char anim[200]; void *clip; } AsSlotDef;
typedef struct { AsSet s; char mod[MOD_NAME_MAX]; AsSlotDef *sl; int nsl, wired, missing; } AsDef;
static AsDef g_asd[AS_MAX_SETS]; static int g_asd_n;
static void *g_aso[AS_NTABLES];                                        // the table objects (rooted at boot)
static int g_as_mode = -1, g_as_booted, g_as_need, g_as_base, g_as_dump = -1, g_as_check, g_as_sched, g_as_quit, g_as_worlds;
static unsigned g_as_hold_gen = ~0u; static int g_as_hold_style = -1, g_as_hold_ok; static char g_as_hold_why[240];

static void *as_fmalloc(size_t n) { return ((void *(*)(size_t, unsigned))(g_base + RVA_FMEMORY_MALLOC))(n, 0); }   // the engine's allocator: TArray frees it at exit / GC
static void as_logline(const char *l) { logf_("%s", l); }

// ----- JSON
static int as_parse(const char *json, AsDef *out, char *err, int cap) {
    JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    static const char *const known[] = { "id", "index", "fallback_style", "slots", NULL };
    const char *id = cs_str(&d, 0, "id"), *ix = cs_str(&d, 0, "index"), *fb = cs_str(&d, 0, "fallback_style"); int ok = 0, n = 0;
    int sl = jfind(&d, 0, "slots");
    if (!ix || (_stricmp(ix, "auto") && (atoi(ix) < AS_STOCK || atoi(ix) > AS_IDX_MAX))) snprintf(err, cap, "\"index\" must be \"auto\" (a hidden set) or a real style index %d..%d", AS_STOCK, AS_IDX_MAX);
    else if (!fb || atoi(fb) < 0 || atoi(fb) >= AS_STOCK || (fb[0] < '0' || fb[0] > '9')) snprintf(err, cap, "\"fallback_style\" must be a stock style 0..5");
    else if (sl < 0 || d.n[sl].t != J_ARR) snprintf(err, cap, "needs \"slots\": an array of { table, arr, at, anim }");
    else {
        ok = 1;
        for (int e = d.n[sl].first; e >= 0 && ok; e = d.n[e].next, n++) {
            if (n >= ASD_MAX_SLOTS) { snprintf(err, cap, "more than %d slot entries", ASD_MAX_SLOTS); ok = 0; break; }
            if (d.n[e].t != J_OBJ) { snprintf(err, cap, "slot %d is not an object", n + 1); ok = 0; break; }
            const char *tb = cs_str(&d, e, "table"), *an = cs_str(&d, e, "anim"), *ar = cs_str(&d, e, "arr"); int at = jfind(&d, e, "at");
            AsWire w; memset(&w, 0, sizeof w); w.t = as_table_find(tb);
            if (w.t < 0) { snprintf(err, cap, "slot %d: unknown table '%s' (expected Asset.property)", n + 1, tb ? tb : "?"); ok = 0; break; }
            const AsTable *t = &AS_TABLES[w.t];
            w.a = !ar ? 0 : !strcmp(ar, "*") ? -1 : atoi(ar);
            if (!an || strncmp(an, "/Game/", 6) || !strchr(an, '.') || strlen(an) >= sizeof out->sl[0].anim) { snprintf(err, cap, "slot %d: \"anim\" must be an object path /Game/.../Name.Name", n + 1); ok = 0; break; }
            int k = 0;
            if (at >= 0 && d.n[at].t == J_ARR) for (int c = d.n[at].first; c >= 0; c = d.n[c].next, k++) {
                if (k >= 4) break;
                const char *v = d.n[c].str; w.ix[k] = v && !strcmp(v, "*") ? -1 : v ? atoi(v) : -2;
            }
            if (k != t->ndi) { snprintf(err, cap, "slot %d: %s needs \"at\" with %d index(es), got %d", n + 1, tb, t->ndi, k); ok = 0; break; }
            char why[120]; if (!as_wire_check(&w, why, sizeof why)) { snprintf(err, cap, "slot %d: %s", n + 1, why); ok = 0; break; }
            if (out) { AsSlotDef *s = &out->sl[n]; s->w = w; snprintf(s->anim, sizeof s->anim, "%s", an); s->clip = NULL; }
        }
        if (ok && !n) { snprintf(err, cap, "\"slots\" is empty"); ok = 0; }
    }
    if (ok && out) {
        memset(&out->s, 0, sizeof out->s); snprintf(out->s.id, sizeof out->s.id, "%s", id ? id : "?");
        out->s.fixed = _stricmp(ix, "auto") ? atoi(ix) : -1; out->s.fallback = atoi(fb); out->s.index = -1; out->nsl = n;
    }
    if (ok) cs_warn_unknown(&d, "anim_sets", id ? id : "?", known);
    jdoc_free(&d); return ok;
}
static int as_kind_validate(const char *json, char *err, int cap, void *u) { (void)u; return as_parse(json, NULL, err, cap); }

// ----- the table objects
static int as_find_tables(int load, void **objs) {
    int missing = 0;
    for (int i = 0; i < AS_NTABLES; i++) {
        objs[i] = CA->find_object(CA, AS_TABLES[i].asset);
        if (!objs[i] && load) { objs[i] = CA->load_object(CA, AS_TABLES[i].asset); if (objs[i] && (i == 0 || strcmp(AS_TABLES[i].asset, AS_TABLES[i - 1].asset))) logf_("animsets: loaded %s -> %p", AS_TABLES[i].asset, objs[i]); }
        if (!objs[i]) missing++;
    }
    return missing;
}
// the engine's property offset and size (ElementSize x ArrayDim) must be the SDK values of the path table (another game build refuses everything)
static int as_layout_ok(void **objs, char *err, int cap) {
    for (int i = 0; i < AS_NTABLES; i++) {
        const AsTable *t = &AS_TABLES[i]; void *cls = CA->object_class(CA, objs[i]), *p = cls ? CA->find_property(CA, cls, t->prop) : NULL;
        int off = p ? CA->property_offset(CA, p) : -1, size = -1;
        __try { if (p) size = *(int32_t *)((uint8_t *)p + 0x30) * *(int32_t *)((uint8_t *)p + 0x34); } __except (EXCEPTION_EXECUTE_HANDLER) { size = -1; }
        if (!as_check_layout(t, off, size)) { snprintf(err, cap, "%s.%s: engine offset 0x%X size %d, the path table says 0x%X size %d", t->asset, t->prop, off, size, t->off, 16 * t->narr); return 0; }
    }
    return 1;
}
// hidden sets start past every real style: the character BPs' m_FightingStyles, the "styles" kind, and any table a pak already made longer
static int as_base_now(void **objs) {
    int b = AS_STOCK, bp = 0, sk = 0, tb = 0; static const char *const bps[] = { "BP_TPSCharacter_C", "BP_MenuCharacter_C", "BP_StartUpCharacter_C" };
    for (int i = 0; i < 3; i++) { void *cdo = CA->find_cdo(CA, bps[i]), *dc = cdo ? cd_comp(cdo, "m_DefenseComponent") : NULL; AsArr *fs = dc ? api_p_prop_ptr(NULL, dc, "m_FightingStyles") : NULL;
        if (fs && fs->num > 0 && fs->num <= AS_IDX_MAX && fs->num > bp) bp = fs->num; logf_("animsets: base: %s CDO %p defense %p m_FightingStyles %d", bps[i], cdo, dc, fs ? fs->num : -1); }
    for (int s = 0; s < g_styles_n; s++) if (g_styles[s].style_id + 1 > sk) sk = g_styles[s].style_id + 1;
    for (int i = 0; i < AS_NTABLES; i++) for (int a = 0; a < AS_TABLES[i].narr; a++) { int n = as_arr(&AS_TABLES[i], objs[i], a)->num; if (n > tb && n <= AS_IDX_MAX + 1) tb = n; }
    if (bp > b) b = bp; if (sk > b) b = sk; if (tb > b) b = tb;
    logf_("animsets: base index %d (stock %d, character BPs %d, styles kind %d, longest table %d)", b, AS_STOCK, bp, sk, tb);
    return b;
}

// ----- slots
static void as_slot_name(const AsSlotDef *s, int a, long off, char *out, int cap) {
    const AsTable *t = &AS_TABLES[s->w.t]; const char *b = strrchr(t->asset, '.'); snprintf(out, cap, "%s.%s[%d]+0x%lX", b ? b + 1 : t->asset, t->prop, a, off);
}
// reads every slot of set d at `style`. log_each: one line per slot. Returns the number of slots that hold the set's own (loaded) clip; *total = all slots
static int as_read_set(const AsDef *d, void **objs, int style, int log_each, const char *why, int *total) {
    int same = 0, all = 0, oob = 0;
    for (int i = 0; i < d->nsl; i++) {
        const AsSlotDef *s = &d->sl[i]; const AsTable *t = &AS_TABLES[s->w.t]; int arrs[64]; long offs[64];
        int n = as_wire_slots(&s->w, style, arrs, offs, 64); if (n > 64) n = 64;
        for (int k = 0; k < n; k++, all++) {
            AsArr *arr = objs[s->w.t] ? as_arr(t, objs[s->w.t], arrs[k]) : NULL; char path[200] = "?", nm[120];
            void *p = NULL; int ok = arr && offs[k] + as_leaf_size(t->leaf) <= (long)arr->num * (long)t->elem;
            if (!ok) { oob++; snprintf(path, sizeof path, "(past the table: %d entries)", arr ? arr->num : -1); }
            else { __try { p = *(void **)((uint8_t *)arr->data + offs[k]); if (p) se_path_of(p, path, sizeof path); else snprintf(path, sizeof path, "None"); } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(path, sizeof path, "(unreadable)"); } }
            int mine = ok && p && s->clip && p == s->clip && !strcmp(path, s->anim); same += mine;
            if (log_each) { as_slot_name(s, arrs[k], offs[k], nm, sizeof nm); logf_("animsets: dump %s @%d %s = %s%s", d->s.id, style, nm, path, s->clip && ok && !mine ? "  (NOT the set's clip)" : ""); }
        }
    }
    logf_("animsets: %s @%d (%s): %d of %d slot(s) hold the set's clip%s", d->s.id, style, why, same, all, oob ? " - SOME PAST THE TABLE" : "");
    if (total) *total = all;
    return same;
}
// a clip that is not loaded and not in any pak is never loaded (that would hang); such slots keep the fallback clip (warning)
static void as_wire(AsDef *d, void **objs) {
    d->wired = d->missing = 0; int bad = 0;
    for (int i = 0; i < d->nsl; i++) {
        AsSlotDef *s = &d->sl[i]; const AsTable *t = &AS_TABLES[s->w.t]; char why[240];
        if (!s->clip) {
            if (!rows_objref_ok("Object", s->anim, why, sizeof why)) { logf_("animsets: %s: WARNING %s - %s keeps the fallback clip", d->s.id, why, t->prop); d->missing++; continue; }
            void *c = CA->load_object(CA, s->anim); int isa = c && CA->is_a(CA, c, "AnimSequence");
            if (!isa) { logf_("animsets: %s: WARNING %s %s - slot keeps the fallback clip", d->s.id, s->anim, c ? "is not an AnimSequence" : "did not load"); d->missing++; continue; }
            s->clip = c;
        }
        int arrs[64]; long offs[64]; int n = as_wire_slots(&s->w, d->s.index, arrs, offs, 64); if (n > 64) n = 64;
        for (int k = 0; k < n; k++) {
            AsArr *arr = as_arr(t, objs[s->w.t], arrs[k]);
            if (offs[k] + as_leaf_size(t->leaf) > (long)arr->num * (long)t->elem) { bad++; continue; }   // cannot happen after as_apply; never write past a buffer
            *(void **)((uint8_t *)arr->data + offs[k]) = s->clip; d->wired++;                         // the leaf's first 8 bytes: FAnimContainer.m_Anim / UAnimSequence*
        }
    }
    logf_("animsets: %s -> index %d: %d slot(s) wired from %d entr%s, %d clip(s) missing (fallback kept)%s", d->s.id, d->s.index, d->wired, d->nsl, d->nsl == 1 ? "y" : "ies", d->missing, bad ? " - SLOTS PAST THE TABLE SKIPPED" : "");
}

// ----- boot / world load
static int as_run(void) {                                              // allocate + pad + wire on g_aso; 0 = nothing accepted
    AsSet s[AS_MAX_SETS]; for (int i = 0; i < g_asd_n; i++) s[i] = g_asd[i].s;
    g_as_base = as_base_now(g_aso);
    int acc = as_apply(g_aso, s, g_asd_n, g_as_base, as_fmalloc, as_logline), src[AS_IDX_MAX + 1];
    g_as_need = acc ? as_need(s, g_asd_n, src) : 0;
    for (int i = 0; i < g_asd_n; i++) { g_asd[i].s = s[i]; if (s[i].ok) as_wire(&g_asd[i], g_aso); }
    g_as_hold_gen = ~0u; return acc;
}
static void as_gc_verify(void *u) {
    (void)u;
    for (int i = 0; i < g_asd_n; i++) if (g_asd[i].s.ok) { int tot = 0, n = as_read_set(&g_asd[i], g_aso, g_asd[i].s.index, 0, "after a forced garbage collection", &tot);
        logf_("animsets: GC check %s: %s", g_asd[i].s.id, n == g_asd[i].wired && g_asd[i].wired > 0 ?"PASS (every wired slot still holds its clip)" : "FAIL"); }
}
static void as_gc_force(void *u) {
    (void)u; void *cdo = CA->find_cdo(CA, "KismetSystemLibrary"), *cls = cdo ? CA->object_class(CA, cdo) : NULL, *fn = cls ? CA->find_function_in_class(CA, cls, "CollectGarbage") : NULL;
    int ok = fn && CA->call_function_params(CA, cdo, fn, NULL, NULL);
    logf_("animsets: GC check: KismetSystemLibrary::CollectGarbage -> %d (runs at the end of this frame); reading the slots back in 3 s", ok);
    CA->set_timer(CA, 3.0f, 0, as_gc_verify, NULL);
}
static void as_quit(void *u) { (void)u; logf_("animsets: quit test: sending the console command quit (a clean exit frees every TArray, our FMemory buffers included)"); md_console("quit"); }
static void as_world(const char *map, void *u) {
    (void)u; if (!g_as_booted || g_as_mode != 1 || !g_as_need) return;
    void *now[AS_NTABLES]; int missing = as_find_tables(0, now), moved = 0; char err[240] = "";
    for (int i = 0; i < AS_NTABLES; i++) if (now[i] != g_aso[i]) moved++;
    int bad = moved ? 0 : as_validate(g_aso, g_as_need, err, sizeof err);
    if (!moved && !bad) logf_("animsets: world %s: the 8 table objects are unchanged and hold %d entries", map ? map : "?", g_as_need);
    else {
        logf_("animsets: world %s: %s - padding and wiring again", map ? map : "?", moved ? "a table object changed (the engine reloaded it)" : err);
        if (missing) { logf_("animsets: %d table object(s) not loaded: every set refused, the local override removed", missing); for (int i = 0; i < g_asd_n; i++) { g_asd[i].s.ok = 0; g_asd[i].s.index = -1; } g_as_need = 0; as_set_ex(CA, NULL, -1, -1); return; }
        memcpy(g_aso, now, sizeof g_aso); for (int i = 0; i < AS_NTABLES; i++) CA->add_to_root(CA, g_aso[i], 1);
        if (!as_run()) as_set_ex(CA, NULL, -1, -1);
    }
    for (int i = 0; i < g_asd_n; i++) if (g_asd[i].s.ok) as_read_set(&g_asd[i], g_aso, g_asd[i].s.index, 0, "world load", NULL);
    if (g_as_check) CA->set_timer(CA, 5.0f, 0, as_gc_force, NULL);
    if (g_as_quit > 0 && !g_as_worlds++) { logf_("animsets: quit test: console quit in %d s", g_as_quit); CA->set_timer(CA, (float)g_as_quit, 0, as_quit, NULL); }
}
static void as_boot(void *u) {
    (void)u; g_as_booted = 1; char v[32] = ""; wchar_t src[MAX_PATH];
    g_as_mode = !(setting_get("anim_sets", L"animsets.txt", 0, v, sizeof v, src) && !_strnicmp(v, "off", 3));
    if (setting_get("anim_sets_dump", L"animsets_dump.txt", 0, v, sizeof v, src) && v[0] >= '0' && v[0] <= '9') g_as_dump = atoi(v);
    g_as_check = setting_get("anim_sets_check", L"animsets_check.txt", 0, v, sizeof v, src) && !_strnicmp(v, "on", 2);
    if (setting_get("anim_sets_quit", L"animsets_quit.txt", 0, v, sizeof v, src)) g_as_quit = atoi(v);
    logf_("animsets: %d set(s) defined; mode %s, dump index %d, GC check %s, quit test %d s (%ls)", g_asd_n, g_as_mode ? "ON (runtime tables)" : "OFF (no padding / wiring: items use their numeric anim_style, the old superset pak)", g_as_dump, g_as_check ? "on" : "off", g_as_quit, src);
    int missing = as_find_tables(1, g_aso); char err[240];
    if (g_as_mode) {
        if (missing) logf_("animsets: %d table object(s) did not load: ALL sets refused (vanilla animations stay)", missing);
        else if (!as_layout_ok(g_aso, err, sizeof err)) logf_("animsets: layout check (R2) FAILED, ALL sets refused (another game build?): %s", err);
        else {
            for (int i = 0; i < AS_NTABLES; i++) if (i == 0 || g_aso[i] != g_aso[i - 1]) CA->add_to_root(CA, g_aso[i], 1);   // never unloaded / reloaded: the padding stays
            logf_("animsets: layout check (R2) ok: 14 properties at their SDK offsets and sizes; the 8 objects are rooted");
            if (as_run()) for (int i = 0; i < g_asd_n; i++) if (g_asd[i].s.ok) as_read_set(&g_asd[i], g_aso, g_asd[i].s.index, 1, "after wiring", NULL);
            if (!CA->on_world_loaded(CA, as_world, NULL)) logf_("animsets: world-load re-check registered");
        }
    }
    if (g_as_dump >= 0 && !missing) for (int i = 0; i < g_asd_n; i++) as_read_set(&g_asd[i], g_aso, g_as_dump, 1, "anim_sets_dump", NULL);
}
static int as_kind_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u;
    if (g_as_booted) { logf_("content: anim_sets %s/%s: sets are applied once at boot - a change needs a restart", mod, id); return 0; }
    AsDef *d = NULL; for (int i = 0; i < g_asd_n; i++) if (!strcmp(g_asd[i].s.id, id)) d = &g_asd[i];
    if (!d) { if (g_asd_n >= AS_MAX_SETS) { logf_("content: anim_sets: more than %d sets, %s ignored", AS_MAX_SETS, id); return 0; } d = &g_asd[g_asd_n++]; memset(d, 0, sizeof *d); }
    if (!d->sl) d->sl = calloc(ASD_MAX_SLOTS, sizeof *d->sl);
    char err[200]; if (!d->sl || !as_parse(json, d, err, sizeof err)) { logf_("content: anim_sets %s/%s: %s", mod, id, d->sl ? err : "out of memory"); g_asd_n--; return 0; }
    snprintf(d->mod, sizeof d->mod, "%s", mod); snprintf(d->s.id, sizeof d->s.id, "%s", id);
    logf_("content: anim_sets %s/%s: index %s, fallback style %d, %d slot entr%s", mod, id, d->s.fixed >= 0 ? "fixed" : "auto", d->s.fallback, d->nsl, d->nsl == 1 ? "y" : "ies");
    if (!g_as_sched) { g_as_sched = 1; CA->set_timer(CA, 0.01f, 0, as_boot, NULL); }   // next game-thread tick: after this content pass, before the first world
    return 0;
}

// ----- lookups for weaponslot.inc / animstyle.inc
// the table index of an accepted set, -1 if unknown / refused / mode off / its tables no longer hold it
static int as_set_index(const char *id) {
    if (g_as_mode != 1 || !id) return -1;
    for (int i = 0; i < g_asd_n; i++) if (!strcmp(g_asd[i].s.id, id)) {
        if (!g_asd[i].s.ok) { logf_("animsets: set %s was refused (%s)", id, g_asd[i].s.why[0] ? g_asd[i].s.why : "see the boot lines"); return -1; }
        return g_asd[i].s.index;
    }
    return -1;
}
// does every style table hold `style` (and the unchecked HandPoses table cover its gate)? Stock styles always. Cached per world and style.
static int as_tables_hold(int style, char *why, int cap) {
    if (style < AS_STOCK) return 1;
    if (g_as_hold_gen != g_se_world_gen || g_as_hold_style != style) {
        void *objs[AS_NTABLES]; int miss = as_find_tables(0, objs);
        g_as_hold_ok = !miss && as_validate(objs, style + 1, g_as_hold_why, sizeof g_as_hold_why) == 0;
        if (miss) snprintf(g_as_hold_why, sizeof g_as_hold_why, "%d table object(s) not loaded", miss);
        g_as_hold_gen = g_se_world_gen; g_as_hold_style = style;
        logf_("animsets: tables %s style index %d%s%s", g_as_hold_ok ? "hold" : "do NOT hold", style, g_as_hold_ok ? "" : ": ", g_as_hold_ok ? "" : g_as_hold_why);
    }
    if (!g_as_hold_ok) snprintf(why, cap, "%s", g_as_hold_why);
    return g_as_hold_ok;
}

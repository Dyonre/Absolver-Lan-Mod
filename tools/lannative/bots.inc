// bots.inc - bots + ranks (included at the end of content.inc, after npcs.inc). API: docs\PLUGIN-API.md "Bots and ranks (v2.15)".
// Part 1: content kind "bots" = an npcs entry (npcs.inc does the archetype copy, spawn point, aggro, reactions; spawn with npc_spawn("<id>")) built from
//   template (default the unused dev bot AI_Classic_Hard) + a DECK (bot_decks / decks_json shape, inline or a file in the mod) + an AI PROFILE
//   (vanilla / classic_hard, bots.h tables; a local add-on may add more) + ai_level (initial difficulty 0..100). Every write is read back and logged.
// Part 2: content kind "ranks" (reward tables) and the progression API v2.15: xp_add, gm_points_add, rank_info, trial_reward, npc_difficulty.
#include "bots.h"
#include "ranks.h"

// ================================================================ helpers: element-aware property text ("name" or "name[i]" of a static array)
static uint8_t *bt_prop(void *obj, const char *name, int *ix, uint8_t **val) {
    char base[64]; *ix = bt_split_index(name, base, sizeof base); if (*ix == -2 || !obj) return NULL;
    uint8_t *pr = api_p_find_property(CA, *(void **)((uint8_t *)obj + 0x10), base); if (!pr) return NULL;
    int dim = *(int *)(pr + 0x30), i = *ix < 0 ? 0 : *ix; if (i >= dim) return NULL;
    *val = (uint8_t *)obj + *(int *)(pr + 0x44) + i * *(int *)(pr + 0x34); return pr;
}
static int bt_set(void *obj, const char *name, const char *text) {
    int ix, ok = 0; uint8_t *val; wchar_t *w = utf8_to_wide(text); if (!w) return 0;
    __try { uint8_t *pr = bt_prop(obj, name, &ix, &val); if (pr) ok = prop_import_w(pr, val, obj, w); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(w); return ok;
}
static int bt_get(void *obj, const char *name, char *out, int cap) {
    int ix, r = -1; uint8_t *val; out[0] = 0;
    __try { uint8_t *pr = bt_prop(obj, name, &ix, &val); wchar_t *t = pr ? prop_export_w(pr, val, obj) : NULL; if (t) { int n = WideCharToMultiByte(CP_UTF8, 0, t, -1, out, cap, NULL, NULL); r = n > 0 ? n - 1 : -1; free(t); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
// write + read back; counts into ok / bad
static void bt_write(const char *id, const char *what, void *obj, const char *k, const char *v, int *ok, int *bad) {
    char back[600]; int r = bt_set(obj, k, v); bt_get(obj, k, back, sizeof back); int same = r && bt_text_same(v, back);
    if (same) (*ok)++; else (*bad)++;
    logf_("bots: %s: %s %s = %s -> set %d, reads back '%.160s'%s", id, what, k, v, r, back, same ? "" : "  <-- DIFFERS");
}

// ================================================================ part 1: content kind "bots"
#define BT_TEMPLATE "/Game/DB/AI/Bots/AI_Classic_Hard.AI_Classic_Hard"
static const char *const BT_KNOWN[] = { "id", "name", "template", "profile", "ai_level", "ai_level_fixed", "deck", "mirror", "style", "health", "hp", "aggro", "faction", "spawn", "props",
                                        "on_health_below", "on_death", "on_area", "on_player_near", "rank", "tags", "resilience",
                                        "stats", "outfit", "see_forever", "deck_sword", "intro", "brain", NULL };
// stats (guide build, applied to the spawned pawn's StatsComponent), intro (the 1v1 intro row the bot plays), the archetype it runs (to find it from a pawn)
typedef struct { char mod[MOD_NAME_MAX], id[CR_ID_MAX]; int profile, level, mirror, style, moves, will, has_stats, stat_pts[5]; char deck_txt[1200], intro[32]; void *arch; } CBot;
static CBot g_bt[32]; static int g_bt_n;
static void (*g_bt_entry_cb)(const char *mod, const char *id, const char *json);   // botai.inc: every applied bots entry (its "brain" preset); NULL = none
static void (*g_bt_spawn_cb)(void *ch, const char *mod, const char *id);           // botai.inc: a bots entry's pawn was spawned (called at the spawn and again 1.5 s later)

// a deck object: bot_decks shape {"weapon_slot":0,"rows":{"FR":[..],"FL":..,"BR":..,"BL":..},"alternates":{"FR":"170",..}}
// or decks_json shape {"rows":[{"start":"FR","chain":[{"id":"197"},..],"alternate":{"id":"083"}},..]}. Returns 1 or 0 + err.
static int bt_add_row(BtDeck *k, int q, const char *s, char *err, int cap) {
    if (!s || !np_row_ok(s)) { snprintf(err, cap, "deck: attack rows are short names like \"078\""); return 0; }
    if (k->n[q] >= BT_ROW_MAX) { snprintf(err, cap, "deck: a row holds at most %d moves", BT_ROW_MAX); return 0; }
    snprintf(k->rows[q][k->n[q]++], 16, "%s", s); k->moves++; return 1;
}
static int bt_read_deck(const JDoc *d, int node, BtDeck *k, int slot, char *err, int cap) {     // slot 0 = "deck" (bare hands), 1 = "deck_sword"
    memset(k, 0, sizeof *k); const char *v; static const char *const wn[2] = { "barehands", "sword" };
    if (d->n[node].t != J_OBJ) { snprintf(err, cap, "\"%s\" must be an object (a bot_decks deck, or {\"file\": \"decks/x.json\"})", slot ? "deck_sword" : "deck"); return 0; }
    if ((v = cs_str(d, node, "weapon_slot")) && atoi(v) != slot) { snprintf(err, cap, "%s: weapon_slot %s - \"deck\" takes bare-hands decks (0), \"deck_sword\" sword decks (1); war gloves are not built", slot ? "deck_sword" : "deck", v); return 0; }
    if ((v = cs_str(d, node, "weapon")) && _stricmp(v, wn[slot])) { snprintf(err, cap, "%s: weapon '%s' - expected %s", slot ? "deck_sword" : "deck", v, wn[slot]); return 0; }
    int rows = jfind(d, node, "rows"), alts = jfind(d, node, "alternates");
    if (rows < 0) { snprintf(err, cap, "deck: needs \"rows\""); return 0; }
    if (d->n[rows].t == J_OBJ) {
        for (int c = d->n[rows].first; c >= 0; c = d->n[c].next) {
            int q = bt_quad(d->n[c].key); if (q < 0 || d->n[c].t != J_ARR) { snprintf(err, cap, "deck.rows: keys are FR, FL, BR, BL with arrays of attack rows"); return 0; }
            for (int r = d->n[c].first; r >= 0; r = d->n[r].next) if (!bt_add_row(k, q, d->n[r].t == J_STR ? d->n[r].str : NULL, err, cap)) return 0;
        }
        if (alts >= 0) {
            if (d->n[alts].t != J_OBJ) { snprintf(err, cap, "deck.alternates must be an object FR/FL/BR/BL -> attack row"); return 0; }
            for (int c = d->n[alts].first; c >= 0; c = d->n[c].next) {
                int q = bt_quad(d->n[c].key); if (q < 0) { snprintf(err, cap, "deck.alternates: keys are FR, FL, BR, BL"); return 0; }
                if (d->n[c].t == J_NULL) continue;
                if (d->n[c].t != J_STR || !np_row_ok(d->n[c].str)) { snprintf(err, cap, "deck.alternates.%s: an attack row like \"170\"", d->n[c].key); return 0; }
                snprintf(k->alts[q], 16, "%s", d->n[c].str);
            }
        }
    } else if (d->n[rows].t == J_ARR) {
        for (int e = d->n[rows].first; e >= 0; e = d->n[e].next) {
            int q = d->n[e].t == J_OBJ ? bt_quad(cs_str(d, e, "start")) : -1, ch = q >= 0 ? jfind(d, e, "chain") : -1, al = q >= 0 ? jfind(d, e, "alternate") : -1;
            if (q < 0 || ch < 0 || d->n[ch].t != J_ARR) { snprintf(err, cap, "deck.rows[]: each row needs \"start\" (FR/FL/BR/BL) and \"chain\" [{\"id\": ...}]"); return 0; }
            for (int m = d->n[ch].first; m >= 0; m = d->n[m].next) if (!bt_add_row(k, q, d->n[m].t == J_OBJ ? cs_str(d, m, "id") : NULL, err, cap)) return 0;
            if (al >= 0 && d->n[al].t == J_OBJ && (v = cs_str(d, al, "id")) && np_row_ok(v)) snprintf(k->alts[q], 16, "%s", v);
        }
    } else { snprintf(err, cap, "deck.rows must be an object (bot_decks) or an array (decks_json)"); return 0; }
    if (!k->moves) { snprintf(err, cap, "deck: no moves"); return 0; }
    return 1;
}
// the deck of an entry: inline, or {"file": "<path inside the mod folder>"} read at apply time (g_ce_dir = the mod folder)
static int bt_deck_of(const JDoc *d, int slot, BtDeck *k, int at_apply, char *err, int cap) {   // slot 0: "deck" (required), 1: "deck_sword" (optional: returns -1 when absent)
    int dk = jfind(d, 0, slot ? "deck_sword" : "deck"); if (dk < 0) { if (slot) return -1; snprintf(err, cap, "needs \"deck\""); return 0; }
    const char *f = d->n[dk].t == J_OBJ ? cs_str(d, dk, "file") : NULL;
    if (!f) return bt_read_deck(d, dk, k, slot, err, cap);
    if (strstr(f, "..") || strchr(f, ':') || f[0] == '/' || f[0] == '\\') { snprintf(err, cap, "deck.file '%s' must be a path inside the mod folder", f); return 0; }
    if (!at_apply) return 1;
    char p8[MAX_PATH * 3]; wchar_t pw[MAX_PATH]; snprintf(p8, sizeof p8, "%s\\%s", g_ce_dir, f); MultiByteToWideChar(CP_UTF8, 0, p8, -1, pw, MAX_PATH);
    char *t = mods_read_text(pw, 1u << 20); if (!t) { snprintf(err, cap, "deck.file %s cannot be read", p8); return 0; }
    JDoc fd; int ok = 0;
    if (jdoc_parse(&fd, t) != 0) snprintf(err, cap, "deck.file %s: JSON error: %s", f, fd.err); else ok = bt_read_deck(&fd, 0, k, slot, err, cap);
    jdoc_free(&fd); free(t); return ok;
}
// the npcs entry the bot is made of (npcs.inc copies the template, places spawns, aggro, reactions); passthrough fields are copied as JSON
static int bt_npc_json(const JDoc *d, const char *id, char *out, int cap) {
    static const char *const pass[] = { "name", "health", "hp", "faction", "spawn", "on_health_below", "on_death", "on_area", "on_player_near", NULL };
    const char *t = cs_str(d, 0, "template"), *sv = cs_str(d, 0, "style"); int st = bt_style(sv), len, ag = jfind(d, 0, "aggro");
    len = snprintf(out, cap, "{\"id\":\"%s\",\"base\":\"%s\"", id, t ? t : BT_TEMPLATE);
    if (st >= 0) len += snprintf(out + len, len < cap ? cap - len : 0, ",\"style\":%d", st);
    len += snprintf(out + len, len < cap ? cap - len : 0, ",\"aggro\":%s", ag < 0 || cs_true(d, 0, "aggro") ? "true" : "false");
    for (int i = 0; pass[i]; i++) {
        int c = jfind(d, 0, pass[i]); if (c < 0) continue;
        len += snprintf(out + len, len < cap ? cap - len : 0, ",\"%s\":", pass[i]);
        if (len >= cap) return -1;
        int w = jdoc_write(d, c, out + len, cap - len); if (w < 0) return -1; len += w;
    }
    len += snprintf(out + len, len < cap ? cap - len : 0, "}");
    return len < cap ? len : -1;
}
// outfit: own copy of the template's EquipmentSelectionData (ArchetypeAsset.m_EquipmentSelections[0], what the AI spawner dresses the pawn with) with the
// entry's items. "outfit": { "mask": "/Game/DB/Equipment/...X.X" | null, ... (bots.h bt_outfit_slot), "materials": { "gloves": 2 }, "weapon": "/Game/DB/Weapons/...", "skin_tone": 10, "sex": "Man" }
static void bt_outfit(const char *id, const char *ms, void *arch, const JDoc *d, int node, int *ok, int *bad) {
    struct { uint8_t *data; int num, max; } *sel = CA->prop_ptr(CA, arch, "m_EquipmentSelections");
    void *src = sel && sel->data && sel->num > 0 ? *(void **)sel->data : NULL; char path[400], nm[64], val[400], back[600];
    if (!src || !tk_ptr_ok(src)) { logf_("bots: %s: outfit: the template has no m_EquipmentSelections entry to copy - outfit NOT set", id); (*bad)++; return; }
    snprintf(path, sizeof path, "/Game/Mods/%s/%s_Outfit.%s_Outfit", ms, id, id); void *cp = CA->create_asset(CA, src, path);
    if (!cp) { logf_("bots: %s: outfit: create_asset %s failed", id, path); (*bad)++; return; }
    for (int c = d->n[node].first; c >= 0; c = d->n[c].next) {
        const char *k = d->n[c].key; int s = bt_outfit_slot(k);
        if (s >= 0 || !strcmp(k, "weapon")) {
            if (s >= 0) snprintf(nm, sizeof nm, "m_CharacterEquipments[%d]", s); else snprintf(nm, sizeof nm, "m_Weapon");
            if (d->n[c].t == J_NULL) snprintf(val, sizeof val, "None");
            else if (d->n[c].t == J_STR && d->n[c].str[0] == '/') { snprintf(val, sizeof val, "%s", d->n[c].str); void *o = CA->find_object(CA, val); if (!o) o = CA->load_object(CA, val); if (!o) { logf_("bots: %s: outfit.%s: %s not found - slot left as the template has it", id, k, val); (*bad)++; continue; } }
            else { logf_("bots: %s: outfit.%s: an item path (/Game/...) or null", id, k); (*bad)++; continue; }
            bt_write(id, "outfit", cp, nm, val, ok, bad);
        } else if (!strcmp(k, "materials") && d->n[c].t == J_OBJ) {
            for (int m = d->n[c].first; m >= 0; m = d->n[m].next) { int ms2 = bt_outfit_slot(d->n[m].key); if (ms2 < 0 || !d->n[m].str) { logf_("bots: %s: outfit.materials.%s: unknown slot", id, d->n[m].key ? d->n[m].key : "?"); continue; }
                snprintf(nm, sizeof nm, "m_EquipmentMaterials[%d]", ms2); bt_write(id, "outfit", cp, nm, d->n[m].str, ok, bad); }
        } else if (!strcmp(k, "skin_tone") && d->n[c].str) bt_write(id, "outfit", cp, "m_uiSkinToneIndex", d->n[c].str, ok, bad);
        else if (!strcmp(k, "sex") && d->n[c].str) { *(uint8_t *)(sel->data + 8) = !_stricmp(d->n[c].str, "Woman") ? 1 : 0; logf_("bots: %s: outfit sex -> %s", id, d->n[c].str); }
        else logf_("bots: %s: outfit.%s: unknown key (slots: mask elbow shoulder undertop overtop gloves belt trousers shoes hair; materials, weapon, skin_tone, sex)", id, k);
    }
    *(void **)sel->data = cp; CA->get_property_text(CA, arch, "m_EquipmentSelections", back, sizeof back);
    logf_("bots: %s: outfit %s linked as m_EquipmentSelections[0] (%d entr%s; only [0] is replaced): %.300s", id, path, sel->num, sel->num == 1 ? "y" : "ies", back);
    if (sel->num > 1) { sel->num = 1; logf_("bots: %s: outfit: the template had several selections - cut to the one outfit so every spawn wears it", id); }
}
// the guide's stat points on a spawned bot pawn (UStatsComponent m_CurrentGobalStats +0x1050, FCharacterStats.m_StatsPointDistribution +0x08: Vitality, Strength,
// Agility, Endurance, Faith). The damage of a hit comes from them (UStatsComponent::GetCurrentHitDamage: Agility + Strength points, weight ratio) like a player's.
// The layout is checked first: the 5 ints there must equal BPF_GetCharacterDistributionPoint(type, Current) or nothing is written.
static CBot *bt_of_arch(void *arch) { for (int i = 0; i < g_bt_n; i++) if (arch && g_bt[i].arch == arch) return &g_bt[i]; return NULL; }
static int bt_dist_get(void *sc, int type, int global) {
    static void *f[2]; if (!f[global]) f[global] = CA->find_function(CA, sc, global ? "BPF_GetGlobalCharacterDistributionPoint" : "BPF_GetCharacterDistributionPoint");
    uint8_t t = (uint8_t)type, mode = 0; int r = -1; void *args[2] = { &t, &mode }; if (!f[global] || !CA->call_function_params(CA, sc, f[global], args, &r)) return -1; return r;
}
static void bt_on_spawned(void *ch, const char *why) {
    if (!CA || !tk_ptr_ok(ch)) return; void *arch = CA->npc_archetype(CA, ch); CBot *b = bt_of_arch(arch); if (b && g_bt_spawn_cb) g_bt_spawn_cb(ch, b->mod, b->id); if (!b || !b->has_stats) return;
    __try {
        void *sc = se_comp(CA, ch, "m_StatsComponent"); void **db = sc ? (void **)CA->prop_ptr(CA, sc, "m_StatsDB") : NULL;
        if (!sc || !CA->is_a(CA, sc, "StatsComponent")) { logf_("bots: %s: stats (%s): the pawn has no StatsComponent - stat points not set", b->id, why); return; }
        int before[5], *dist = (int *)((uint8_t *)sc + 0x1050 + 0x08), same = 1;
        for (int i = 0; i < 5; i++) { before[i] = bt_dist_get(sc, i, 0); if (before[i] != dist[i]) same = 0; }
        if (!same) { logf_("bots: %s: stats (%s): layout check FAILED (getter %d %d %d %d %d vs memory %d %d %d %d %d) - stat points not written", b->id, why, before[0], before[1], before[2], before[3], before[4], dist[0], dist[1], dist[2], dist[3], dist[4]); return; }
        for (int i = 0; i < 5; i++) dist[i] = b->stat_pts[i];
        AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; CA->char_info(CA, ch, &ci);
        logf_("bots: %s: stats (%s) on %p: points Vit/Str/Dex/End/Will %d %d %d %d %d -> %d %d %d %d %d (%s build), with style bonus %d %d %d %d %d; StatsDB %s; health %.0f/%.0f",
              b->id, why, ch, before[0], before[1], before[2], before[3], before[4], bt_dist_get(sc, 0, 0), bt_dist_get(sc, 1, 0), bt_dist_get(sc, 2, 0), bt_dist_get(sc, 3, 0), bt_dist_get(sc, 4, 0), b->will ? "Will" : "non-Will",
              bt_dist_get(sc, 0, 1), bt_dist_get(sc, 1, 1), bt_dist_get(sc, 2, 1), bt_dist_get(sc, 3, 1), bt_dist_get(sc, 4, 1), db && *db ? "set" : "NONE (damage does not use the points)", ci.health, ci.max_health);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bots: %s: stats (%s): EXCEPTION 0x%08lX", b->id, why, GetExceptionCode()); }
}
static int bots_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    const char *id = cs_str(&d, 0, "id"), *pr = cs_str(&d, 0, "profile"), *lv = cs_str(&d, 0, "ai_level"), *t = cs_str(&d, 0, "template"), *sv = cs_str(&d, 0, "style");
    int ok = 1, p = jfind(&d, 0, "props"); BtDeck k; static char nj[16384];
    if (pr && bt_profile(pr) < 0) { snprintf(err, cap, "\"profile\" must be " BT_PROFILE_NAMES); ok = 0; }
    else if (lv && (atof(lv) < 0 || atof(lv) > 100)) { snprintf(err, cap, "\"ai_level\" must be 0..100 (the AI's initial difficulty level)"); ok = 0; }
    else if (t && (t[0] != '/' || !strchr(t, '.'))) { snprintf(err, cap, "\"template\" must be an ArchetypeAsset path like %s", BT_TEMPLATE); ok = 0; }
    else if (sv && bt_style(sv) < 0) { snprintf(err, cap, "\"style\": Forsaken, Windfall, Kahlt, Stagger, Faejin or a number 0..63"); ok = 0; }
    else if (cs_str(&d, 0, "resilience") && bt_resilience(cs_str(&d, 0, "resilience")) < 0) { snprintf(err, cap, "\"resilience\" must be 0..10 (0 = interruptible like a player)"); ok = 0; }
    else if (p >= 0 && d.n[p].t != J_OBJ) { snprintf(err, cap, "\"props\" must be an object of ArchetypeAsset property (or property[i]) -> engine text"); ok = 0; }
    else if (!bt_deck_of(&d, 0, &k, 0, err, cap)) ok = 0;
    else if (bt_deck_of(&d, 1, &k, 0, err, cap) == 0) ok = 0;
    else if (bt_stats_mode(cs_str(&d, 0, "stats")) == -2) { snprintf(err, cap, "\"stats\": guide (Will build when the bot has a sword deck), guide_will, guide_no_will or none"); ok = 0; }
    else if (jfind(&d, 0, "outfit") >= 0 && d.n[jfind(&d, 0, "outfit")].t != J_OBJ) { snprintf(err, cap, "\"outfit\" must be an object: mask/elbow/shoulder/undertop/overtop/gloves/belt/trousers/shoes/hair -> item path or null, plus materials {}, weapon, skin_tone, sex"); ok = 0; }
    else if (bt_npc_json(&d, id ? id : "x", nj, sizeof nj) < 0) { snprintf(err, cap, "entry too large"); ok = 0; }
    else { char e2[200] = ""; if (!npcs_validate(nj, e2, sizeof e2, NULL)) { snprintf(err, cap, "%s", e2); ok = 0; } }
    if (ok) cs_warn_unknown(&d, "bots", id ? id : "?", BT_KNOWN);
    jdoc_free(&d); return ok;
}
static int bots_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    static char nj[16384], txt[1400]; char err[200] = "", ms[MOD_NAME_MAX], path[400]; BtDeck k; const char *v;
    int prof = (v = cs_str(&d, 0, "profile")) ? bt_profile(v) : BT_CLASSIC_HARD, level = (v = cs_str(&d, 0, "ai_level")) ? atoi(v) : -1, style = bt_style(cs_str(&d, 0, "style"));
    int mirror = cs_str(&d, 0, "mirror") ? cs_true(&d, 0, "mirror") : 1;
    if (!bt_deck_of(&d, 0, &k, 1, err, sizeof err)) { logf_("bots: %s/%s: %s - entry skipped", mod, id, err); jdoc_free(&d); return 0; }
    static BtDeck ks; int has_sword = bt_deck_of(&d, 1, &ks, 1, err, sizeof err);
    if (has_sword == 0) { logf_("bots: %s/%s: %s - entry skipped", mod, id, err); jdoc_free(&d); return 0; }
    has_sword = has_sword > 0;
    if (bt_npc_json(&d, id, nj, sizeof nj) < 0) { jdoc_free(&d); return 0; }
    int rc = npcs_apply(mod, id, nj, NULL);
    if (rc) { jdoc_free(&d); return rc; }                                               // template not loaded yet: retried at the next pass
    int idx = -1; for (int i = 0; i < g_np_n; i++) if (!strcmp(g_np[i].mod, mod) && !strcmp(g_np[i].id, id)) idx = i;
    void *arch = idx >= 0 ? g_np[idx].obj : NULL;
    if (!arch) { logf_("bots: %s/%s: the npcs part made no archetype - see the npcs: lines", mod, id); jdoc_free(&d); return 0; }
    cs_sanitize(mod, ms, sizeof ms); int ok = 0, bad = 0, n;
    // deck: own copy of the template's bare-hands combo DB with all 4 rows + 4 alternates
    void **dbs = (void **)CA->prop_ptr(CA, arch, "m_ComboDBPerWeapon"); snprintf(path, sizeof path, "/Game/Mods/%s/%s_Deck.%s_Deck", ms, id, id);
    void *deck = dbs && dbs[0] ? CA->create_asset(CA, dbs[0], path) : NULL;
    if (!deck || bt_struct_text(&k, mirror, txt, sizeof txt) < 0) { logf_("bots: %s: no combo DB to copy (the template has no bare-hands deck) - deck NOT set", id); bad++; }
    else {
        int r = CA->set_property_text(CA, deck, "m_Struct", txt); static char back[1400]; CA->get_property_text(CA, deck, "m_Struct", back, sizeof back);
        if (r) dbs[0] = deck;
        int same = !strcmp(back, txt); if (r && same) ok++; else bad++;
        logf_("bots: %s: deck %s (%d moves, %s) set %d, linked %d, read back %s: %s", id, path, k.moves, mirror ? "mirrored like save decks: FR row -> FrontLeft field" : "not mirrored", r, dbs[0] == deck, same ? "IDENTICAL" : "DIFFERENT", back);
    }
    if (has_sword) {                                                                   // the sword deck -> m_ComboDBPerWeapon[1] (copy of the template's sword deck, or of its bare-hands deck: same class)
        void *src = dbs && dbs[1] ? dbs[1] : dbs ? dbs[0] : NULL; snprintf(path, sizeof path, "/Game/Mods/%s/%s_DeckSword.%s_DeckSword", ms, id, id);
        void *sd = src ? CA->create_asset(CA, src, path) : NULL;
        if (!sd || bt_struct_text(&ks, mirror, txt, sizeof txt) < 0) { logf_("bots: %s: sword deck NOT set (no combo DB to copy)", id); bad++; }
        else { int r = CA->set_property_text(CA, sd, "m_Struct", txt); static char back[1400]; CA->get_property_text(CA, sd, "m_Struct", back, sizeof back); if (r) dbs[1] = sd; int same = !strcmp(back, txt); if (r && same) ok++; else bad++;
               logf_("bots: %s: sword deck %s (%d moves) -> m_ComboDBPerWeapon[1] set %d, read back %s (the bot needs a sword: outfit.weapon)", id, path, ks.moves, r, same ? "IDENTICAL" : "DIFFERENT"); }
    }
    // profile: archetype values, then (add-on profile only) own copies of the difficulty presets + its feint asset
    const BtKV *kv = bt_arch_props(prof, &n); for (int i = 0; i < n; i++) bt_write(id, "archetype", arch, kv[i].k, kv[i].v, &ok, &bad);
    kv = bt_diff_props(prof, &n);
    if (level >= 0) { char lv[16]; snprintf(lv, sizeof lv, "%d", level); bt_write(id, "archetype", arch, "m_uiInitialDifficultyLevel", lv, &ok, &bad); }
    if ((v = cs_str(&d, 0, "resilience")) && bt_resilience(v) >= 0) { char rs[8]; snprintf(rs, sizeof rs, "%d", bt_resilience(v)); bt_write(id, "resilience", arch, "m_uiResilience", rs, &ok, &bad); }   // overrides the profile's value
    if (cs_true(&d, 0, "ai_level_fixed")) {                                // pin the live difficulty: own copy of the variation asset with every delta and the drift rate 0 (bots.h bt_fixed_props)
        void **vp = (void **)CA->prop_ptr(CA, arch, "m_difficultyVariationData");
        if (!vp || !*vp) logf_("bots: %s: ai_level_fixed: the template has no m_difficultyVariationData - the level never changes anyway", id);
        else {
            snprintf(path, sizeof path, "/Game/Mods/%s/%s_Fixed.%s_Fixed", ms, id, id); void *cp = CA->create_asset(CA, *vp, path);
            if (!cp) { logf_("bots: %s: ai_level_fixed: create_asset %s failed - the level will drift", id, path); bad++; }
            else { *vp = cp; const BtKV *f = bt_fixed_props(&n); for (int i = 0; i < n; i++) bt_write(id, "Fixed", cp, f[i].k, f[i].v, &ok, &bad); logf_("bots: %s: ai_level_fixed: m_difficultyVariationData -> %s (all deltas 0: the live level stays at the initial level)", id, path); }
        }
    }
    const char *ab = bt_style_ability(style);
    if (ab) { void *o = CA->find_object(CA, ab); if (!o) o = CA->load_object(CA, ab); if (o) bt_write(id, "archetype", arch, "m_SpecialAbilityDB", ab, &ok, &bad); else logf_("bots: %s: %s not found - style ability not set", id, ab); }
    // targeting: m_bAICanSeeForever = the AI never loses aggro. Default on for bots.
    if (!cs_str(&d, 0, "see_forever") || cs_true(&d, 0, "see_forever")) bt_write(id, "targeting", arch, "m_bAICanSeeForever", "True", &ok, &bad);
    // stats: the guide's level-60 build for the bot's style (bots.h bt_stat_build); Will build when it has a sword deck. Body (health / guard) on the archetype now,
    // the stat points on each spawned pawn's StatsComponent (bt_on_spawned: they drive attack damage, weight and shard refill like a player's).
    int smode = bt_stats_mode(cs_str(&d, 0, "stats")), will = smode == 2 ? bt_will_build(1, has_sword) : smode; const BtStatBuild *sb = smode >= 0 ? bt_stat_build(style, will) : NULL;
    if (smode >= 0 && !sb) logf_("bots: %s: stats '%s': no guide build for style %d (Forsaken, Windfall, Kahlt, Stagger, Faejin only) - stats not set", id, cs_str(&d, 0, "stats"), style);
    if (sb) {
        char hv[32]; logf_("bots: %s: stats = guide %s build for style %d (%s): Vitality %d, Strength %d, Dexterity %d, Endurance %d, Will %d (sum %d), health %.0f, stamina %.0f", id, will ? "WILL" : "NON-WILL", style,
                           smode == 2 ? (has_sword ? "auto: it has a sword deck" : "auto: bare hands only") : "set by the entry", sb->pts[0], sb->pts[1], sb->pts[2], sb->pts[3], sb->pts[4], sb->pts[0] + sb->pts[1] + sb->pts[2] + sb->pts[3] + sb->pts[4], sb->health, sb->stamina);
        if (!cs_str(&d, 0, "health") && !cs_str(&d, 0, "hp")) { snprintf(hv, sizeof hv, "%.1f", sb->health); bt_write(id, "stats", arch, "m_fHealth", hv, &ok, &bad); }
        else logf_("bots: %s: stats: the entry sets health itself - the guide's %.0f is not used", id, sb->health);
        snprintf(hv, sizeof hv, "%.1f", sb->stamina); bt_write(id, "stats", arch, "m_fStamina", hv, &ok, &bad);
    }
    int of = jfind(&d, 0, "outfit"); if (of >= 0) bt_outfit(id, ms, arch, &d, of, &ok, &bad);
    int pr = jfind(&d, 0, "props");
    if (pr >= 0) for (int c = d.n[pr].first; c >= 0; c = d.n[c].next) if (d.n[c].key && d.n[c].str) bt_write(id, "props", arch, d.n[c].key, d.n[c].str, &ok, &bad);
    // remember the bot (for spawn hooks, trials and logs)
    CBot *b = NULL; for (int i = 0; i < g_bt_n; i++) if (!strcmp(g_bt[i].mod, mod) && !strcmp(g_bt[i].id, id)) b = &g_bt[i];
    if (!b && g_bt_n < 32) b = &g_bt[g_bt_n++];
    if (b) { memset(b, 0, sizeof *b); snprintf(b->mod, sizeof b->mod, "%s", mod); snprintf(b->id, sizeof b->id, "%s", id); b->profile = prof; b->level = level; b->mirror = mirror; b->style = style; b->moves = k.moves; bt_struct_text(&k, mirror, b->deck_txt, sizeof b->deck_txt);
             b->arch = arch; b->will = will; b->has_stats = sb != NULL; if (sb) memcpy(b->stat_pts, sb->pts, sizeof b->stat_pts); snprintf(b->intro, sizeof b->intro, "%s", (v = cs_str(&d, 0, "intro")) ? v : ""); }
    char il[16] = "?", pl[16] = "?", rs[16] = "?", sh[16] = "?"; bt_get(arch, "m_uiInitialDifficultyLevel", il, sizeof il); bt_get(arch, "m_uiPerfectLinkProbas", pl, sizeof pl);
    bt_get(arch, "m_uiResilience", rs, sizeof rs); bt_get(arch, "m_bSteadyHand", sh, sizeof sh);
    logf_("bots: %s/%s ready: %s, profile %s, initial difficulty %s, perfect link %s, resilience %s, steady hand %s, %d deck moves; %d value(s) read back as written, %d differ%s. Spawn: npc_spawn(\"%s\")",
          mod, id, g_np[idx].path, bt_profile_name(prof), il, pl, rs, sh, k.moves, ok, bad, bad ? " (see <-- DIFFERS above)" : "", id);
    if (g_bt_entry_cb) g_bt_entry_cb(mod, id, json);
    jdoc_free(&d); return 0;
}

// ================================================================ part 2: content kind "ranks" (reward tables for trial_reward)
typedef struct { char mod[MOD_NAME_MAX], id[CR_ID_MAX]; RkTable t; } CRank;
static CRank g_rk[16]; static int g_rk_n;
static int rk_read(const JDoc *d, RkTable *t, char *err, int cap) {
    static const char *const keys[4] = { "finish", "victory", "performance", "mentoring" };
    rk_stock(t); int rw = jfind(d, 0, "rewards"); const char *v;
    if (cs_str(d, 0, "gm_points")) t->gm_points = cs_true(d, 0, "gm_points");
    if (rw < 0) return 1;
    if (d->n[rw].t != J_OBJ) { snprintf(err, cap, "\"rewards\" must be an object"); return 0; }
    if ((v = cs_str(d, rw, "xp_scale"))) { t->xp_scale = atof(v); if (t->xp_scale < 0 || t->xp_scale > 100) { snprintf(err, cap, "rewards.xp_scale must be 0..100"); return 0; } }
    for (int i = 0; i < 4; i++) {
        int c = jfind(d, rw, keys[i]), vals[3] = { -1, -1, -1 }, n = 0; if (c < 0) continue;
        if (d->n[c].t != J_ARR) { snprintf(err, cap, "rewards.%s must be [points, xp]%s", keys[i], i == 2 ? " or [points, xp, min_rounds_won]" : ""); return 0; }
        for (int e = d->n[c].first; e >= 0 && n < 3; e = d->n[e].next) vals[n++] = d->n[e].str ? atoi(d->n[e].str) : -1;
        if (n < 2 || vals[0] < 0 || vals[0] > 100 || vals[1] < 0 || vals[1] > 1000000 || (n == 3 && (i != 2 || vals[2] < 0 || vals[2] > 20))) { snprintf(err, cap, "rewards.%s: [points 0..100, xp 0..1000000%s]", keys[i], i == 2 ? ", min_rounds_won 0..20" : ""); return 0; }
        t->pts[i] = vals[0]; t->xp[i] = vals[1]; if (n == 3) t->min_rounds = vals[2];
    }
    return 1;
}
static int ranks_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    static const char *const known[] = { "id", "rewards", "gm_points", "match", "ladder", "prestige", NULL };
    RkTable t; int ok = rk_read(&d, &t, err, cap); const char *id = cs_str(&d, 0, "id");
    if (ok) { cs_warn_unknown(&d, "ranks", id ? id : "?", known); for (const char *w[] = { "match", "ladder", "prestige", NULL }, **q = w; *q; q++) if (jfind(&d, 0, *q) >= 0) logf_("content: ranks '%s': \"%s\" is not supported - ignored", id ? id : "?", *q); }
    jdoc_free(&d); return ok;
}
static int ranks_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    char err[200]; RkTable t; int ok = rk_read(&d, &t, err, sizeof err); jdoc_free(&d); if (!ok) return 0;
    CRank *r = NULL; for (int i = 0; i < g_rk_n; i++) if (!strcmp(g_rk[i].id, id)) r = &g_rk[i];       // ids are global: the later mod in the load order wins (the registry already resolved that)
    if (!r) { if (g_rk_n >= 16) { logf_("ranks: too many entries (16), %s ignored", id); return 0; } r = &g_rk[g_rk_n++]; }
    snprintf(r->mod, sizeof r->mod, "%s", mod); snprintf(r->id, sizeof r->id, "%s", id); r->t = t;
    int p; logf_("ranks: %s/%s: finish %d/%d, victory %d/%d, performance %d/%d (%d+ rounds), mentoring %d/%d, xp x%.2f, gm points %d -> a won 3-0 trial pays %d XP",
                 mod, id, t.pts[0], t.xp[0], t.pts[1], t.xp[1], t.pts[2], t.xp[2], t.min_rounds, t.pts[3], t.xp[3], t.xp_scale, t.gm_points, rk_reward(&t, 1, 3, 0, &p));
    return 0;
}

// ================================================================ progression API (v2.15)
static void *bt_stats(const AbsApi *self, void *ch) { if (!ch) ch = np_player(self); return tk_ptr_ok(ch) ? se_comp(self, ch, "m_StatsComponent") : NULL; }
// FCharacterStats (Current) through the StatsComponent getters (UFunctions): st[] uses the struct's int layout (0 spare prestige, 7 grade, 8 prestige level, 9 level, 10 XP, 11 gm XP, 12 gm level)
static int bt_stats_read(const AbsApi *self, void *sc, int st[17]) {
    static const struct { int ix; const char *fn; } g[] = { { 0, "BPF_GetSparePrestigePoints" }, { 7, "BPF_GetPrestigeGrade" }, { 8, "BPF_GetPrestigeLevel" }, { 9, "BPF_GetGlobalLevel" },
                                                            { 10, "BPF_GetGlobalXP" }, { 11, "BPF_GetGameModeXP" }, { 12, "BPF_GetGameModeLevel" } };
    memset(st, 0, 17 * sizeof(int)); uint8_t mode = 0; void *args[1] = { &mode };
    for (int i = 0; i < 7; i++) { void *f = api_p_find_function(self, sc, g[i].fn); int v = 0; if (!f || !api_p_call_params(self, sc, f, args, &v)) { logf_("bots: stats: %s not callable", g[i].fn); return 0; } st[g[i].ix] = v; }
    return 1;
}
static int api_p_rank_info(const AbsApi *self, void *ch, AbsRankInfo *out) {
    if (!out || out->size < 8) return 0; const char *who = plug_name_of(plug_of(self));
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: rank_info refused - not on the game thread", who); return 0; }
    AbsRankInfo r; memset(&r, 0, sizeof r); r.size = sizeof r; int ok = 0;
    __try {
        void *sc = bt_stats(self, ch); int st[17];
        if (sc && bt_stats_read(self, sc, st)) {
            r.spare_prestige_points = st[0]; r.prestige_grade = st[7]; r.prestige_level = st[8]; r.level = st[9]; r.xp = st[10]; r.gm_xp = st[11]; r.gm_level = st[12];
            int lvl = -1; uint8_t mode = 0; void *a1[1] = { &mode }, *f = api_p_find_function(self, sc, "BPF_GetVisibleCharacterLevel"); if (f && api_p_call_params(self, sc, f, a1, &lvl)) r.visible_level = lvl;
            lvl = -1; f = api_p_find_function(self, sc, "BPF_GetCharacterLevel"); if (f && api_p_call_params(self, sc, f, a1, &lvl)) r.level = lvl;   // m_iGlobalLevel + unspent stat points: the level the XP loop counts (e.g. 51 + 3 = 54)
            r.next_xp = rk_next_xp(r.level, r.prestige_grade, r.prestige_level);
            int want = r.prestige_grade <= 0 && r.level < 60 ? r.level + 1 : r.prestige_level + 1, g = -1; void *a2[1] = { &want }; f = api_p_find_function(self, sc, "BPF_GetRequiredXPForLevel");
            r.next_xp_game = f && api_p_call_params(self, sc, f, a2, &g) ? g : -1;
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: rank_info: EXCEPTION 0x%08lX", who, GetExceptionCode()); ok = 0; }
    memcpy(out, &r, out->size < sizeof r ? out->size : sizeof r); return ok;
}
static int api_p_xp_add(const AbsApi *self, void *ch, int amount) {
    const char *who = plug_name_of(plug_of(self)); int ok = 0;
    if (!api_p_is_game_thread(self) || amount <= 0 || amount > 10000000) { logf_("plugin %s: xp_add(%d) refused - needs the game thread and 1..10000000 XP", who, amount); return 0; }
    __try {
        void *sc = bt_stats(self, ch), *f = sc ? api_p_find_function(self, sc, "BPF_AddGlobalXP") : NULL; uint8_t inst = 2, mode = 0, notify = 0; void *args[4] = { &inst, &amount, &mode, &notify };   // GAMEMODE, xp, Current, no notify (like the post-match code)
        ok = f && api_p_call_params(self, sc, f, args, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: xp_add: EXCEPTION 0x%08lX", who, GetExceptionCode()); ok = 0; }
    logf_("plugin %s: xp_add(%d) -> %d", who, amount, ok); return ok;
}
static int api_p_gm_points_add(const AbsApi *self, void *ch, int points) {                  // native UStatsComponent::AddGameModeXP(points, Current, NULL, NULL): a Rift Coin per game-mode level crossed
    const char *who = plug_name_of(plug_of(self)); int coins = -1;
    if (!api_p_is_game_thread(self) || points <= 0 || points > 1000) { logf_("plugin %s: gm_points_add(%d) refused - needs the game thread and 1..1000 points", who, points); return -1; }
    static void (*fn)(void *, int, int, void *, void *); if (!fn) { uint32_t rva = api_p_find_symbol(self, "UStatsComponent::AddGameModeXP"); if (rva) fn = (void (*)(void *, int, int, void *, void *))(g_base + rva); }
    if (!fn) { logf_("plugin %s: gm_points_add: UStatsComponent::AddGameModeXP not in symbols.bin", who); return -1; }
    __try {
        void *sc = bt_stats(self, ch); int a[17], b[17];
        if (sc && bt_stats_read(self, sc, a)) { fn(sc, points, 0, NULL, NULL); if (bt_stats_read(self, sc, b)) coins = b[12] - a[12]; logf_("plugin %s: gm_points_add(%d): game-mode XP %d -> %d, level %d -> %d", who, points, a[11], b[11], a[12], b[12]); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: gm_points_add: EXCEPTION 0x%08lX", who, GetExceptionCode()); coins = -1; }
    return coins;
}
static int api_p_trial_reward(const AbsApi *self, void *ch, const char *table, int won, int rounds_won, int mentoring, int pay, AbsTrialReward *out) {
    const char *who = plug_name_of(plug_of(self)); RkTable t; rk_stock(&t); const char *src = "stock 1v1";
    if (table && *table) { int f = 0; for (int i = 0; i < g_rk_n; i++) if (!strcmp(g_rk[i].id, table)) { t = g_rk[i].t; f = 1; src = table; } if (!f) logf_("plugin %s: trial_reward: no ranks entry '%s' - stock table used", who, table); }
    AbsTrialReward r; memset(&r, 0, sizeof r); r.size = sizeof r; r.xp = rk_reward(&t, won, rounds_won, mentoring, &r.points); r.rift_coins = -1;
    if (pay) { r.paid = r.xp <= 0 || api_p_xp_add(self, ch, r.xp); if (r.points > 0) r.rift_coins = api_p_gm_points_add(self, ch, r.points); else r.rift_coins = 0; }
    logf_("plugin %s: trial_reward(%s, won %d, rounds %d, mentoring %d, pay %d) -> %d XP, %d point(s), paid %d, rift coins %d", who, src, won, rounds_won, mentoring, pay, r.xp, r.points, r.paid, r.rift_coins);
    if (out && out->size >= 8) memcpy(out, &r, out->size < sizeof r ? out->size : sizeof r);
    return r.xp;
}
// UAIFightingComponent::m_fCurrentDifficultyLevel (+0x2DC, not a reflected property). set >= 0 writes it (0..100). Returns the level or -1.
static float api_p_npc_difficulty(const AbsApi *self, void *npc, float set) {
    float r = -1.f; if (!tk_ptr_ok(npc)) return -1.f;
    __try {
        void *aic = se_comp(self, npc, "m_AIComponent");
        if (aic && api_p_is_a(self, aic, "AIFightingComponent")) {
            float *p = (float *)((uint8_t *)aic + 0x2DC);
            if (isfinite(*p) && *p >= 0.f && *p <= 100.f) { if (set >= 0.f && set <= 100.f) *p = set; r = *p; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1.f; }
    return r;
}
#include "bottrial.inc"                                             // kind "bot_trials" and API 2.16 bot_trial_start / bot_trial_status
static void bt_fill_api(AbsApi *a) {
    a->bot_trial_start = api_p_bot_trial_start; a->bot_trial_status = api_p_bot_trial_status;
    a->xp_add = api_p_xp_add; a->gm_points_add = api_p_gm_points_add; a->rank_info = api_p_rank_info; a->trial_reward = api_p_trial_reward; a->npc_difficulty = api_p_npc_difficulty;
}

// powers.inc - "powers" (included at the end of content.inc). Guide: docs\MODDING-GUIDE.md section 10c, API: docs\PLUGIN-API.md "Powers (v2.9)".
// Part 1: API v2.9 (inventory_equip_slot, on_power_used, power_use). Part 2: the content kind "powers" = a clone of a stock power (UsableItemData) built by the items code,
// plus per-power animations, an own ItemAttack row and a per-INSTANCE override of the spawned effect actor (attack row, radius). The stock classes and their CDOs are never written.
#include "powers.h"

// ================================================================ part 1: API
// A power is used through AFightingCharacter::LaunchItemUseOrder(dir); the order later spawns the effect actor and calls its Blueprint event BPE_BeginItemUseAction (owner already set).
// on_power_used remembers the item of every launch (GetUsableItemFromDirection: inventory slot for players, archetype use case for NPCs) and reports it when that actor begins.
typedef struct { int plugin; void (*cb)(void *character, void *item, void *actor, int dir, void *user); void *user; } PwCb;
static PwCb g_pwcb[16]; static int g_pwcb_n, g_pw_hooked; static const AbsApi *g_pw_self;
typedef struct { void *ch, *item; int dir; } PwLaunch;
static PwLaunch g_pwl[8]; static unsigned g_pwl_n;
static void (*g_o_launch)(void *, uint8_t); static void *(*g_pw_fromdir)(void *, uint8_t);
static void pw_launch_det(void *ch, uint8_t dir) {
    void *it = NULL; __try { if (g_pw_fromdir) it = g_pw_fromdir(ch, dir); } __except (EXCEPTION_EXECUTE_HANDLER) { it = NULL; }
    PwLaunch *l = &g_pwl[g_pwl_n++ & 7]; l->ch = ch; l->item = it; l->dir = dir;
    g_o_launch(ch, dir);
}
static int pw_begin_pre(void *actor, void *func, void *params, void *user) {
    (void)func; (void)params; (void)user;
    void **po = (void **)api_p_prop_ptr(g_pw_self, actor, "Owner"); void *owner = po && se_valid_ptr(*po) ? *po : NULL; const PwLaunch *l = NULL;
    for (unsigned i = 0; i < 8 && i < g_pwl_n; i++) { const PwLaunch *c = &g_pwl[(g_pwl_n - 1 - i) & 7]; if (!owner || c->ch == owner) { l = c; break; } }
    if (!l) { static int warned; if (warned++ < 3) logf_("powers: BPE_BeginItemUseAction on %p (owner %p) without a matching launch - ignored", actor, owner); return 0; }
    for (int i = 0; i < g_pwcb_n; i++) {
        if (!plug_guard(g_pwcb[i].plugin)) continue;
        __try { g_pwcb[i].cb(l->ch, l->item, actor, l->dir, g_pwcb[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(g_pwcb[i].plugin, "on_power_used", GetExceptionCode()); }
    }
    return 0;
}
static int api_p_on_power_used(const AbsApi *self, void (*cb)(void *, void *, void *, int, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_pwcb_n >= 16) return 1;
    if (!g_pw_hooked) {
        g_pw_self = self; g_pw_fromdir = (void *(*)(void *, uint8_t))(g_base + se_rva_sym("AFightingCharacter::GetUsableItemFromDirection", 0x335160u));
        g_o_launch = (void (*)(void *, uint8_t))api_p_hook_symbol(self, "AFightingCharacter::LaunchItemUseOrder", (void *)pw_launch_det);
        int oc = api_p_on_call(self, NULL, "BPE_BeginItemUseAction", pw_begin_pre, NULL, NULL);
        logf_("powers: on_power_used hooks: LaunchItemUseOrder %p, BPE_BeginItemUseAction on_call %d", (void *)g_o_launch, oc);
        if (!g_o_launch || oc) return 2;
        g_pw_hooked = 1;
    }
    EnterCriticalSection(&g_lock); g_pwcb[g_pwcb_n].plugin = p; g_pwcb[g_pwcb_n].cb = cb; g_pwcb[g_pwcb_n].user = user; g_pwcb_n++; LeaveCriticalSection(&g_lock);
    logf_("plugin %s: on_power_used registered", plug_name_of(p)); return 0;
}
static int api_p_inventory_equip_slot(const AbsApi *self, int index, int slot) {         // FightingPlayerController::EquipFromInventory(index, slot): power slots 0 (key 1) and 1 (key 3)
    const char *who = plug_name_of(plug_of(self)); void *pc = api_p_local_pc(self), *f = pc ? api_p_find_function(self, pc, "EquipFromInventory") : NULL;
    if (!f || index < 0 || slot < 0) { logf_("plugin %s: inventory_equip_slot(%d, %d): no player controller / function, or bad arguments", who, index, slot); return 0; }
    uint32_t i = (uint32_t)index; int s = slot; void *args[2] = { &i, &s };
    int ok = api_p_call_params(self, pc, f, args, NULL); logf_("plugin %s: inventory_equip_slot(%d, %d) -> %d", who, index, slot, ok); return ok;
}
static int api_p_power_use(const AbsApi *self, void *ch, int dir) {                     // the input path without the key: CanUseEquippedItem (local player) + LaunchItemUseOrder
    const char *who = plug_name_of(plug_of(self));
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: power_use refused - not on the game thread", who); return 0; }
    if (dir < 0 || dir > 1) { logf_("plugin %s: power_use: dir must be 0 (key 1) or 1 (key 3)", who); return 0; }
    void *pc = api_p_local_pc(self), *pawn = NULL, *gp = pc ? api_p_find_function(self, pc, "K2_GetPawn") : NULL; if (gp) api_p_call_params(self, pc, gp, NULL, &pawn);
    if (!ch) ch = pawn; if (!ch) { logf_("plugin %s: power_use: no character", who); return 0; }
    if (ch == pawn) {
        void **ip = (void **)api_p_prop_ptr(self, ch, "m_InventoryComponent"); void *inv = ip && se_valid_ptr(*ip) ? *ip : NULL; unsigned char r = 255;
        if (!inv) { logf_("plugin %s: power_use: no inventory component", who); return 0; }
        // UInventoryComponent::CanUseEquippedItem(inv, slot, broadcastFailure) returns 1 when the power CAN be used (CanUseItem(inv, &m_EquippedItems[slot], slot) == CanUse; an empty list also answers 0); 0x3F8530 / 0x3F85B0
        __try { r = ((unsigned char (*)(void *, uint8_t, uint8_t))(g_base + se_rva_sym("UInventoryComponent::CanUseEquippedItem", 0x3F8530u)))(inv, (uint8_t)dir, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: power_use: EXCEPTION in CanUseEquippedItem", who); return 0; }
        if (r == 0) {
            static const char *const why[] = { "CanUse", "NotEquipped", "OnCoolDown", "NoItemLeft", "InsufficientShards", "TargetRecquirementsNotMet", "ShardsFrozen" }; unsigned char w = 255;
            uint8_t *arr = (uint8_t *)api_p_prop_ptr(self, inv, "m_EquippedItems");
            __try { if (arr && *(int *)(arr + 8) > dir) w = ((unsigned char (*)(void *, void *, uint8_t))(g_base + se_rva_sym("UInventoryComponent::CanUseItem", 0x3F85B0u)))(inv, *(uint8_t **)arr + (size_t)dir * 0x88, (uint8_t)dir); } __except (EXCEPTION_EXECUTE_HANDLER) { w = 255; }
            logf_("plugin %s: power_use(dir %d): refused, %s", who, dir, w < 7 ? why[w] : "cannot be used"); return 0;
        }
    }
    __try { ((void (*)(void *, uint8_t))(g_base + se_rva_sym("AFightingCharacter::LaunchItemUseOrder", 0x365D90u)))(ch, (uint8_t)dir); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: power_use: EXCEPTION in LaunchItemUseOrder", who); return 0; }
    logf_("plugin %s: power_use(%p, dir %d) launched", who, ch, dir); return 1;
}

// ================================================================ part 2: content kind "powers"
#define PW_ITEMATK "/Game/DB/Items/ItemAttack.ItemAttack"
typedef struct {
    char mod[MOD_NAME_MAX], id[CR_ID_MAX], base[300], row[160], atk_copy[64]; int nset; char sk[16][64], sv[16][200];
    int cost, treq, slot, grant, mirror[4]; double cooldown; double radius; char anim[4][300]; char icon_file[MAX_PATH * 3];
    void *obj; int granted, row_done, hooked, gtries, gwhy;          // granted = done for this character; gtries / gwhy: grant poll (content.inc grants_poll, grantpoll.h)
} CPower;
static CPower g_pw[32]; static int g_pw_n;

static int powers_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    static const char *const known[] = { "id", "base", "name", "description", "icon", "icon_file", "cost", "cooldown", "target_requirements", "anims", "anims_mirror", "attack", "radius", "props", "grant", "equip_slot", NULL };
    const char *base = cs_str(&d, 0, "base"), *id = cs_str(&d, 0, "id"); int ok = 1, a = jfind(&d, 0, "anims"), m = jfind(&d, 0, "anims_mirror"), at = jfind(&d, 0, "attack"), pr = jfind(&d, 0, "props");
    const char *v;
    if (!base || base[0] != '/' || !strchr(base, '.')) { snprintf(err, cap, "needs \"base\": the stock power to copy, like /Game/DB/Items/Shockwave.Shockwave (it decides the effect actor and its look)"); ok = 0; }
    else if (!pw_check_numbers((v = cs_str(&d, 0, "cost")) ? atoi(v) : -1, (v = cs_str(&d, 0, "cooldown")) ? atof(v) : -1, (v = cs_str(&d, 0, "target_requirements")) ? atoi(v) : -1,
                               (v = cs_str(&d, 0, "equip_slot")) ? atoi(v) : -1, (v = cs_str(&d, 0, "radius")) ? atof(v) : -1, err, cap)) ok = 0;
    else if ((v = cs_str(&d, 0, "cost")) && atoi(v) < 0) { snprintf(err, cap, "\"cost\" (shards) must be 0-255"); ok = 0; }
    else if ((v = cs_str(&d, 0, "cooldown")) && atof(v) < 0) { snprintf(err, cap, "\"cooldown\" is in seconds and must be 0 or more"); ok = 0; }
    else if (pr >= 0 && d.n[pr].t != J_OBJ) { snprintf(err, cap, "\"props\" must be an object"); ok = 0; }
    else if (a >= 0 && d.n[a].t != J_OBJ) { snprintf(err, cap, "\"anims\" must be an object: \"all\" or \"fl\" / \"fr\" / \"br\" / \"bl\" -> AnimSequence path"); ok = 0; }
    else if (m >= 0 && (d.n[m].t != J_ARR)) { snprintf(err, cap, "\"anims_mirror\" must be an array of 4 true/false"); ok = 0; }
    else if (at >= 0 && (d.n[at].t != J_OBJ || !cs_str(&d, at, "copy_of") || (jfind(&d, at, "set") >= 0 && d.n[jfind(&d, at, "set")].t != J_OBJ))) { snprintf(err, cap, "\"attack\" must be an object with \"copy_of\" (an ItemAttack row such as 10001) and optional \"set\" (an object of row values)"); ok = 0; }
    if (ok && a >= 0) for (int c = d.n[a].first; c >= 0 && ok; c = d.n[c].next) {
        if (pw_anim_index(d.n[c].key) == -2) { snprintf(err, cap, "\"anims\" key '%s' is not all / fl / fr / br / bl", d.n[c].key ? d.n[c].key : "?"); ok = 0; }
        else if (!d.n[c].str || d.n[c].str[0] != '/' || !strchr(d.n[c].str, '.')) { snprintf(err, cap, "\"anims\" values are AnimSequence paths like /Game/Animations/ItemUse/Exhaust.Exhaust"); ok = 0; }
    }
    if (ok && m >= 0) { int n = 0; for (int c = d.n[m].first; c >= 0; c = d.n[c].next) n++; if (n != 4) { snprintf(err, cap, "\"anims_mirror\" needs exactly 4 values (one per stance)"); ok = 0; } }
    if (ok) cs_warn_unknown(&d, "powers", id ? id : "?", known);
    jdoc_free(&d); return ok;
}
static int powers_parse(CPower *p, const char *mod, const char *id, const char *json, char *items_json, int cap) {
    JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    memset(p, 0, sizeof *p); snprintf(p->mod, sizeof p->mod, "%s", mod); snprintf(p->id, sizeof p->id, "%s", id); p->cost = p->treq = p->slot = -1; p->cooldown = -1; for (int i = 0; i < 4; i++) p->mirror[i] = -1;
    char ms[MOD_NAME_MAX]; cs_sanitize(mod, ms, sizeof ms); snprintf(p->row, sizeof p->row, "%s_%s", ms, id);
    const char *v;
    if ((v = cs_str(&d, 0, "base"))) snprintf(p->base, sizeof p->base, "%s", v);
    if ((v = cs_str(&d, 0, "cost"))) p->cost = atoi(v);
    if ((v = cs_str(&d, 0, "cooldown"))) p->cooldown = atof(v);
    if ((v = cs_str(&d, 0, "target_requirements"))) p->treq = atoi(v);
    if ((v = cs_str(&d, 0, "equip_slot"))) p->slot = atoi(v);
    if ((v = cs_str(&d, 0, "radius"))) p->radius = atof(v);
    p->grant = cs_true(&d, 0, "grant");
    int a = jfind(&d, 0, "anims");
    if (a >= 0) for (int c = d.n[a].first; c >= 0; c = d.n[c].next) { int k = pw_anim_index(d.n[c].key); for (int i = 0; i < 4; i++) if ((k == -1 || k == i) && d.n[c].str) snprintf(p->anim[i], sizeof p->anim[i], "%s", d.n[c].str); }
    int m = jfind(&d, 0, "anims_mirror"); if (m >= 0) { int i = 0; for (int c = d.n[m].first; c >= 0 && i < 4; c = d.n[c].next, i++) p->mirror[i] = d.n[c].str && (!strcmp(d.n[c].str, "true") || !strcmp(d.n[c].str, "1")); }
    int at = jfind(&d, 0, "attack");
    if (at >= 0) {
        snprintf(p->atk_copy, sizeof p->atk_copy, "%s", cs_str(&d, at, "copy_of")); int s = jfind(&d, at, "set");
        if (s >= 0) for (int c = d.n[s].first; c >= 0 && p->nset < 16; c = d.n[c].next) if (d.n[c].key && d.n[c].str) { snprintf(p->sk[p->nset], 64, "%s", d.n[c].key); snprintf(p->sv[p->nset], 200, "%s", d.n[c].str); p->nset++; }
    }
    // the items entry that builds the clone: name, description, icon, and the three UsableItemData values as props
    char e1[500], e2[900], e3[700], e4[700]; int n = 0;
    n += snprintf(items_json + n, cap - n, "{\"id\":\"%s\",\"base\":\"%s\"", id, p->base);
    if ((v = cs_str(&d, 0, "name"))) { pw_json_escape(v, e1, sizeof e1); n += snprintf(items_json + n, cap - n, ",\"name\":\"%s\"", e1); }
    if ((v = cs_str(&d, 0, "description"))) { pw_json_escape(v, e2, sizeof e2); n += snprintf(items_json + n, cap - n, ",\"description\":\"%s\"", e2); }
    if ((v = cs_str(&d, 0, "icon"))) { pw_json_escape(v, e3, sizeof e3); n += snprintf(items_json + n, cap - n, ",\"icon\":\"%s\"", e3); }
    if ((v = cs_str(&d, 0, "icon_file"))) { pw_json_escape(v, e4, sizeof e4); n += snprintf(items_json + n, cap - n, ",\"icon_file\":\"%s\"", e4); }
    n += snprintf(items_json + n, cap - n, ",\"props\":{");
    int first = 1;
    if (p->cost >= 0) { n += snprintf(items_json + n, cap - n, "%s\"m_uiShardsCost\":\"%d\"", first ? "" : ",", p->cost); first = 0; }
    if (p->cooldown >= 0) { n += snprintf(items_json + n, cap - n, "%s\"m_fCooldown\":\"%f\"", first ? "" : ",", p->cooldown); first = 0; }
    if (p->treq >= 0) { n += snprintf(items_json + n, cap - n, "%s\"m_iTargetRecquirements\":\"%d\"", first ? "" : ",", p->treq); first = 0; }
    int pr = jfind(&d, 0, "props"), np = 0;
    if (pr >= 0) for (int c = d.n[pr].first; c >= 0 && np < 12; c = d.n[c].next) if (d.n[c].key && d.n[c].str) { pw_json_escape(d.n[c].key, e1, sizeof e1); pw_json_escape(d.n[c].str, e2, sizeof e2); n += snprintf(items_json + n, cap - n, "%s\"%s\":\"%s\"", first ? "" : ",", e1, e2); first = 0; np++; }
    n += snprintf(items_json + n, cap - n, "}}");
    jdoc_free(&d); return n < cap;
}
static CItem *powers_item(const char *mod, const char *id) { for (int i = 0; i < g_items_n; i++) if (!strcmp(g_items[i].mod, mod) && !strcmp(g_items[i].id, id)) return &g_items[i]; return NULL; }

// the effect actor of a registry power was spawned: patch THIS INSTANCE (attack row, hit radius); the class and its defaults stay stock
static void powers_used(void *ch, void *item, void *actor, int dir, void *u) {
    (void)u; CPower *p = NULL; for (int i = 0; i < g_pw_n; i++) if (g_pw[i].obj && g_pw[i].obj == item) p = &g_pw[i];
    if (!p) return;
    char cn[96] = "?", an[96] = "?", rad[64] = "?", cls[96] = "?"; CA->object_name(CA, ch, cn, sizeof cn); CA->class_name(CA, CA->object_class(CA, actor), cls, sizeof cls);
    void **shape = (void **)CA->prop_ptr(CA, actor, "m_ShapeComponent"); void *sh = shape && *shape ? *shape : NULL; int ok1 = -1, ok2 = -1;
    if (p->row_done && p->atk_copy[0]) ok1 = CA->set_property_text(CA, actor, "m_ItemAttackName", p->row);
    if (p->radius > 0 && sh) { void *f = CA->find_function(CA, sh, "SetSphereRadius"); float r = (float)p->radius; int upd = 1; void *args[2] = { &r, &upd }; ok2 = f ? CA->call_function_params(CA, sh, f, args, NULL) : 0; }
    CA->get_property_text(CA, actor, "m_ItemAttackName", an, sizeof an); if (sh) CA->get_property_text(CA, sh, "SphereRadius", rad, sizeof rad);
    logf_("powers: %s/%s used by %s (dir %d): actor %s -> m_ItemAttackName=%s (set %d), SphereRadius=%s (set %d)", p->mod, p->id, cn, dir, cls, an, ok1, rad, ok2);
}
static void powers_grant_step(int ready, int *pending) {               // one poll of content.inc grants_poll; a power slot is equipped only on a NEW grant (the player's own slot choice stays)
    for (int i = 0; i < g_pw_n; i++) {
        CPower *p = &g_pw[i]; if (!p->grant || p->granted) continue;
        int why = p->obj ? ready : GP_NO_OBJECT, owned = why == GP_OK ? CA->inventory_index(CA, p->obj) : -1; p->gtries++;
        GpVerdict v = gp_decide(why, owned);
        if (v == GPV_OWNED) { p->granted = 1; logf_("powers: %s already owned (inventory index %d), not granted again", p->id, owned); continue; }
        if (v == GPV_ADD) {
            int idx = CA->inventory_add(CA, p->obj, 0, 0);
            if (idx >= 0) {
                p->granted = 1; logf_("powers: granted %s -> inventory index %d (attempt %d)", p->id, idx, p->gtries);
                if (p->slot >= 0) logf_("powers: equip %s (index %d) in power slot %d -> %d", p->id, idx, p->slot, CA->inventory_equip_slot(CA, idx, p->slot));
                continue;
            }
            why = GP_ADD_FAILED;
        }
        if (gp_log_now(p->gtries, why, p->gwhy)) logf_("powers: grant %s: waiting (%s), attempt %d", p->id, gp_why(why), p->gtries);
        p->gwhy = why; (*pending)++;
    }
}
static void powers_grant_rearm(void) { for (int i = 0; i < g_pw_n; i++) { g_pw[i].granted = 0; g_pw[i].gtries = 0; g_pw[i].gwhy = 0; } }
static int powers_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; static char ij[8192]; CPower n;
    if (!powers_parse(&n, mod, id, json, ij, sizeof ij)) { logf_("powers: %s/%s: entry could not be parsed (or is too long)", mod, id); return 0; }
    CPower *p = NULL; for (int i = 0; i < g_pw_n; i++) if (!strcmp(g_pw[i].mod, mod) && !strcmp(g_pw[i].id, id)) p = &g_pw[i];
    if (p && g_reloading && (strcmp(p->base, n.base) || strcmp(p->atk_copy, n.atk_copy))) { g_reload_blocked++; logf_("content: reload: power %s/%s: base / attack.copy_of changed - that needs a restart; this entry was not changed", mod, id); return 0; }
    int rc = items_apply(mod, id, ij, NULL); if (rc) return rc;                                    // the clone itself: create_asset + name / icon / cost / cooldown (items code, incl. hot reload)
    CItem *it = powers_item(mod, id); if (!it || !it->obj) { logf_("powers: %s/%s: no clone object (items table full?) - skipped", mod, id); return 0; }
    if (!CA->is_a(CA, it->obj, "UsableItemData")) { logf_("powers: %s/%s: base %s is not a UsableItemData (a power) - skipped", mod, id, n.base); return 0; }
    if (!p) { if (g_pw_n >= 32) { logf_("powers: too many powers (32), %s ignored", id); return 0; } p = &g_pw[g_pw_n++]; memset(p, 0, sizeof *p); }
    void *obj = p->obj; int granted = p->granted, row_done = p->row_done; *p = n; p->obj = obj ? obj : it->obj; p->granted = granted; p->row_done = row_done;
    uint8_t *anims = (uint8_t *)CA->prop_ptr(CA, p->obj, "m_Anims");                              // UAnimSequence *m_animation +0, bool m_bMirror +8, stride 0x20
    for (int i = 0; i < 4; i++) {
        if (p->anim[i][0] && anims) { void *an = CA->find_object(CA, p->anim[i]); if (!an) an = CA->load_object(CA, p->anim[i]); if (an) { *(void **)(anims + i * 0x20) = an; logf_("powers: %s anim[%d] = %s", id, i, p->anim[i]); } else logf_("powers: %s anim[%d] %s could not be loaded - stock animation kept", id, i, p->anim[i]); }
        if (p->mirror[i] >= 0 && anims) *(anims + i * 0x20 + 8) = (uint8_t)p->mirror[i];
    }
    if (p->atk_copy[0]) {
        if (!CA->find_object(CA, PW_ITEMATK)) { logf_("powers: %s: ItemAttack table not loaded yet - retried", id); return 1; }
        if (!p->row_done) { p->row_done = CA->table_add_row(CA, PW_ITEMATK, p->row, p->atk_copy); logf_("powers: %s: ItemAttack row %s = copy of %s -> %d", id, p->row, p->atk_copy, p->row_done); }
        int ok = 0; for (int i = 0; i < p->nset && p->row_done; i++) { int r = CA->row_set_text(CA, PW_ITEMATK, p->row, p->sk[i], p->sv[i]); ok += r; logf_("powers: %s: row %s.%s = %s -> %d", id, p->row, p->sk[i], p->sv[i], r); }
        (void)ok;
    }
    if (!p->hooked) { p->hooked = 1; static int reg; if (!reg) { reg = 1; logf_("powers: on_power_used -> %d (grants: content.inc grants_poll)", CA->on_power_used(CA, powers_used, NULL)); } }
    char t[300] = ""; CA->get_property_text(CA, p->obj, "m_uiShardsCost", t, sizeof t); char c2[64] = ""; CA->get_property_text(CA, p->obj, "m_fCooldown", c2, sizeof c2);
    logf_("powers: %s/%s ready: %p, shards cost %s, cooldown %s, attack row %s, radius %.0f, slot %d, grant %d", mod, id, p->obj, t, c2, p->atk_copy[0] ? p->row : "(stock)", p->radius, p->slot, p->grant);
    if (p->grant && !p->granted) grants_arm(g_reloading ? "hot reload" : "power ready");
    return 0;
}

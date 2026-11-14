// npcs.inc - NPCs (included at the end of content.inc, after powers.inc). Guide: docs\MODDING-GUIDE.md section 10d, API: docs\PLUGIN-API.md "NPCs (v2.11)".
// Part 1: API v2.11 (npc_spawn, npc_remove, npc_set_target, npc_set_faction, faction_can_target, npc_archetype, on_area, area_remove, show_message).
// Part 2: the content kind "npcs" = a copy of a stock ArchetypeAsset (health, style, deck rows, AI tuning, faction) plus data-driven reactions (on_health_below, on_death, on_area) and an optional
// spawn point. The stock archetype and its deck are never written; every NPC of the entry is made by npc_spawn (own AAISpawner placed AT the spot: an AI pawn that is teleported after it spawned never moves again).
#include "npcs.h"

// ================================================================ part 1: API
#define NP_LIVE_MAX 64
typedef struct { void *ch, *sp; int def, hp_fired, area, stats_done; float z0; } NpLive;           // def = index into g_np, -1 = a character that is not an npcs entry's (cached so it is not looked up again)
static NpLive g_npl[NP_LIVE_MAX]; static int g_npl_n, g_npl_next; static const AbsApi *g_np_api;

typedef struct { int plugin, used, handle, in; float c[3], r; void *actor, *who; void (*cb)(void *, int, void *); void *user; } NpArea;
static NpArea g_ar[32]; static int g_ar_next = 1, g_ar_timer, g_ar_hooked;

static void *np_player(const AbsApi *self) {
    void *pc = api_p_local_pc(self), *pawn = NULL, *gp = pc ? api_p_find_function(self, pc, "K2_GetPawn") : NULL;
    if (gp) api_p_call_params(self, pc, gp, NULL, &pawn);
    return tk_ptr_ok(pawn) ? pawn : NULL;
}
static void np_show(const char *text, unsigned seconds) {                           // the loader's own on-screen text (the toast of the F-key messages): up to 3 lines of 96 characters
    char lines[3][LM_LINE_MAX]; int n = 0; const char *p = text;
    while (*p && n < 3) { int k = 0; while (*p && *p != '\n' && k < LM_LINE_MAX - 4) lines[n][k++] = *p++; lines[n][k] = 0; n++; if (*p == '\n') p++; }
    if (n) toast_show(lines, n, seconds < 1 ? 1 : seconds > 30 ? 30 : seconds);
}
static int api_p_show_message(const AbsApi *self, const char *utf8, float seconds) {
    if (!utf8 || !*utf8) return 0;
    logf_("plugin %s: show_message '%.80s' (%.1f s)", plug_name_of(plug_of(self)), utf8, seconds);
    np_show(utf8, (unsigned)(seconds + 0.5f)); return 1;
}

// ----- areas: one 0.1 s timer for all registrations (2D distance, 5 % hysteresis on the way out)
static void np_area_poll(void *u) {
    (void)u; void *player = NULL; int have_player = 0;
    for (int i = 0; i < 32; i++) {
        NpArea *a = &g_ar[i]; if (!a->used) continue;
        float c[3]; if (a->actor) { if (!api_p_test_get_location(g_np_api, a->actor, c)) continue; } else memcpy(c, a->c, sizeof c);
        void *who = a->who; if (!who) { if (!have_player) { player = np_player(g_np_api); have_player = 1; } who = player; }
        float w[3]; if (!who || !api_p_test_get_location(g_np_api, who, w)) continue;
        int now_in = a->in, r = np_area_step(np_dist2d(c, w), a->r, a->in, &now_in); a->in = now_in;
        if (r != 0 && plug_guard(a->plugin)) { __try { a->cb(who, r > 0, a->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(a->plugin, "on_area", GetExceptionCode()); } }
    }
}
static void np_forget_char(void *ch);
static void np_destroyed(void *ch, int reason, void *u) {
    (void)reason; (void)u;
    for (int i = 0; i < 32; i++) if (g_ar[i].used && (g_ar[i].actor == ch || g_ar[i].who == ch)) g_ar[i].used = 0;
    np_forget_char(ch);
}
static int api_p_on_area(const AbsApi *self, const float center[3], void *center_actor, float radius, void *who, void (*cb)(void *, int, void *), void *user) {
    int p = plug_of(self); const char *nm = plug_name_of(p);
    if (p < 0 || !cb || radius <= 0 || (!center && !center_actor)) { logf_("plugin %s: on_area: bad arguments (a callback, a radius > 0 and a center or a center actor are needed)", nm); return 0; }
    if (!g_np_api) g_np_api = self;
    if (!g_ar_hooked) { g_ar_hooked = 1; logf_("area: on_pawn_destroyed -> %d (areas of a destroyed character are dropped)", api_p_on_pawn_destroyed(self, np_destroyed, NULL)); }
    int slot = -1; for (int i = 0; i < 32; i++) if (!g_ar[i].used) { slot = i; break; }
    if (slot < 0) { logf_("plugin %s: on_area: all 32 area slots are used", nm); return 0; }
    NpArea *a = &g_ar[slot]; memset(a, 0, sizeof *a); a->plugin = p; a->used = 1; a->handle = g_ar_next++; a->r = radius; a->actor = center_actor; a->who = who; a->cb = cb; a->user = user;
    if (center) memcpy(a->c, center, sizeof a->c);
    if (!g_ar_timer) { g_ar_timer = api_p_set_timer_id(self, 0.1f, 1, np_area_poll, NULL); logf_("area: poll timer %d", g_ar_timer); }
    logf_("plugin %s: on_area #%d radius %.0f %s %s", nm, a->handle, radius, center_actor ? "around an actor" : "around a point", who ? "for one character" : "for the local player");
    return a->handle;
}
static int api_p_area_remove(const AbsApi *self, int handle) {
    (void)self; for (int i = 0; i < 32; i++) if (g_ar[i].used && g_ar[i].handle == handle) { g_ar[i].used = 0; return 1; }
    return 0;
}

// ----- live NPCs: character -> entry (found by the archetype the character runs, cached)
typedef struct { char mod[MOD_NAME_MAX], ms[MOD_NAME_MAX], id[CR_ID_MAX], base[300], path[300], name[120], deck_copy[300], smap[64], pk[16][64], pv[16][200], cls[200];   // cls: optional character class ("class", e.g. the practice dummy's BP_AICharacter_Training_C)
                 char rows[5][8][16]; int nrows[5]; int has_deck, nprops, nacts[3]; char acts[3][6][120];       // acts: 0 on_health_below, 1 on_death, 2 on_area
                 double health, style, faction, cooldown, hp_frac, area_r, near_r, yaw, delay; int can_attack, can_defend, aggro, has_spawn; float sloc[3];
                 void *obj; int area_h; void *spawned_world; } CNpc;
static CNpc g_np[32]; static int g_np_n, g_np_react;
static void np_area_attach(const AbsApi *self, NpLive *l);

static void np_forget_char(void *ch) {
    for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch) { g_npl[i] = g_npl[--g_npl_n]; if (g_npl_next >= g_npl_n) g_npl_next = 0; return; }
}
static NpLive *np_live_add(const AbsApi *self, void *ch, void *sp, int def) {
    NpLive *l = NULL; for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch) l = &g_npl[i];
    if (!l) { if (g_npl_n < NP_LIVE_MAX) l = &g_npl[g_npl_n++]; else { l = &g_npl[g_npl_next]; g_npl_next = (g_npl_next + 1) % NP_LIVE_MAX; } }
    memset(l, 0, sizeof *l); l->ch = ch; l->sp = sp; l->def = def;
    if (def >= 0) np_area_attach(self, l);
    return l;
}
static int np_find_def_by_path(const char *path) { for (int i = 0; i < g_np_n; i++) if (!_stricmp(g_np[i].path, path)) return i; return -1; }
static NpLive *np_live_of(const AbsApi *self, void *ch) {                         // the entry-bound NPC record of a character, or NULL
    if (!tk_ptr_ok(ch)) return NULL;
    for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch) return g_npl[i].def >= 0 ? &g_npl[i] : NULL;
    int def = -1; char txt[700] = "";
    __try {
        void **aic = (void **)api_p_prop_ptr(self, ch, "m_AIComponent");
        if (aic && tk_ptr_ok(*aic) && api_p_get_text(self, *aic, "m_ArchetypeDescription", txt, sizeof txt) > 0) for (int i = 0; i < g_np_n && def < 0; i++) if (np_text_names(txt, g_np[i].path)) def = i;
    } __except (EXCEPTION_EXECUTE_HANDLER) { def = -1; }
    NpLive *l = np_live_add(self, ch, NULL, def); return def >= 0 ? l : NULL;
}

// ----- npc_set_target / npc_set_faction / faction_can_target / npc_archetype
static int api_p_npc_set_target(const AbsApi *self, void *ch, void *target) {
    const char *who = plug_name_of(plug_of(self)); int ok = 0;
    if (!tk_ptr_ok(ch) || !api_p_is_game_thread(self)) { logf_("plugin %s: npc_set_target refused - not a character / not on the game thread", who); return 0; }
    __try {
        void **aic = (void **)api_p_prop_ptr(self, ch, "m_AIComponent"), *f = aic && tk_ptr_ok(*aic) ? api_p_find_function(self, *aic, "BPF_ForceEnemy") : NULL; void *args[1] = { &target };
        ok = f && api_p_call_params(self, *aic, f, args, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: npc_set_target: EXCEPTION 0x%08lX", who, GetExceptionCode()); ok = 0; }
    logf_("plugin %s: npc_set_target(%p -> %p) -> %d", who, ch, target, ok); return ok;
}
static int api_p_npc_set_faction(const AbsApi *self, void *ch, int faction) {
    const char *who = plug_name_of(plug_of(self)); int r = -1;
    if (!tk_ptr_ok(ch) || faction < 0 || faction > 5 || !api_p_is_game_thread(self)) { logf_("plugin %s: npc_set_faction: needs a character, a faction 0..5 and the game thread", who); return -1; }
    __try {
        void *f = api_p_find_function(self, ch, "ServerSetFaction"); unsigned char v = (unsigned char)faction; void *args[1] = { &v }; if (f) api_p_call_params(self, ch, f, args, NULL);
        unsigned char *fp = (unsigned char *)api_p_prop_ptr(self, ch, "m_eFaction");
        if (fp && *fp != v) { logf_("plugin %s: npc_set_faction: ServerSetFaction left faction %d - written directly", who, *fp); *fp = v; }
        r = fp ? *fp : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: npc_set_faction: EXCEPTION 0x%08lX", who, GetExceptionCode()); r = -1; }
    logf_("plugin %s: npc_set_faction(%p, %d) -> %d", who, ch, faction, r); return r;
}
static int api_p_faction_can_target(const AbsApi *self, int a, int b, int set) {    // UThePlainesGameInstance::m_FactionsManager -> m_FactionsTargetTable (6 x 6 bytes, row = the attacker's faction)
    int ix = np_faction_index(a, b), r = -1; if (ix < 0) return -1;
    __try {
        void *w = api_p_get_world(self); void **fm = w ? (void **)api_p_prop_ptr_path(self, w, "OwningGameInstance.m_FactionsManager") : NULL;
        uint8_t *t = fm && tk_ptr_ok(*fm) ? (uint8_t *)api_p_prop_ptr(self, *fm, "m_FactionsTargetTable") : NULL;
        if (t) { if (set == 0 || set == 1) { t[ix] = (uint8_t)set; logf_("plugin %s: faction_can_target(%d -> %d) set to %d", plug_name_of(plug_of(self)), a, b, set); } r = t[ix] != 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
static void *api_p_npc_archetype(const AbsApi *self, void *ch) {
    void *r = NULL; if (!tk_ptr_ok(ch)) return NULL;
    __try { void **aic = (void **)api_p_prop_ptr(self, ch, "m_AIComponent"); void **p = aic && tk_ptr_ok(*aic) ? (void **)api_p_prop_ptr_path(self, *aic, "m_ArchetypeDescription.m_Archetype") : NULL; r = p && tk_ptr_ok(*p) ? *p : NULL; } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    return r;
}

// ----- npc_spawn / npc_remove
// The engage sequence (without it, NPCs of the npcs kind can miss the player until a later BPF_ForceEnemy): UAIFightingComponent::BPF_ForceEnemy(player) (sets m_ForcedEnemy, which the
// behaviour tree's UpdateBlackBoard service copies into the Enemy key) PLUS the controller's blackboard key Enemy written directly (UBlackboardComponent::SetValueAsObject("Enemy", player)). Both are
// idempotent. Every call logs the NPC's distance and whether each step took, so a missed engage is visible. Used by `aggro` after the spawn (repeated: the first calls can be dropped while InitAI /
// the behaviour tree are still starting) and when the player walks into the NPC's on_area; npc_set_target (ForceEnemy only) stays for manual use.
static int np_engage(const AbsApi *s, void *ch, const char *why) {
    void *p = np_player(s); const char *id = "?"; for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch && g_npl[i].def >= 0) id = g_np[g_npl[i].def].id;
    if (!p || !tk_ptr_ok(ch)) { logf_("npcs: %s engage (%s): no player / no character yet", id, why); return 0; }
    int a = api_p_npc_set_target(s, ch, p), b = 0; float nl[3] = { 0, 0, 0 }, pl[3] = { 0, 0, 0 }; void *focus = NULL, *tgt_before = NULL; int alerted = -1, tgt_set = -1;
    __try {
        void *ctl = se_comp(s, ch, "Controller"); void **bb = ctl ? (void **)api_p_prop_ptr(s, ctl, "m_BlackboardComp") : NULL;
        if (bb && tk_ptr_ok(*bb)) {
            void *fn = api_p_find_function(s, *bb, "SetValueAsObject"); uint64_t key = 0; void *actor = p;
            if (fn && api_p_fname_make(s, "Enemy", &key) == 0) { void *args[2] = { &key, &actor }; b = api_p_call_params(s, *bb, fn, args, NULL); }
        }
        void *ff = ctl ? api_p_find_function(s, ctl, "GetFocusActor") : NULL; if (ff) api_p_call_params(s, ctl, ff, NULL, &focus);
        void **aic = (void **)api_p_prop_ptr(s, ch, "m_AIComponent"); if (aic && tk_ptr_ok(*aic)) api_p_get_bool(s, *aic, "m_bAlertedOnce", &alerted);
        // the character's own lock target (ABaseFightingCharacter::GetTarget, native): UMoveStatusBTService only switches to Combat (and the AI only attacks) with one. Bots spawned
        // after an earlier one in the same world can have Enemy + ticket but no target and never attack; SetTarget(player) (UFunction) gives them one (bot trials)
        static void *(*get_target)(void *); if (!get_target) { uint32_t rva = api_p_find_symbol(s, "ABaseFightingCharacter::GetTarget"); if (rva) get_target = (void *(*)(void *))(g_base + rva); }
        tgt_before = get_target ? get_target(ch) : (void *)-1;
        if (tgt_before != p) { void *sf = api_p_find_function(s, ch, "SetTarget"), *pp = p; void *args1[1] = { &pp }; tgt_set = sf && api_p_call_params(s, ch, sf, args1, NULL); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("npcs: %s engage: EXCEPTION 0x%08lX", id, GetExceptionCode()); }
    api_p_test_get_location(s, ch, nl); api_p_test_get_location(s, p, pl);
    float dx = nl[0] - pl[0], dy = nl[1] - pl[1];
    logf_("npcs: %s %p engage (%s): ForceEnemy %d, blackboard Enemy %d, distance %.0f uu, focus %s, alerted %d, lock target %s%s", id, ch, why, a, b, sqrtf(dx * dx + dy * dy), focus == p ? "player" : focus ? "other" : "none", alerted,
          tgt_before == p ? "player" : tgt_before == (void *)-1 ? "?" : tgt_before ? "OTHER" : "NONE", tgt_set < 0 ? "" : tgt_set ? " -> SetTarget(player) 1" : " -> SetTarget(player) FAILED");
    return a || b;
}
static void bt_on_spawned(void *ch, const char *why);                             // bots.inc: a bot's guide stat points on the spawned pawn
static void np_post_spawn(void *u) {                                              // 1.5 / 4 / 8 / 15 / 30 s after the spawn: the entry's faction and aggro
    void *ch = u; NpLive *l = NULL; for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch) l = &g_npl[i];
    if (!l || l->def < 0 || !g_np_api) return;
    CNpc *n = &g_np[l->def];
    if (!l->stats_done) { l->stats_done = 1; bt_on_spawned(ch, "1.5 s after the spawn"); }
    if (n->faction >= 0) api_p_npc_set_faction(g_np_api, ch, (int)n->faction);
    if (n->aggro) np_engage(g_np_api, ch, "after spawn");
}
static int api_p_npc_remove(const AbsApi *self, void *ch);
static void np_fall_check(void *u) {                                              // 1.2 s after a spawn at a given spot: an NPC that is already far below it fell through (no ground loaded there): remove it instead of letting it die at the kill plane
    void *ch = u; NpLive *l = NULL; for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch) l = &g_npl[i];
    if (!l || !g_np_api) return;
    float p[3]; if (!api_p_test_get_location(g_np_api, ch, p) || p[2] > l->z0 - 300.f) return;
    logf_("npcs: WARNING the NPC %p fell from z %.0f to %.0f - there is no ground at its spawn spot (the area is not loaded, or the spot is over a void); it is removed%s", ch, l->z0, p[2], l->def >= 0 ? " and may spawn again when the player re-enters the spawn point" : "");
    if (l->def >= 0) g_np[l->def].spawned_world = NULL;
    api_p_npc_remove(g_np_api, ch);
}
static void *api_p_npc_spawn(const AbsApi *self, const char *arch, const float loc[3], const float *yaw, const char *cls) {
    const char *who = plug_name_of(plug_of(self)); if (!g_np_api) g_np_api = self;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: npc_spawn refused - not on the game thread", who); return NULL; }
    if (!arch || !*arch) { logf_("plugin %s: npc_spawn: needs an archetype path (/Game/DB/AI/NPCs/Trickster/Trickster.Trickster) or the id of an npcs entry", who); return NULL; }
    char path[300]; int def = -1;
    if (arch[0] == '/') { snprintf(path, sizeof path, "%s", arch); def = np_find_def_by_path(path); }
    else { for (int i = 0; i < g_np_n; i++) if (!strcmp(g_np[i].id, arch)) def = i; if (def < 0) { logf_("plugin %s: npc_spawn: no npcs entry '%s'", who, arch); return NULL; } snprintf(path, sizeof path, "%s", g_np[def].path); }
    if (def >= 0 && (!cls || !*cls) && g_np[def].cls[0]) { cls = g_np[def].cls; logf_("plugin %s: npc_spawn: character class %s from the npcs entry %s", who, cls, g_np[def].id); }   // the entry's "class" (e.g. the practice dummy)
    void *ch = NULL, *sp = NULL; int rc = 0;
    __try { rc = L_npc_spawn_at(path, who, cls ? cls : "", loc, yaw, &ch, &sp); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: npc_spawn: EXCEPTION 0x%08lX", who, GetExceptionCode()); rc = 0; ch = NULL; }
    if (!rc || !ch) { logf_("plugin %s: npc_spawn(%s) produced nothing (see the npc: lines above)", who, path); return NULL; }
    { NpLive *l = np_live_add(self, ch, sp, def); if (loc && l) { l->z0 = loc[2]; api_p_set_timer_id(self, 1.2f, 0, np_fall_check, ch); } }
    bt_on_spawned(ch, "at the spawn");                                              // a bot's stat points (no-op for other NPCs)
    if (!g_ar_hooked) { g_ar_hooked = 1; api_p_on_pawn_destroyed(self, np_destroyed, NULL); }
    if (def >= 0 && (g_np[def].faction >= 0 || g_np[def].aggro)) { static const float at[5] = { 1.5f, 4.f, 8.f, 15.f, 30.f }; for (int k = 0; k < 5; k++) api_p_set_timer_id(self, at[k], 0, np_post_spawn, ch); }   // ForceEnemy right after InitAI can be dropped: repeated, harmless
    logf_("plugin %s: npc_spawn(%s%s) -> %p%s", who, path, loc ? " at the given spot" : " 350 uu ahead", ch, def >= 0 ? " (entry reactions bound)" : "");
    return ch;
}
// ----- attack tickets: UThePlainesGameInstance+0x6810 = UTicketManager. +0x48 TArray<Ticketable> (stride 0x20.
// +0 FWeakObjectPtr target, +8 TArray<Attacker> (stride 0x20, +0 FWeakObjectPtr attacker pawn), +0x18 FWeakObjectPtr holder = the pawn allowed to attack that target). RequestTicket
// gives a free ticket only to attackers[0]; it is freed by ReleaseTicket (mood change, BT death task) and by UnRegisterForTarget (UAIFightingComponent::EndPlay, target change).
// np_ticket_release calls UnRegisterForTarget(pawn, target) for every target the pawn is listed for, BEFORE the pawn is removed (frees the ticket even if EndPlay skips it).
static void *np_weak(const int *w) {                                                // FWeakObjectPtr {index, serial} -> live object or NULL (GUObjectArray item: obj +0, flags +8, serial +0x10)
    uint8_t *arr = g_base + RVA_GUOBJECTARRAY, *items = *(uint8_t **)(arr + 0x10); int count = *(int32_t *)(arr + 0x1C);
    if (!items || w[0] < 0 || w[0] >= count) return NULL;
    uint8_t *it = items + (size_t)w[0] * 24; if (*(int *)(it + 0x10) != w[1] || (*(uint32_t *)(it + 8) & (1u << 29))) return NULL;   // stale serial / PendingKill
    return *(void **)it;
}
static uint8_t *np_ticket_mgr(const AbsApi *s) {
    void *w = api_p_get_world(s); void **gi = w ? (void **)api_p_prop_ptr(s, w, "OwningGameInstance") : NULL;
    if (!gi || !tk_ptr_ok(*gi) || !api_p_is_a(s, *gi, "ThePlainesGameInstance")) return NULL;
    uint8_t *m = *(uint8_t **)((uint8_t *)*gi + 0x6810); return tk_ptr_ok(m) && api_p_is_a(s, m, "TicketManager") ? m : NULL;
}
static void np_ticket_dump(const AbsApi *s, const char *why) {                    // one log line per target: holder + attackers (names), to see a ticket held by a gone / dead NPC
    __try {
        uint8_t *m = np_ticket_mgr(s); if (!m) { logf_("tickets (%s): no ticket manager", why); return; }
        uint8_t *t = *(uint8_t **)(m + 0x48); int n = *(int *)(m + 0x50); logf_("tickets (%s): %d target(s)", why, n);
        for (int i = 0; i < n && i < 8; i++) {
            uint8_t *e = t + i * 0x20; char tn[64] = "-", hn[64] = "none", line[600]; int len = 0; void *to = np_weak((int *)e), *ho = np_weak((int *)(e + 0x18));
            if (to) api_p_object_name(s, to, tn, sizeof tn);
            if (ho) api_p_object_name(s, ho, hn, sizeof hn); else if (*(int *)(e + 0x18) >= 0) snprintf(hn, sizeof hn, "STALE #%d", *(int *)(e + 0x18));
            uint8_t *a = *(uint8_t **)(e + 8); int na = *(int *)(e + 0x10); line[0] = 0;
            for (int k = 0; k < na && k < 8 && len < 500; k++) { char an[64] = "gone"; void *ao = np_weak((int *)(a + k * 0x20)); if (ao) api_p_object_name(s, ao, an, sizeof an); len += snprintf(line + len, sizeof line - len, "%s%s", k ? ", " : "", an); }
            logf_("tickets (%s): target %s: holder %s, %d attacker(s) [%s]", why, tn, hn, na, line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("tickets (%s): EXCEPTION 0x%08lX", why, GetExceptionCode()); }
}
static int np_ticket_release(const AbsApi *s, void *ch, const char *why) {         // returns the number of targets the pawn was unregistered from
    static void (*unreg)(void *, void *); int done = 0;
    if (!unreg) { uint32_t rva = api_p_find_symbol(s, "UTicketManager::UnRegisterForTarget"); if (rva) unreg = (void (*)(void *, void *))(g_base + rva); }
    if (!unreg || !tk_ptr_ok(ch)) return 0;
    __try {
        uint8_t *m = np_ticket_mgr(s); void *targets[8]; int nt = 0;
        if (m) { uint8_t *t = *(uint8_t **)(m + 0x48); int n = *(int *)(m + 0x50);
            for (int i = 0; i < n && nt < 8; i++) { uint8_t *e = t + i * 0x20, *a = *(uint8_t **)(e + 8); int na = *(int *)(e + 0x10), listed = np_weak((int *)(e + 0x18)) == ch;
                for (int k = 0; k < na && !listed; k++) listed = np_weak((int *)(a + k * 0x20)) == ch;
                void *to = np_weak((int *)e); if (listed && to) targets[nt++] = to; } }
        for (int i = 0; i < nt; i++) { unreg(ch, targets[i]); done++; }                 // UnRegisterForTargetInternal: drops the pawn from the attackers and frees the ticket it holds
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("tickets: release %p (%s): EXCEPTION 0x%08lX", ch, why, GetExceptionCode()); }
    if (done) logf_("tickets: %p unregistered from %d target(s) (%s)", ch, done, why);
    return done;
}
// 1 if the pawn is in the attackers list of `target` (2 = and holds its ticket), 0 = not listed, -1 = no ticket manager / target not tracked
static int np_ticket_state(const AbsApi *s, void *ch, void *target) {
    int r = -1;
    __try { uint8_t *m = np_ticket_mgr(s); if (m) { uint8_t *t = *(uint8_t **)(m + 0x48); int n = *(int *)(m + 0x50);
        for (int i = 0; i < n; i++) { uint8_t *e = t + i * 0x20, *a = *(uint8_t **)(e + 8); int na = *(int *)(e + 0x10); if (np_weak((int *)e) != target) continue;
            r = 0; for (int k = 0; k < na; k++) if (np_weak((int *)(a + k * 0x20)) == ch) r = 1;
            if (np_weak((int *)(e + 0x18)) == ch) r = 2; } } } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
// UTicketManager::RegisterForTarget(pawn, target) (what UAIFightingComponent::TargetChangedDelegate calls): a pawn missing from the list never gets the ticket (RequestTicket only grants attackers[0])
static int np_ticket_register(const AbsApi *s, void *ch, void *target) {
    static void (*reg)(void *, void *); if (!reg) { uint32_t rva = api_p_find_symbol(s, "UTicketManager::RegisterForTarget"); if (rva) reg = (void (*)(void *, void *))(g_base + rva); }
    if (!reg || !tk_ptr_ok(ch) || !tk_ptr_ok(target)) return 0;
    __try { reg(ch, target); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("tickets: register %p: EXCEPTION 0x%08lX", ch, GetExceptionCode()); return 0; }
    return 1;
}
static int api_p_npc_remove(const AbsApi *self, void *ch) {
    const char *who = plug_name_of(plug_of(self)); int ok = 0;
    if (!tk_ptr_ok(ch) || !api_p_is_game_thread(self)) { logf_("plugin %s: npc_remove refused - not a character / not on the game thread", who); return 0; }
    void *sp = NULL; for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == ch) sp = g_npl[i].sp;
    np_ticket_release(self, ch, "npc_remove");                                      // a removed bot must not keep the attack ticket on the player (a later bot would never attack)
    __try {
        if (sp) ok = L_npc_unspawn(sp);                                          // only acts when it is one of the tracked spawners
        if (!ok) { void *f = api_p_find_function(self, ch, "K2_DestroyActor"); ok = f && api_p_call_params(self, ch, f, NULL, NULL); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: npc_remove: EXCEPTION 0x%08lX", who, GetExceptionCode()); ok = 0; }
    np_forget_char(ch); logf_("plugin %s: npc_remove(%p) -> %d (%s)", who, ch, ok, sp ? "spawner unspawned" : "actor destroyed"); return ok;
}

// ================================================================ part 2: content kind "npcs"
static const char *const NP_KNOWN[] = { "id", "base", "class", "name","health", "hp", "style", "faction", "deck", "ai", "props", "aggro", "spawn", "on_health_below", "on_death", "on_area", "on_player_near", "weapon", "gear", "ai_level", NULL };
static int np_read_acts(const JDoc *d, int node, char acts[6][120], int *n) {         // "do": ["log ...", "heal", ...]; returns 0 and sets nothing on a bad action (err text by the caller)
    *n = 0; int dc = jfind(d, node, "do"); if (dc < 0) return 1; if (d->n[dc].t != J_ARR) return 0;
    for (int c = d->n[dc].first; c >= 0; c = d->n[c].next) { if (d->n[c].t != J_STR || np_action(d->n[c].str) == NPA_BAD || *n >= 6 || strlen(d->n[c].str) >= 120) return 0; snprintf(acts[(*n)++], 120, "%s", d->n[c].str); }
    return 1;
}
static int npcs_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    const char *base = cs_str(&d, 0, "base"), *id = cs_str(&d, 0, "id"), *v; int ok = 1, dk = jfind(&d, 0, "deck"), ai = jfind(&d, 0, "ai"), pr = jfind(&d, 0, "props"), sp = jfind(&d, 0, "spawn"); char a[6][120]; int na;
    double hp = (v = cs_str(&d, 0, "health")) || (v = cs_str(&d, 0, "hp")) ? atof(v) : -1, st = (v = cs_str(&d, 0, "style")) ? atof(v) : -1, fa = (v = cs_str(&d, 0, "faction")) ? atof(v) : -1;
    double cd = ai >= 0 && (v = cs_str(&d, ai, "attack_cooldown")) ? atof(v) : -1;
    if (!base || base[0] != '/' || !strchr(base, '.')) { snprintf(err, cap, "needs \"base\": the stock ArchetypeAsset to copy, like /Game/DB/AI/NPCs/Trickster/Trickster.Trickster"); ok = 0; }
    else if ((v = cs_str(&d, 0, "class")) && (v[0] != '/' || !strchr(v, '.') || strlen(v) >= 200)) { snprintf(err, cap, "\"class\" must be a character class path like /Game/Blueprints/AI/BP_AICharacter_Training.BP_AICharacter_Training_C"); ok = 0; }
    else if (!np_check_numbers(hp, st, fa, cd, err, cap)) ok = 0;
    else if (pr >= 0 && d.n[pr].t != J_OBJ) { snprintf(err, cap, "\"props\" must be an object of ArchetypeAsset property -> engine text"); ok = 0; }
    else if (ai >= 0 && d.n[ai].t != J_OBJ) { snprintf(err, cap, "\"ai\" must be an object (attack_cooldown, can_attack, can_defend, xp_bonus)"); ok = 0; }
    else if (dk >= 0 && d.n[dk].t != J_OBJ) { snprintf(err, cap, "\"deck\" must be an object: FrontLeft / FrontRight / BackRight / BackLeft / Alt -> array of attack rows, and optionally copy_of"); ok = 0; }
    else if (sp >= 0 && (d.n[sp].t != J_OBJ || jfind(&d, sp, "loc") < 0 || d.n[jfind(&d, sp, "loc")].t != J_ARR)) { snprintf(err, cap, "\"spawn\" must be an object with \"loc\": [x, y, z] (optional map, yaw, near, delay)"); ok = 0; }
    if (ok && dk >= 0) for (int c = d.n[dk].first; c >= 0 && ok; c = d.n[c].next) {
        if (d.n[c].key && !strcmp(d.n[c].key, "copy_of")) { if (d.n[c].t != J_STR || d.n[c].str[0] != '/' || !strchr(d.n[c].str, '.')) { snprintf(err, cap, "deck.copy_of must be an AttacksComboDB asset path like /Game/DB/AI/NPCs/Trickster/Trickster_CD.Trickster_CD"); ok = 0; } continue; }
        int k = np_deck_key(d.n[c].key), n = 0;
        if (k < 0 || d.n[c].t != J_ARR) { snprintf(err, cap, "deck.%s: the keys are FrontLeft, FrontRight, BackRight, BackLeft, Alt (arrays of attack rows) and copy_of", d.n[c].key ? d.n[c].key : "?"); ok = 0; break; }
        for (int r = d.n[c].first; r >= 0; r = d.n[r].next, n++) if (d.n[r].t != J_STR || !np_row_ok(d.n[r].str)) { snprintf(err, cap, "deck.%s: attack rows are short names like \"078\"", d.n[c].key); ok = 0; break; }
        if (ok && (n > (k == 4 ? 4 : 8))) { snprintf(err, cap, "deck.%s has too many rows (Alt takes 4, a quadrant 8)", d.n[c].key); ok = 0; }
    }
    static const char *const rk[] = { "on_health_below", "on_death", "on_area", "on_player_near" };
    for (int i = 0; i < 4 && ok; i++) { int c = jfind(&d, 0, rk[i]); if (c < 0) continue; if (d.n[c].t != J_OBJ || !np_read_acts(&d, c, a, &na)) { snprintf(err, cap, "%s: needs an object with \"do\": a list of log <text> / message <text> / heal / ai on / ai off / target player / faction <0-5> / remove (at most 6)", rk[i]); ok = 0; } }
    if (ok && jfind(&d, 0, "on_health_below") >= 0) { v = cs_str(&d, jfind(&d, 0, "on_health_below"), "fraction"); if (!v || atof(v) <= 0 || atof(v) > 1) { snprintf(err, cap, "on_health_below needs \"fraction\" between 0 and 1"); ok = 0; } }
    for (int i = 2; i < 4 && ok; i++) { int c = jfind(&d, 0, rk[i]); if (c >= 0 && ((v = cs_str(&d, c, "radius")) == NULL || atof(v) <= 0 || atof(v) > 100000)) { snprintf(err, cap, "%s needs \"radius\" (uu, 1..100000)", rk[i]); ok = 0; } }
    if (ok) { cs_warn_unknown(&d, "npcs", id ? id : "?", NP_KNOWN); for (const char *w[] = { "weapon", "gear", "ai_level", NULL }, **q = w; *q; q++) if (jfind(&d, 0, *q) >= 0) logf_("content: npcs '%s': \"%s\" is not supported - ignored", id ? id : "?", *q); }
    jdoc_free(&d); return ok;
}
static void np_fill_acts(const JDoc *d, int node, CNpc *n, int which) { np_read_acts(d, node, n->acts[which], &n->nacts[which]); }
static int npcs_parse(CNpc *n, const char *mod, const char *id, const char *json) {
    JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    memset(n, 0, sizeof *n); snprintf(n->mod, sizeof n->mod, "%s", mod); snprintf(n->id, sizeof n->id, "%s", id); cs_sanitize(mod, n->ms, sizeof n->ms); np_asset_path(n->ms, id, n->path, sizeof n->path);
    n->health = n->style = n->faction = n->cooldown = -1; n->can_attack = n->can_defend = -1; const char *v;
    if ((v = cs_str(&d, 0, "base"))) snprintf(n->base, sizeof n->base, "%s", v);
    if ((v = cs_str(&d, 0, "class"))) snprintf(n->cls, sizeof n->cls, "%s", v);
    if ((v = cs_str(&d, 0, "name"))) snprintf(n->name, sizeof n->name, "%s", v);
    if ((v = cs_str(&d, 0, "health")) || (v = cs_str(&d, 0, "hp"))) n->health = atof(v);
    if ((v = cs_str(&d, 0, "style"))) n->style = atof(v);
    if ((v = cs_str(&d, 0, "faction"))) n->faction = atof(v);
    n->aggro = cs_true(&d, 0, "aggro");
    int ai = jfind(&d, 0, "ai");
    if (ai >= 0) { if ((v = cs_str(&d, ai, "attack_cooldown"))) n->cooldown = atof(v); if (cs_str(&d, ai, "can_attack")) n->can_attack = cs_true(&d, ai, "can_attack"); if (cs_str(&d, ai, "can_defend")) n->can_defend = cs_true(&d, ai, "can_defend");
                   if ((v = cs_str(&d, ai, "xp_bonus"))) { snprintf(n->pk[n->nprops], 64, "m_iXPBonus"); snprintf(n->pv[n->nprops++], 200, "%d", atoi(v)); } }
    int pr = jfind(&d, 0, "props");
    if (pr >= 0) for (int c = d.n[pr].first; c >= 0 && n->nprops < 16; c = d.n[c].next) if (d.n[c].key && d.n[c].str) { snprintf(n->pk[n->nprops], 64, "%s", d.n[c].key); snprintf(n->pv[n->nprops++], 200, "%s", d.n[c].str); }
    int dk = jfind(&d, 0, "deck");
    if (dk >= 0) {
        n->has_deck = 1;
        for (int c = d.n[dk].first; c >= 0; c = d.n[c].next) {
            if (d.n[c].key && !strcmp(d.n[c].key, "copy_of")) { snprintf(n->deck_copy, sizeof n->deck_copy, "%s", d.n[c].str); continue; }
            int k = np_deck_key(d.n[c].key); if (k < 0) continue;
            for (int r = d.n[c].first; r >= 0 && n->nrows[k] < 8; r = d.n[r].next) snprintf(n->rows[k][n->nrows[k]++], 16, "%s", d.n[r].str);
        }
    }
    int c;
    if ((c = jfind(&d, 0, "on_health_below")) >= 0) { n->hp_frac = atof(cs_str(&d, c, "fraction")); np_fill_acts(&d, c, n, 0); }
    if ((c = jfind(&d, 0, "on_death")) >= 0) np_fill_acts(&d, c, n, 1);
    if ((c = jfind(&d, 0, "on_area")) < 0) c = jfind(&d, 0, "on_player_near");
    if (c >= 0) { n->area_r = atof(cs_str(&d, c, "radius")); np_fill_acts(&d, c, n, 2); }
    int sp = jfind(&d, 0, "spawn");
    if (sp >= 0) {
        int lc = jfind(&d, sp, "loc"), i = 0; for (int e = d.n[lc].first; e >= 0 && i < 3; e = d.n[e].next, i++) n->sloc[i] = (float)atof(d.n[e].str);
        n->has_spawn = i == 3; n->near_r = (v = cs_str(&d, sp, "near")) ? atof(v) : 1500; n->yaw = (v = cs_str(&d, sp, "yaw")) ? atof(v) : 0; n->delay = (v = cs_str(&d, sp, "delay")) ? atof(v) : 4; if ((v = cs_str(&d, sp, "map"))) snprintf(n->smap, sizeof n->smap, "%s", v);
    }
    jdoc_free(&d); return 1;
}

// reactions
static void np_remove_job(void *ch) { if (g_np_api) api_p_npc_remove(g_np_api, ch); }
static void np_run_acts(NpLive *l, int which, const char *why) {
    CNpc *n = &g_np[l->def]; const AbsApi *s = CA ? CA : g_np_api;
    for (int i = 0; i < n->nacts[which]; i++) {
        const char *a = n->acts[which][i]; int k = np_action(a);
        switch (k) {
        case NPA_LOG:     logf_("npcs: %s/%s %s: %s", n->mod, n->id, why, a + 4); break;
        case NPA_MESSAGE: logf_("npcs: %s/%s %s: message '%s'", n->mod, n->id, why, a + 8); np_show(a + 8, 4); break;
        case NPA_HEAL:    logf_("npcs: %s/%s %s: heal -> %d", n->mod, n->id, why, api_p_test_heal(s, l->ch)); break;
        case NPA_AI_ON:
        case NPA_AI_OFF:  logf_("npcs: %s/%s %s: %s -> %d", n->mod, n->id, why, a, api_p_test_ai(s, l->ch, k == NPA_AI_ON)); break;
        case NPA_TARGET_PLAYER: { void *p = np_player(s); logf_("npcs: %s/%s %s: target player -> %d", n->mod, n->id, why, p ? api_p_npc_set_target(s, l->ch, p) : 0); break; }
        case NPA_FACTION: logf_("npcs: %s/%s %s: %s -> %d", n->mod, n->id, why, a, api_p_npc_set_faction(s, l->ch, a[8] - '0')); break;
        case NPA_REMOVE:  logf_("npcs: %s/%s %s: remove (next tick)", n->mod, n->id, why); s->run_on_game_thread(s, np_remove_job, l->ch); break;
        }
    }
}
static void np_on_hit(const AbsHitInfo *h, void *u) {
    (void)u; NpLive *l = np_live_of(CA, h->victim); if (!l || l->hp_fired || g_np[l->def].hp_frac <= 0) return;
    AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; if (!api_p_char_info(CA, l->ch, &ci)) return;
    if (np_hp_below(ci.health, ci.max_health, g_np[l->def].hp_frac)) { l->hp_fired = 1; logf_("npcs: %s on_health_below %.2f: %.0f/%.0f after %s", g_np[l->def].id, g_np[l->def].hp_frac, ci.health, ci.max_health, h->attack); np_run_acts(l, 0, "on_health_below"); }
}
static void np_on_death(void *victim, void *killer, int id, const char *attack, void *u) {
    (void)killer; (void)id; (void)u; NpLive *l = np_live_of(CA, victim); if (!l) return;
    logf_("npcs: %s on_death (%s)", g_np[l->def].id, attack ? attack : "?"); np_ticket_release(CA, victim, "death"); np_run_acts(l, 1, "on_death");
}
static void np_area_cb(void *who, int entered, void *user) {                      // an NPC's own on_area: the player walks into its radius
    (void)who; if (!entered) return; NpLive *l = NULL; for (int i = 0; i < g_npl_n; i++) if (g_npl[i].ch == user) l = &g_npl[i];
    if (!l || l->def < 0) return; logf_("npcs: %s on_area (player within %.0f uu)", g_np[l->def].id, g_np[l->def].area_r);
    if (g_np[l->def].aggro && CA) np_engage(CA, l->ch, "on_area enter"); np_run_acts(l, 2, "on_area");
}
static void np_area_attach(const AbsApi *self, NpLive *l) {
    CNpc *n = &g_np[l->def]; if (n->area_r <= 0 || (n->nacts[2] == 0 && !n->aggro)) return;
    l->area = api_p_on_area(CA ? CA : self, NULL, l->ch, (float)n->area_r, NULL, np_area_cb, l->ch);
}
// spawn point: the player comes within `near` of the spot -> one NPC per world (a later visit does not add more)
static void np_spawn_timer(void *user) {                                          // spawn.delay seconds after the player came near: the ground around the spot needs time to stream in after a teleport
    CNpc *n = &g_np[(int)(intptr_t)user]; void *w = api_p_get_world(CA); if (w != n->spawned_world) return;      // another world meanwhile
    float yaw = (float)n->yaw; logf_("npcs: %s: spawning at the spawn spot", n->id);
    if (!api_p_npc_spawn(CA, n->path, n->sloc, &yaw, "")) n->spawned_world = NULL;
}
static void np_spawn_area_cb(void *who, int entered, void *user) {
    (void)who; if (!entered) return; CNpc *n = &g_np[(int)(intptr_t)user]; void *w = api_p_get_world(CA); char wn[128] = "";
    if (w) api_p_object_name(CA, w, wn, sizeof wn);
    if (n->smap[0] && !strstr(wn, n->smap)) return;
    if (n->spawned_world == w) return;
    n->spawned_world = w;
    logf_("npcs: %s: the player is near the spawn spot (world %s) - spawning in %.1f s", n->id, wn, n->delay);
    api_p_set_timer_id(CA, n->delay > 0.1 ? (float)n->delay : 0.1f, 0, np_spawn_timer, user);
}
static void np_apply_spawn(CNpc *n, int idx) {
    if (n->area_h) { api_p_area_remove(CA, n->area_h); n->area_h = 0; }
    if (n->has_spawn) { n->area_h = api_p_on_area(CA, n->sloc, NULL, (float)n->near_r, NULL, np_spawn_area_cb, (void *)(intptr_t)idx); logf_("npcs: %s: spawn point %.0f %.0f %.0f (spawns when the player is within %.0f uu%s%s)", n->id, n->sloc[0], n->sloc[1], n->sloc[2], n->near_r, n->smap[0] ? " in " : "", n->smap); }
}

static int npcs_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; static CNpc nn; static char txt[1400];
    if (!npcs_parse(&nn, mod, id, json)) { logf_("npcs: %s/%s: entry could not be parsed", mod, id); return 0; }
    int idx = -1; for (int i = 0; i < g_np_n; i++) if (!strcmp(g_np[i].mod, mod) && !strcmp(g_np[i].id, id)) idx = i;
    if (idx >= 0 && g_reloading && (strcmp(g_np[idx].base, nn.base) || strcmp(g_np[idx].deck_copy, nn.deck_copy) || g_np[idx].has_deck != nn.has_deck)) { g_reload_blocked++; logf_("content: reload: npc %s/%s: base / deck.copy_of changed - that needs a restart; this entry was not changed", mod, id); return 0; }
    void *src = CA->find_object(CA, nn.base); if (!src) src = CA->load_object(CA, nn.base);
    if (!src || !CA->is_a(CA, src, "ArchetypeAsset")) { logf_("npcs: %s: base %s is %s - retried before the save loads", id, nn.base, src ? "not an ArchetypeAsset" : "not found"); return 1; }
    void *arch = CA->create_asset(CA, src, nn.path); if (!arch) { logf_("npcs: %s: create_asset(%s) failed", id, nn.path); return 1; }
    if (idx < 0) { if (g_np_n >= 32) { logf_("npcs: too many npcs entries (32), %s ignored", id); return 0; } idx = g_np_n++; memset(&g_np[idx], 0, sizeof g_np[idx]); }
    int area_h = g_np[idx].area_h; void *sw = g_np[idx].spawned_world; g_np[idx] = nn; g_np[idx].obj = arch; g_np[idx].area_h = area_h; g_np[idx].spawned_world = sw;
    CNpc *n = &g_np[idx]; int ok = 0, bad = 0;
    #define NSET(k, fmt, val) do { snprintf(txt, sizeof txt, fmt, val); int r_ = CA->set_property_text(CA, arch, k, txt); ok += r_; bad += !r_; logf_("npcs: %s: %s = %s -> %d", id, k, txt, r_); } while (0)
    if (n->health >= 0) NSET("m_fHealth", "%g", n->health);
    if (n->style >= 0) NSET("m_uiCombatStyle", "%d", (int)n->style);
    if (n->cooldown >= 0) NSET("m_fAttackCooldown", "%g", n->cooldown);
    if (n->can_attack >= 0) NSET("m_bCanAttack", "%s", n->can_attack ? "True" : "False");
    if (n->can_defend >= 0) NSET("m_bCanDefend", "%s", n->can_defend ? "True" : "False");
    for (int i = 0; i < n->nprops; i++) NSET(n->pk[i], "%s", n->pv[i]);
    #undef NSET
    if (n->has_deck) {                                                           // own copy of the combo DB (the stock deck is shared by every NPC of that archetype) with the rows of the entry
        void **dbs = (void **)CA->prop_ptr(CA, arch, "m_ComboDBPerWeapon"), *dsrc = NULL;       // Object<AttacksComboDB>[3]: 0 bare hands, 1 sword, 2 wargloves
        if (n->deck_copy[0]) { dsrc = CA->find_object(CA, n->deck_copy); if (!dsrc) dsrc = CA->load_object(CA, n->deck_copy); } else dsrc = dbs ? dbs[0] : NULL;
        char dpath[320]; snprintf(dpath, sizeof dpath, "/Game/Mods/%s/%s_Deck.%s_Deck", n->ms, id, id);
        void *deck = dsrc ? CA->create_asset(CA, dsrc, dpath) : NULL;
        if (!deck) logf_("npcs: %s: no bare-hands combo DB to copy (%s) - deck skipped", id, n->deck_copy[0] ? n->deck_copy : "the base has none");
        else {
            int len = snprintf(txt, sizeof txt, "("), parts = 0;
            for (int k = 0; k < 5; k++) if (n->nrows[k]) { const char *rows[8]; for (int r = 0; r < n->nrows[k]; r++) rows[r] = n->rows[k][r]; len = np_deck_part(txt, len, sizeof txt, parts++, k, rows, n->nrows[k]); }
            snprintf(txt + len, sizeof txt - len, ")");
            int sok = CA->set_property_text(CA, deck, "m_Struct", txt); if (sok && dbs) dbs[0] = deck;
            char back[900] = "?"; CA->get_property_text(CA, deck, "m_Struct", back, sizeof back);
            logf_("npcs: %s: deck %s %s: %s", id, dpath, sok ? "linked into m_ComboDBPerWeapon[0]" : "NOT set", back); ok += sok; bad += !sok;
        }
    }
    if (!g_np_react) { g_np_react = 1; logf_("npcs: events: on_hit -> %d, on_death -> %d, on_pawn_destroyed -> %d", CA->on_hit(CA, np_on_hit, NULL), CA->on_death(CA, np_on_death, NULL), api_p_on_pawn_destroyed(CA, np_destroyed, NULL)); g_ar_hooked = 1; if (!g_np_api) g_np_api = CA; }
    np_apply_spawn(n, idx);
    char hp[40] = "", sty[40] = ""; CA->get_property_text(CA, arch, "m_fHealth", hp, sizeof hp); CA->get_property_text(CA, arch, "m_uiCombatStyle", sty, sizeof sty);
    logf_("npcs: %s/%s ready: %s (health %s, style %s, faction %g, %d deck key(s), reactions hp<%.2f:%d death:%d area %.0f:%d, aggro %d, spawn %d; %d value(s) set, %d failed)", mod, id, n->path, hp, sty, n->faction, n->has_deck, n->hp_frac, n->nacts[0], n->nacts[1], n->area_r, n->nacts[2], n->aggro, n->has_spawn, ok, bad);
    return 0;
}

static void np_fill_api(AbsApi *a) {
    a->npc_spawn = api_p_npc_spawn; a->npc_remove = api_p_npc_remove; a->npc_set_target = api_p_npc_set_target; a->npc_set_faction = api_p_npc_set_faction; a->faction_can_target = api_p_faction_can_target;
    a->npc_archetype = api_p_npc_archetype; a->on_area = api_p_on_area; a->area_remove = api_p_area_remove; a->show_message = api_p_show_message;
}

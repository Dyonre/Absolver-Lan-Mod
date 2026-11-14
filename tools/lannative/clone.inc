// Player clones: bot opponents defined in a mod folder. Content kind "clones": Mods\<mod>\content\clones\<id>.json
//   { "id": "duelist", "name": "Duelist", "brain": "peak_stagger_v", "profile": "clones/duelist.txt", "style": 4, "menu": true }
// A clone is a SECOND LOCAL PLAYER (UGameplayStatics::CreatePlayer, ids 1..3): a real player character with its own character data, driven by a bot brain
// (botai.inc preset = a bots entry with a "brain"; without bot_ai = on it just stands there).
//   profile : a file inside the mod folder = a whole m_SavedData as UE text (deck, outfit, body build, name, style). Make one with the F1 entry
//             "Clones: save my character as a profile" (LanNative\data\clones\<name>.txt), then copy it into the mod. Without a profile the clone is a copy of
//             your current character. Absolver's encrypted .sav files do not load through LoadGameFromSlot, hence the text profile.
//   style   : optional fighting style number written over the profile's (not range-checked; the stock styles are 0..5).
// Safety (the clone must never touch player 0's save): GameInstance.m_CurrentSave is swapped for the clone's own copy around CreatePlayer and every
// AFightingPlayerController::InitCharacterAndSave of a clone controller; SaveGame of any controller but player 0's is skipped; the Steam entitlement pass and the
// lock-target widget (both need a real local player) are skipped for clones. Every hook checks its prologue bytes (hook_rva); any failure = no clone.
// Mods page (F2): "Clones: spawn <name>" per entry with "menu" (default true), "Clones: save my character as a profile", "Clones: remove all".
// LanNative\config.txt: clone_spawn = <id>[,<id>] spawns those clones 6 s after you are in the world (a sparring partner that is always there).
#define CL_MAX 3
typedef struct { char id[48], name[64], brain[64], profile[MAX_PATH * 3], mod[64]; int style, menu; } CloneDef;
static CloneDef g_cl[32]; static int g_cl_n;
static struct { void *pc, *save; int def; double t_spawn, t_pawn; int adopted; } g_cls[CL_MAX]; static int g_cls_n;
static void *g_cl_pending, **g_cl_csp; static int g_cl_active, g_cl_creating, g_cl_blocked;
static const char *const CL_KNOWN[] = { "id", "name", "brain", "profile", "style", "menu", NULL };

typedef uint8_t (*ClAddScreen_t)(void *, int); static ClAddScreen_t o_cl_addscreen;
typedef void (*ClEnt_t)(void *, void *); static ClEnt_t o_cl_ent;
typedef void (*ClSave_t)(void *); static ClSave_t o_cl_savegame;
typedef void (*ClInit_t)(void *); static ClInit_t o_cl_initsave;
// clones are LOCAL PLAYERS, they survive a map change but get NEW controllers there. A clone is therefore its player number (i + 1):
// UGameplayStatics::GetPlayerController(world, i + 1) finds its current controller, in the InitCharacterAndSave hook and every tick.
static void *cl_pc_of(int idx) {
    static void *cdo, *fn; if (!fn) { cdo = CA->find_object(CA, "/Script/Engine.Default__GameplayStatics"); fn = cdo ? CA->find_function(CA, cdo, "GetPlayerController") : NULL; }
    void *ctx = CA->get_world(CA), *pc = NULL; int i = idx; void *args[2] = { &ctx, &i }; if (fn && ctx) CA->call_function_params(CA, cdo, fn, args, &pc); return se_valid_ptr(pc) ? pc : NULL;
}
static int cl_slot_of_pc(void *pc) { for (int i = 0; i < g_cls_n; i++) if (g_cls[i].pc == pc) return i; for (int i = 0; i < g_cls_n; i++) if (cl_pc_of(i + 1) == pc) { g_cls[i].pc = pc; g_cls[i].adopted = 0; g_cls[i].t_pawn = 0; return i; } return -1; }
static void hk_cl_savegame(void *pc) {
    if (g_cl_active && pc != CA->get_local_player_controller(CA)) { if (g_cl_blocked++ < 20) logf_("clones: SaveGame of controller %p skipped (a clone never saves)", pc); return; }
    o_cl_savegame(pc);
}
static void hk_cl_initsave(void *pc) {
    if (g_cl_active && g_cl_csp && pc != CA->get_local_player_controller(CA)) {
        void *sv = NULL; int si = cl_slot_of_pc(pc); if (si >= 0) sv = g_cls[si].save;
        if (!sv && g_cl_pending && g_cls_n > 0 && !g_cls[g_cls_n - 1].pc) { g_cls[g_cls_n - 1].pc = pc; sv = g_cl_pending; }   // the clone being created
        if (sv) { void *s0 = *g_cl_csp; *g_cl_csp = sv; o_cl_initsave(pc); *g_cl_csp = s0; logf_("clones: controller %p set up from its own save %p", pc, sv); return; }
    }
    o_cl_initsave(pc);
}
static void hk_cl_ent(void *self, void *save) { if (g_cl_active && self != cd_local_pawn()) return; o_cl_ent(self, save); }
static uint8_t hk_cl_addscreen(void *self, int z) { if (g_cl_creating) return 0; return o_cl_addscreen(self, z); }

static int cl_hooks(void) {
    static int state; if (state) return state > 0; state = -1;
    static const uint8_t e1[15] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57, 0x48,0x83,0xEC,0x70 };
    static const uint8_t e2[14] = { 0x40,0x55, 0x56, 0x57, 0x41,0x55, 0x48,0x8D,0xAC,0x24,0x58,0xFF,0xFF,0xFF };
    static const uint8_t e3[17] = { 0x40,0x55, 0x57, 0x41,0x56, 0x48,0x8D,0x6C,0x24,0xB9, 0x48,0x81,0xEC,0xA0,0x00,0x00,0x00 };
    static const uint8_t e4[17] = { 0x40,0x53, 0x55, 0x56, 0x41,0x55, 0x48,0x83,0xEC,0x58, 0x48,0x8B,0xA9,0xA8,0x03,0x00,0x00 };
    o_cl_addscreen = (ClAddScreen_t)CA->hook_rva(CA, "UUserWidget::AddToPlayerScreen", 0x10515E0u, e1, 15, (void *)hk_cl_addscreen);
    o_cl_ent = (ClEnt_t)CA->hook_rva(CA, "AFightingCharacter::UpdateSaveFromEntitlements", 0x356010u, e2, 14, (void *)hk_cl_ent);
    o_cl_savegame = (ClSave_t)CA->hook_rva(CA, "AFightingPlayerController::SaveGame", 0x3A2430u, e3, 17, (void *)hk_cl_savegame);
    o_cl_initsave = (ClInit_t)CA->hook_rva(CA, "AFightingPlayerController::InitCharacterAndSave", 0x399890u, e4, 17, (void *)hk_cl_initsave);
    logf_("clones: hooks addscreen %p entitlements %p savegame %p initsave %p", (void *)o_cl_addscreen, (void *)o_cl_ent, (void *)o_cl_savegame, (void *)o_cl_initsave);
    if (o_cl_addscreen && o_cl_ent && o_cl_savegame && o_cl_initsave) state = 1;
    return state > 0;
}
static void **cl_current_save_slot(void) {
    void *world = CA->get_world(CA), **gip = world ? CA->prop_ptr(CA, world, "OwningGameInstance") : NULL, *gi = gip ? *gip : NULL;
    return gi ? (void **)CA->prop_ptr(CA, gi, "m_CurrentSave") : NULL;
}
static char *cl_read(const char *path8, int cap) { wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, path8, -1, w, MAX_PATH); return mods_read_text(w, (unsigned)cap); }

static int cl_spawn(int di) {
    const CloneDef *c = &g_cl[di]; void *world_ctx = cd_local_pawn();
    if (g_cls_n >= CL_MAX) { logf_("clones: %s: %d clones already - remove one first", c->id, CL_MAX); return 0; }
    if (!world_ctx) { logf_("clones: %s: no player pawn yet", c->id); return 0; }
    if (!cl_hooks()) { logf_("clones: %s: refused - the safety hooks are not installed (a clone would share or write your save)", c->id); return 0; }
    void **csp = cl_current_save_slot(), *save0 = csp ? *csp : NULL; if (!save0) { logf_("clones: %s: no current save", c->id); return 0; }
    void *copy = CA->duplicate_object(CA, save0, CA->find_object(CA, "/Engine/Transient")); if (!copy) { logf_("clones: %s: could not copy the save object", c->id); return 0; }
    CA->add_to_root(CA, copy, 1);
    if (c->profile[0]) {
        char *t = cl_read(c->profile, 16 << 20); int ok = t && CA->set_property_text(CA, copy, "m_SavedData", t); free(t);
        logf_("clones: %s: profile %s %s", c->id, c->profile, ok ? "loaded" : "NOT loaded (the clone is a copy of your character)");
    }
    if (c->style >= 0) { uint8_t *st = CA->prop_ptr_path(CA, copy, "m_SavedData.m_uiCombatStyle"); if (st) *st = (uint8_t)c->style; }
    g_cl_active = 1; g_cl_csp = csp; g_cl_pending = copy;
    memset(&g_cls[g_cls_n], 0, sizeof g_cls[0]); g_cls[g_cls_n].save = copy; g_cls[g_cls_n].def = di; g_cls[g_cls_n].t_spawn = GetTickCount64() / 1000.0; g_cls_n++;
    void *pc = NULL; *csp = copy; g_cl_creating = 1;
    __try { pc = ((void *(*)(void *, int, char))(g_base + 0x146D1D0u))(world_ctx, g_cls_n, 1); }   // UGameplayStatics::CreatePlayer(world context, controller id, spawn pawn)
    __except (EXCEPTION_EXECUTE_HANDLER) { pc = NULL; logf_("clones: %s: CreatePlayer EXCEPTION 0x%08lX", c->id, GetExceptionCode()); }
    g_cl_creating = 0; *csp = save0; g_cl_pending = NULL;
    if (!pc) {
        g_cls_n--; memset(&g_cls[g_cls_n], 0, sizeof g_cls[0]);
        CA->add_to_root(CA, copy, 0);
        if (!g_cls_n) { g_cl_active = 0; g_cl_csp = NULL; }
        logf_("clones: %s: CreatePlayer refused; copied save released (active %d)", c->id, g_cls_n);
        return 0;
    }
    if (!g_cls[g_cls_n - 1].pc) g_cls[g_cls_n - 1].pc = pc;
    logf_("clones: %s spawned: controller %p (player %d), save %p, brain %s", c->id, pc, g_cls_n, copy, c->brain[0] ? c->brain : "none");
    return 1;
}
static void cl_shake_install(void); static void cl_lookup_install(void);   // defined below (installed from the tick)
static char g_cl_auto[160]; static double g_cl_auto_t;   // config clone_spawn (header)
static void cl_autospawn(void) {
    if (!g_cl_auto[0] || !cd_local_pawn()) { g_cl_auto_t = 0; return; }
    { char wp[300] = ""; void *w = CA->get_world(CA); if (!w || !CA->object_path(CA, w, wp, sizeof wp) || strstr(wp, "/StartMap/")) { g_cl_auto_t = 0; return; } }   // not on the title screen
    double now = GetTickCount64() / 1000.0; if (g_cl_auto_t == 0) { g_cl_auto_t = now; return; } if (now - g_cl_auto_t < 6.0) return;
    char list[160]; snprintf(list, sizeof list, "%s", g_cl_auto); g_cl_auto[0] = 0;
    for (char *t = strtok(list, ", "); t; t = strtok(NULL, ", ")) { int hit = 0; for (int i = 0; i < g_cl_n && !hit; i++) if (!strcmp(g_cl[i].id, t)) { hit = 1; cl_spawn(i); } if (!hit) logf_("clones: clone_spawn: no clone '%s'", t); }
}
static void cl_tick(float dt, void *u) {   // give each new clone pawn its brain once it exists
    (void)dt; (void)u; cl_shake_install(); cl_lookup_install(); cl_autospawn();
    for (int i = 0; i < g_cls_n; i++) {
        { void *cur = cl_pc_of(i + 1); if (cur && cur != g_cls[i].pc) { logf_("clones: player %d has a new controller %p (map change)", i + 1, cur); g_cls[i].pc = cur; g_cls[i].adopted = 0; g_cls[i].t_pawn = 0; } }
        if (g_cls[i].adopted || !g_cls[i].pc) continue; const CloneDef *c = &g_cl[g_cls[i].def];
        void **pp = (void **)CA->prop_ptr(CA, g_cls[i].pc, "Pawn"); void *pawn = pp && se_valid_ptr(*pp) ? *pp : NULL; if (!pawn) continue;
        double now = GetTickCount64() / 1000.0;
        if (g_cls[i].t_pawn == 0) {   // a clone spawns next to the player, where stray hits land on it: put it 8 m in front, facing the player (closer, its fight drifts into the player)
            g_cls[i].t_pawn = now; void *pl = cd_local_pawn(); float pp[3], fw[3];
            if (pl && CA->test_get_location(CA, pl, pp) && CA->test_get_forward(CA, pl, fw)) {
                float side = (i % 2 ? -1.f : 1.f) * 150.f * (float)(1 + i / 2), loc[3] = { pp[0] + fw[0] * 800.f - fw[1] * side, pp[1] + fw[1] * 800.f + fw[0] * side, pp[2] + 20.f };
                float yaw = atan2f(pp[1] - loc[1], pp[0] - loc[0]) * 57.29578f; int ok = CA->test_teleport(CA, pawn, loc, &yaw);
                logf_("clones: player %d placed 8 m in front of you (%s)", i + 1, ok ? "ok" : "teleport refused, left where it spawned");
            }
            continue;
        }
        if (now - g_cls[i].t_pawn < 1.5) continue;   // adopted in the pawn's first frame, the bots-mode pairing (by place) never forms: let the game set the pawn up first
        g_cls[i].adopted = 1;
        if (!c->brain[0]) continue;
        if (!g_bai_on) { logf_("clones: %s: brain %s not started - bot_ai is off (LanNative\\config.txt: bot_ai = on)", c->id, c->brain); continue; }
        bai_adopt(pawn, c->mod, c->brain); logf_("clones: %s: brain %s %s", c->id, c->brain, bai_of(pawn) ? "adopted the pawn" : "did NOT adopt it (unknown preset?)");
    }
}
static void cl_remove_all(void *u) {
    (void)u; void *cdo = CA->find_object(CA, "/Script/Engine.Default__GameplayStatics"), *fn = cdo ? CA->find_function(CA, cdo, "RemovePlayer") : NULL;
    for (int i = g_cls_n - 1; i >= 0; i--) {
        if (fn && g_cls[i].pc) { uint8_t destroy = 1; void *args[2] = { &g_cls[i].pc, &destroy }; CA->call_function_params(CA, cdo, fn, args, NULL); }
        if (g_cls[i].save) CA->add_to_root(CA, g_cls[i].save, 0);
    }
    logf_("clones: removed %d clone(s)", g_cls_n); g_cls_n = 0; g_cl_active = 0; g_cl_csp = NULL; g_cl_pending = NULL;
}
static void cl_menu_spawn(void *u) { cl_spawn((int)(intptr_t)u); }
static void cl_menu_dump(void *u) {
    (void)u; void **csp = cl_current_save_slot(), *sv = csp ? *csp : NULL; if (!sv) { logf_("clones: no current save to write"); return; }
    int cap = 16 << 20; char *buf = malloc(cap); int n = buf ? CA->get_property_text(CA, sv, "m_SavedData", buf, cap) : -1;
    char nm[64] = "character"; { uint8_t *np = CA->prop_ptr_path(CA, sv, "m_SavedData.m_CharaName"); if (np && *(wchar_t **)np) WideCharToMultiByte(CP_UTF8, 0, *(wchar_t **)np, -1, nm, sizeof nm, NULL, NULL); }
    for (char *p = nm; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') *p = '_';
    wchar_t dir[MAX_PATH], path[MAX_PATH], wn[64]; swprintf(dir, MAX_PATH, L"%sdata", g_dir); CreateDirectoryW(dir, NULL); wcscat(dir, L"\\clones"); CreateDirectoryW(dir, NULL);
    MultiByteToWideChar(CP_UTF8, 0, nm, -1, wn, 64); swprintf(path, MAX_PATH, L"%ls\\%ls.txt", dir, wn);
    FILE *f = n > 0 && n < cap - 1 ? _wfopen(path, L"wb") : NULL; if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }
    free(buf); logf_("clones: profile of '%s' %s (%d bytes) -> %ls", nm, f ? "written" : "NOT written", n, path);
}
static int clones_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    const char *id = cs_str(&d, 0, "id"), *pr = cs_str(&d, 0, "profile"); int ok = 1;
    if (!id || !id[0] || strlen(id) >= sizeof g_cl[0].id) { snprintf(err, cap, "needs a short \"id\""); ok = 0; }
    else if (pr && (strstr(pr, "..") || strchr(pr, ':') || pr[0] == '/' || pr[0] == '\\')) { snprintf(err, cap, "profile '%s' must be a path inside the mod folder", pr); ok = 0; }
    if (ok) cs_warn_unknown(&d, "clones", id, CL_KNOWN);
    jdoc_free(&d); return ok;
}
static int clones_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; int at = -1; for (int i = 0; i < g_cl_n; i++) if (!strcmp(g_cl[i].id, id)) at = i;   // content is applied again on reload / world load: update in place
    if (at < 0 && g_cl_n >= 32) { logf_("clones: table full - %s ignored", id); return 0; }
    JDoc d; if (jdoc_parse(&d, json) != 0) { jdoc_free(&d); return 0; }
    int fresh = at < 0; if (fresh) at = g_cl_n; CloneDef *c = &g_cl[at]; memset(c, 0, sizeof *c);
    snprintf(c->id, sizeof c->id, "%s", id); snprintf(c->mod, sizeof c->mod, "%s", mod);
    const char *s = cs_str(&d, 0, "name"); snprintf(c->name, sizeof c->name, "%s", s ? s : id);
    s = cs_str(&d, 0, "brain"); snprintf(c->brain, sizeof c->brain, "%s", s ? s : "");
    s = cs_str(&d, 0, "profile"); if (s && s[0]) snprintf(c->profile, sizeof c->profile, "%s\\%s", g_ce_dir, s);
    int js = jfind(&d, 0, "style"); c->style = js >= 0 && d.n[js].t == J_NUM ? atoi(d.n[js].str) : -1;
    int jm = jfind(&d, 0, "menu"); c->menu = jm < 0 || cs_true(&d, 0, "menu");
    jdoc_free(&d);
    if (c->menu && fresh) { char lb[96]; snprintf(lb, sizeof lb, "Clones: spawn %s", c->name); CA->add_menu_item(CA, lb, cl_menu_spawn, (void *)(intptr_t)at); }
    logf_("clones: %s (%s): brain %s, profile %s, style %d", c->id, c->name, c->brain[0] ? c->brain : "none", c->profile[0] ? c->profile : "none (a copy of your character)", c->style);
    if (fresh) g_cl_n++; return 1;
}
// Camera shakes of a clone's fight must not reach the player's screen. BP_FightingPlayer (every player character, clones too)
// calls PlayerCameraManager::PlayCameraShake on a camera manager for hit / guard-break / avoid shakes - player 0's when the caller is a clone. The native is swapped
// (replace_native): the original runs (the Blueprint VM must consume its arguments), then a shake started by a clone's character is stopped at once.
typedef void (*ClThunk_t)(void *ctx, void *frame, void *result); static ClThunk_t o_cl_shake; static int g_cl_shakes_cut;
static int cl_is_clone_pawn(void *o) {
    for (int i = 0; i < g_cls_n; i++) { if (!g_cls[i].pc) continue; void **pp = (void **)CA->prop_ptr(CA, g_cls[i].pc, "Pawn"); if (pp && *pp == o) return 1; }
    return 0;
}
static void cl_shake_thunk(void *ctx, void *frame, void *result) {
    void *caller = frame ? *(void **)((uint8_t *)frame + 0x18) : NULL;   // FFrame::Object: the Blueprint object making the call (secore.inc / weaponslot.inc read it the same way)
    o_cl_shake(ctx, frame, result);
    if (!g_cls_n || !caller || !result || !cl_is_clone_pawn(caller)) return;
    void *shake = *(void **)result; if (!shake) return;
    void *fn = CA->find_function(CA, ctx, "StopCameraShake"); uint8_t now = 1; void *args[2] = { &shake, &now };
    if (fn) { CA->call_function_params(CA, ctx, fn, args, NULL); if (g_cl_shakes_cut++ < 5) logf_("clones: a camera shake from clone %p stopped (it shook your camera)", caller); }
}
static void cl_init(const AbsApi *a) {
    (void)a; { wchar_t src[MAX_PATH]; if (setting_get("clone_spawn", L"", 0, g_cl_auto, sizeof g_cl_auto, src) && g_cl_auto[0]) logf_("clones: clone_spawn = %s", g_cl_auto); else g_cl_auto[0] = 0; }
    CA->add_menu_item(CA, "Clones: save my character as a profile", cl_menu_dump, NULL); CA->add_menu_item(CA, "Clones: remove all", cl_remove_all, NULL);
    CA->on_tick(CA, cl_tick, NULL);
}
// Player-0 lookups: without a redirect, what happens to a clone (damage and more) is also applied to the real player. The player
// character Blueprints (BP_TPSCharacter: GetPlayerController x4, GetPlayerPawn x8; BP_FightingPlayer: GetPlayerCameraManager x9) look up PLAYER 0 - inside a
// clone's character that is the real player. The four UGameplayStatics lookups are swapped (replace_native): the original runs, then, when the calling object
// is a clone's pawn (or a component of it) and the answer is player 0's controller / pawn / camera manager, the clone's own one is returned instead.
static int cl_clone_slot_of(void *o) {
    if (!o || !g_cls_n) return -1;
    if (CA->is_a(CA, o, "ActorComponent")) { void *fn = CA->find_function(CA, o, "GetOwner"), *ow = NULL; if (fn) CA->call_function_params(CA, o, fn, NULL, &ow); o = ow; if (!o) return -1; }
    for (int i = 0; i < g_cls_n; i++) { if (!g_cls[i].pc) continue; if (o == g_cls[i].pc) return i; void **pp = (void **)CA->prop_ptr(CA, g_cls[i].pc, "Pawn"); if (pp && *pp == o) return i; }
    return -1;
}
static ClThunk_t o_cl_gpc, o_cl_gpp, o_cl_gpch, o_cl_gpcm; static long g_cl_redirects[4];
static void cl_redirect(int kind, void *frame, void *result) {   // kind 0 controller, 1 pawn, 2 character, 3 camera manager
    void *caller = frame ? *(void **)((uint8_t *)frame + 0x18) : NULL; if (!caller || !result || !*(void **)result) return;
    int i = cl_clone_slot_of(caller); if (i < 0) return;
    void *pc0 = CA->get_local_player_controller(CA), *mine = NULL, *theirs = NULL, *cpc = g_cls[i].pc;
    if (kind == 0) { mine = pc0; theirs = cpc; }
    else if (kind == 1 || kind == 2) { mine = cd_local_pawn(); void **pp = (void **)CA->prop_ptr(CA, cpc, "Pawn"); theirs = pp ? *pp : NULL; }
    else { void **m0 = pc0 ? (void **)CA->prop_ptr(CA, pc0, "PlayerCameraManager") : NULL, **m1 = (void **)CA->prop_ptr(CA, cpc, "PlayerCameraManager"); mine = m0 ? *m0 : NULL; theirs = m1 ? *m1 : NULL; }
    if (mine && theirs && *(void **)result == mine) { *(void **)result = theirs; if (g_cl_redirects[kind]++ < 3) logf_("clones: a player-0 lookup (%s) from clone %d's character now answers its own", (const char *[]){ "controller", "pawn", "character", "camera manager" }[kind], i + 1); }
}
static void cl_gpc(void *c, void *f, void *r) { o_cl_gpc(c, f, r); cl_redirect(0, f, r); }
static void cl_gpp(void *c, void *f, void *r) { o_cl_gpp(c, f, r); cl_redirect(1, f, r); }
static void cl_gpch(void *c, void *f, void *r) { o_cl_gpch(c, f, r); cl_redirect(2, f, r); }
static void cl_gpcm(void *c, void *f, void *r) { o_cl_gpcm(c, f, r); cl_redirect(3, f, r); }
static void cl_lookup_install(void) {
    static int done[4], tries; static double t; double now = GetTickCount64() / 1000.0; if ((done[0] && done[1] && done[2] && done[3]) || now - t < 1.0) return; t = now;
    static const char *const names[4] = { "GetPlayerController", "GetPlayerPawn", "GetPlayerCharacter", "GetPlayerCameraManager" };
    void *thunks[4] = { (void *)cl_gpc, (void *)cl_gpp, (void *)cl_gpch, (void *)cl_gpcm }; ClThunk_t *origs[4] = { &o_cl_gpc, &o_cl_gpp, &o_cl_gpch, &o_cl_gpcm };
    for (int k = 0; k < 4; k++) if (!done[k] && CA->replace_native(CA, "GameplayStatics", names[k], thunks[k], (void **)origs[k]) == 0 && *origs[k]) { done[k] = 1; logf_("clones: %s answers clones with their own", names[k]); }
    if (++tries == 30 && !(done[0] && done[1] && done[2] && done[3])) logf_("clones: WARNING not every player-0 lookup redirect is installed (%d %d %d %d)", done[0], done[1], done[2], done[3]);
}
static void cl_shake_install(void) {   // at load replace_native is not ready yet (returns 1): retry from the tick; 0 = installed
    static int done, tries; static double t; double now = GetTickCount64() / 1000.0; if (done || now - t < 1.0) return; t = now;
    int r = CA->replace_native(CA, "PlayerCameraManager", "PlayCameraShake", (void *)cl_shake_thunk, (void **)&o_cl_shake);
    if (r == 0 && o_cl_shake) { done = 1; logf_("clones: camera shake filter on"); }
    else if (++tries == 30) logf_("clones: camera shake filter NOT installed yet (replace_native %d) - clone hits may shake your camera", r);
}

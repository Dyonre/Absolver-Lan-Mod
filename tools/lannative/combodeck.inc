// combodeck.inc - decks, fighting styles, weapon / style events and nested property paths (API "decks and weapon slot"). Included by secore.inc.
// Facts used here:
//   live deck list  = GameInstance.m_CurrentSave (SaveThePlaines) .m_SavedData.m_SavedCombos[family].m_Combos   (TArray<FComboSave>, stride 0x138, FName at +0)
//   current deck    = ... .m_SavedData.m_CurrentComboSaveName[family]   (AFightingPlayerController::BPF_LoadComboFromName writes it, then UAttackComponent::SetComboDb)
//   combat style    = FightingCharacter.m_PlayerComponent (PlayerFightingComponent) .m_uiCombatStyle; changed through FightingPlayerController::BPF_SetPlayerCombatStyle
//   equipped weapon = FightingCharacter.m_InventoryComponent .m_EquippedWeapon
// on_weapon_changed / on_style_changed are polled per tick for the LOCAL player (no hook: every change, also from menus, the save load and the network
// code, is seen within one frame; every registration has its own poll, so one plugin's fault never stops another's events).
#define CD_COMBOSAVE_SIZE 0x138

// ----- prop_ptr_path: "a.b[2].c" through object pointers, structs, static arrays and TArrays
static void *cd_path(void *obj, const char *path, char *why, int cap) {
    uint8_t *base = obj; uint8_t *container = obj ? *(uint8_t **)((uint8_t *)obj + 0x10) : NULL; uint8_t *addr = NULL; char tok[100]; const char *p = path;
    if (!obj || !path || !*path) { snprintf(why, cap, "no object / path"); return NULL; }
    while (*p) {
        int n = 0; while (*p && *p != '.' && *p != '[' && n < 99) tok[n++] = *p++; tok[n] = 0;
        int idx = -1; if (*p == '[') { idx = atoi(++p); while (*p && *p != ']') p++; if (*p == ']') p++; }
        if (*p == '.') p++;
        uint8_t *pr = container ? api_p_find_property(NULL, container, tok) : NULL; if (!pr) { snprintf(why, cap, "'%s' not found", tok); return NULL; }
        const char *k = prop_kind(pr); int dim = *(int *)(pr + 0x30), esz = *(int *)(pr + 0x34);
        addr = base + *(int *)(pr + 0x44);
        if (!strcmp(k, "Array") && idx >= 0) {                                       // TArray element
            uint8_t *inner = *(uint8_t **)(pr + 0x70); int num = *(int *)(addr + 8);
            if (idx >= num) { snprintf(why, cap, "'%s[%d]' out of range (%d)", tok, idx, num); return NULL; }
            addr = *(uint8_t **)addr + (size_t)idx * (size_t)*(int *)(inner + 0x34); pr = inner; k = prop_kind(pr);
        } else if (idx >= 0) {                                                       // static array
            if (idx >= dim) { snprintf(why, cap, "'%s[%d]' out of range (%d)", tok, idx, dim); return NULL; }
            addr += (size_t)idx * (size_t)esz;
        }
        if (!*p) break;
        if (!strcmp(k, "Struct")) { base = addr; container = *(uint8_t **)(pr + 0x70); }
        else if (!strcmp(k, "Object")) { base = *(uint8_t **)addr; if (!se_valid_ptr(base)) { snprintf(why, cap, "'%s' is null", tok); return NULL; } container = *(uint8_t **)(base + 0x10); }
        else { snprintf(why, cap, "'%s' (%s) has no members", tok, k); return NULL; }
    }
    return addr;
}
static void *api_p_prop_ptr_path(const AbsApi *self, void *obj, const char *path) {
    char why[200] = ""; void *r = NULL; __try { r = cd_path(obj, path, why, sizeof why); } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(why, sizeof why, "EXCEPTION 0x%08lX", GetExceptionCode()); r = NULL; }
    if (!r) { static int logged; if (logged++ < 30) logf_("plugin %s: prop_ptr_path '%s': %s", plug_name_of(plug_of(self)), path ? path : "?", why); }
    return r;
}
static int api_p_fname_make(const AbsApi *self, const char *utf8, uint64_t *out) { (void)self; if (!utf8 || !out) return 1; wchar_t w[256]; u8_to_w(utf8, w, 256); FNameRaw f = fname_of(w); memcpy(out, &f, 8); return 0; }
static int api_p_fname_text(const AbsApi *self, const void *fname, char *out, int cap) { (void)self; if (!fname || !out || cap < 1) return 1; out[0] = 0; __try { fname_text(*(const FNameRaw *)fname, out, cap); } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 2; } return 0; }

// ----- the live save object and the local character
static void *cd_live_save(void) {
    void *w = world(); if (!w) return NULL; char why[120]; void **s = NULL;
    __try { s = cd_path(w, "OwningGameInstance.m_CurrentSave", why, sizeof why); } __except (EXCEPTION_EXECUTE_HANDLER) { s = NULL; }
    if (!s && api_p_is_game_thread(NULL)) {                                          // fallback: UGameplayStatics::GetGameInstance(world)
        void *cdo = api_p_find_cdo(NULL, "GameplayStatics"), *c = se_find_class("GameplayStatics"), *f = c ? se_class_find_function(c, "GetGameInstance") : NULL, *gi = NULL, *args[1] = { &w };
        if (cdo && f && api_p_call_params(NULL, cdo, f, args, &gi) && gi) { __try { s = cd_path(gi, "m_CurrentSave", why, sizeof why); } __except (EXCEPTION_EXECUTE_HANDLER) { s = NULL; } }
    }
    return s && se_valid_ptr(*s) ? *s : NULL;
}
static SeTArr *cd_deck_list(int fam) {                                              // TArray<FComboSave> of one family in the live save
    void *s = cd_live_save(); char p[80], why[120]; if (!s || fam < 0 || fam > 2) return NULL; snprintf(p, sizeof p, "m_SavedData.m_SavedCombos[%d]", fam);
    SeTArr *a = NULL; __try { a = cd_path(s, p, why, sizeof why); } __except (EXCEPTION_EXECUTE_HANDLER) { a = NULL; } return a;
}
static uint64_t *cd_current_slot(int fam) {                                          // &m_CurrentComboSaveName[fam] in the live save
    void *s = cd_live_save(); char p[80], why[120]; if (!s || fam < 0 || fam > 2) return NULL; snprintf(p, sizeof p, "m_SavedData.m_CurrentComboSaveName[%d]", fam);
    uint64_t *r = NULL; __try { r = cd_path(s, p, why, sizeof why); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r;
}
static int cd_deck_index(int fam, uint64_t name) { SeTArr *a = cd_deck_list(fam); if (!a) return -1; for (int i = 0; i < a->num; i++) if (*(uint64_t *)((uint8_t *)a->data + (size_t)i * CD_COMBOSAVE_SIZE) == name) return i; return -1; }
static void *cd_local_pawn(void) { void *pc = L_local_pc(); void **p = pc ? api_p_prop_ptr(NULL, pc, "Pawn") : NULL; return p && se_valid_ptr(*p) ? *p : NULL; }
static void *cd_comp(void *pawn, const char *prop) { void **c = pawn ? api_p_prop_ptr(NULL, pawn, prop) : NULL; return c && se_valid_ptr(*c) ? *c : NULL; }
static void *cd_equipped_weapon(void *pawn) { void *ic = cd_comp(pawn, "m_InventoryComponent"); void **w = ic ? api_p_prop_ptr(NULL, ic, "m_EquippedWeapon") : NULL; return w ? *w : NULL; }
static int cd_style_of(void *pawn) { void *pf = cd_comp(pawn, "m_PlayerComponent"); uint8_t *s = pf ? api_p_prop_ptr(NULL, pf, "m_uiCombatStyle") : NULL; return s ? *s : -1; }

// ----- combo_slot_ensure / combo_load / combo_current
static int api_p_combo_slot_ensure(const AbsApi *self, int fam, const char *name, int copy_from) {
    const char *who = plug_name_of(plug_of(self)); if (!name || !*name || fam < 0 || fam > 2) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: combo_slot_ensure refused - not on the game thread", who); return -1; }
    uint64_t fn; api_p_fname_make(self, name, &fn); int idx = cd_deck_index(fam, fn); if (idx >= 0) return idx;
    SeTArr *a = cd_deck_list(fam); void *rs = api_p_find_object(self, "/Script/Absolver.ComboSave");
    if (!a || !rs || a->num <= 0 || copy_from < 0 || copy_from >= a->num) { logf_("plugin %s: combo_slot_ensure(%d, %s): deck list %p (%d), ComboSave struct %p, copy_from %d - not created", who, fam, name, (void *)a, a ? a->num : -1, rs, copy_from); return -1; }
    int ok = 0;
    __try {                                                                           // grow the TArray by one element (elements are bitwise relocatable), deep-copy the source deck into it
        int n = a->num; uint8_t *nb = ((void *(*)(size_t, unsigned))(g_base + RVA_FMEMORY_MALLOC))((size_t)(n + 1) * CD_COMBOSAVE_SIZE, 16);
        if (nb) {
            memcpy(nb, a->data, (size_t)n * CD_COMBOSAVE_SIZE); uint8_t *e = nb + (size_t)n * CD_COMBOSAVE_SIZE; memset(e, 0, CD_COMBOSAVE_SIZE);
            ((void (*)(void *, void *, int))(g_base + RVA_STRUCT_INIT))(rs, e, 1);
            ((void (*)(void *, void *, const void *, int))(g_base + RVA_STRUCT_COPY))(rs, e, nb + (size_t)copy_from * CD_COMBOSAVE_SIZE, 1);
            *(uint64_t *)e = fn; void *old = a->data; a->data = nb; a->num = n + 1; a->max = n + 1;
            if (old) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(old);
            idx = n; ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: combo_slot_ensure: EXCEPTION 0x%08lX", who, GetExceptionCode()); ok = 0; idx = -1; }
    logf_("plugin %s: combo_slot_ensure(family %d, '%s', copy of %d) -> %s index %d", who, fam, name, copy_from, ok ? "CREATED" : "FAILED", idx);
    return idx;
}
static int api_p_combo_load(const AbsApi *self, int fam, const char *name) {
    const char *who = plug_name_of(plug_of(self)); void *pc = L_local_pc(); void *f = pc ? api_p_find_function(self, pc, "BPF_LoadComboFromName") : NULL;
    if (!name || fam < 0 || fam > 2 || !f) { logf_("plugin %s: combo_load(%d, %s): no controller / function", who, fam, name ? name : "?"); return 0; }
    uint64_t fn; api_p_fname_make(self, name, &fn); if (cd_deck_index(fam, fn) < 0) { logf_("plugin %s: combo_load(%d, %s): no deck of that name", who, fam, name); return 0; }
    unsigned char f8 = (unsigned char)fam, mode = 0; void *args[3] = { &fn, &f8, &mode }; int ok = api_p_call_params(self, pc, f, args, NULL);
    uint64_t *cur = cd_current_slot(fam); ok = ok && cur && *cur == fn;
    logf_("plugin %s: combo_load(family %d, '%s') -> %d", who, fam, name, ok); return ok;
}
static int api_p_combo_current(const AbsApi *self, int fam, char *name, int cap) {
    (void)self; if (name && cap > 0) name[0] = 0; uint64_t *cur = cd_current_slot(fam); if (!cur) return -1;
    if (name && cap > 0) fname_text(*(FNameRaw *)cur, name, cap); return cd_deck_index(fam, *cur);
}

// ----- set_combat_style: the local player's FightingPlayerController::BPF_SetPlayerCombatStyle(style, Current) (what the Meditation menu uses)
static int api_p_set_combat_style(const AbsApi *self, void *character, int style) {
    const char *who = plug_name_of(plug_of(self)); void *pawn = cd_local_pawn();
    if (character && character != pawn) { logf_("plugin %s: set_combat_style: only the local player is supported", who); return 0; }
    void *pc = L_local_pc(), *f = pc ? api_p_find_function(self, pc, "BPF_SetPlayerCombatStyle") : NULL; if (!f || style < 0 || style > 63) return 0;
    unsigned char s = (unsigned char)style, mode = 0; void *args[2] = { &s, &mode }; int ok = api_p_call_params(self, pc, f, args, NULL);
    int now = cd_style_of(pawn); logf_("plugin %s: set_combat_style(%d) -> call %d, style now %d", who, style, ok, now); return ok && now == style;
}

// ----- polled events
typedef struct { int plugin, kind, inited; void *cb, *user, *pawn, *last_item; int last_style; } CdWatch;
static CdWatch g_cdw[32]; static int g_cdw_n;
static void cd_watch_tick(float dt, void *u) {
    (void)dt; CdWatch *w = u; void *pawn = cd_local_pawn(); if (!pawn) { w->inited = 0; w->pawn = NULL; return; }
    if (w->pawn != pawn) { w->pawn = pawn; w->inited = 0; }
    if (w->kind == 0) {
        void *it = cd_equipped_weapon(pawn); if (w->inited && it == w->last_item) return;
        void *old = w->inited ? w->last_item : NULL; w->last_item = it; w->inited = 1;
        { static int n; if (n++ < 60) { char a[160] = "-", b[160] = "-"; if (old) se_path_of(old, a, sizeof a); if (it) se_path_of(it, b, sizeof b); logf_("decks: weapon changed %s -> %s (plugin %s)", a, b, plug_name_of(w->plugin)); } }
        ((void (*)(void *, void *, void *, int, void *))w->cb)(pawn, old, it, 0, w->user);
    } else {
        int s = cd_style_of(pawn); if (s < 0 || (w->inited && s == w->last_style)) return;
        int old = w->inited ? w->last_style : -1; w->last_style = s; w->inited = 1;
        { static int n; if (n++ < 60) logf_("decks: style changed %d -> %d (plugin %s)", old, s, plug_name_of(w->plugin)); }
        ((void (*)(void *, int, int, void *))w->cb)(pawn, old, s, w->user);
    }
}
static int cd_watch_add(const AbsApi *self, int kind, void *cb, void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_cdw_n >= 32) return 1;
    CdWatch *w = &g_cdw[g_cdw_n++]; memset(w, 0, sizeof *w); w->plugin = p; w->kind = kind; w->cb = cb; w->user = user; w->last_style = -1;
    logf_("plugin %s: %s registered (polled per tick, local player)", plug_name_of(p), kind ? "on_style_changed" : "on_weapon_changed");
    return api_p_on_tick(self, cd_watch_tick, w);
}
static int api_p_on_weapon_changed(const AbsApi *self, void (*cb)(void *, void *, void *, int, void *), void *user) { return cd_watch_add(self, 0, (void *)cb, user); }
static int api_p_on_style_changed(const AbsApi *self, void (*cb)(void *, int, int, void *), void *user) { return cd_watch_add(self, 1, (void *)cb, user); }

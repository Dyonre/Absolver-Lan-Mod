// The F1 menu's "Mods" page, plugin menu items (AbsApi::add_menu_item) and hot reload triggers (included by lannative.c after content.inc).
//   F2          opens the menu on the Mods page: safe-mode state, load order, every mod (loaded / off / skipped with the reason), conflicts, content registry state, plugin faults, last reload
//   hot reload: content_reload + rows_reload, run from the first line of the Mods page, from LanNative\reload.txt (create the file: it is read once a second, deleted, and the result
//               lands in LanNative\reload_result.txt) and from AbsApi::reload_content
// Text layout: modsui.h.
typedef struct { int plugin; char label[64], text[64]; void (*cb)(void *); void *user; } MenuItemReg;   // label = "Plugin: text" (log lines); text = what the plugin passed
#define MITEM_MAX 64
static MenuItemReg g_mitem[MITEM_MAX]; static int g_mitem_n;
static char g_reload_status[240];

static int api_p_add_menu_item(const AbsApi *self, const char *label, void (*cb)(void *), void *user) {
    int p = plug_of(self); if (p < 0 || !label || !*label || !cb) return 1;
    EnterCriticalSection(&g_lock); int rc = 1;
    if (g_mitem_n < MITEM_MAX) { MenuItemReg *m = &g_mitem[g_mitem_n++]; m->plugin = p; snprintf(m->label, sizeof m->label, "%s: %s", plug_name_of(p), label); snprintf(m->text, sizeof m->text, "%s", label); m->cb = cb; m->user = user; rc = 0; }
    LeaveCriticalSection(&g_lock);
    logf_("plugin %s: add_menu_item '%s' -> %d", plug_name_of(p), label, rc); return rc;
}

// ----- hot reload
static void reload_write_result(const char *text) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sreload_result.txt", g_dir); FILE *f = _wfopen(p, L"wb");
    if (f) { SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d %s\r\n", t.wHour, t.wMinute, t.wSecond, text); fclose(f); }
}
static int mods_reload_all(void) {
    static int busy; if (busy) return -1; busy = 1;
    char cst[200] = "", rst[200] = ""; int c = -1, r = -1;
    logf_("reload: hot reload of Mods\\*\\content and rows.json (game thread %lu)", GetCurrentThreadId());
    if (g_safe_mode) { snprintf(g_reload_status, sizeof g_reload_status, "not available in safe mode"); logf_("reload: %s", g_reload_status); busy = 0; return -1; }
    __try { c = content_reload(cst, sizeof cst); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("reload: EXCEPTION 0x%08lX in the content reload", GetExceptionCode()); snprintf(cst, sizeof cst, "content: exception"); c = -1; }
    __try { r = rows_reload(rst, sizeof rst); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("reload: EXCEPTION 0x%08lX in the rows.json reload", GetExceptionCode()); snprintf(rst, sizeof rst, "rows.json: exception"); r = -1; }
    snprintf(g_reload_status, sizeof g_reload_status, "%s | %s", cst, rst);
    reload_write_result(g_reload_status);
    { char lines[2][LM_LINE_MAX]; snprintf(lines[0], LM_LINE_MAX, "RELOAD: %.90s", cst); snprintf(lines[1], LM_LINE_MAX, "%.96s", rst); toast_show(lines, 2, 8); }
    busy = 0; return (c < 0 && r < 0) ? -1 : (c > 0 ? c : 0) + (r > 0 ? r : 0);
}
static int api_p_reload_content(const AbsApi *self) {
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: reload_content refused - not on the game thread", plug_name_of(plug_of(self))); return -1; }
    return mods_reload_all();
}

// ----- Mods page: load order edits, live switch-on, settings forms (commands come from the overlay: OVC_MODS_*)
static int md_console(const char *cmd);                                           // modes.inc
static int g_mi_live[MODS_MAX];                                                    // switched on while the game runs (rows / content only)
#define MUI_FORM_LOADER (-2)                                                          // g_form_row of the loader's own settings (F1 > Settings)
static int g_form_row = -1, g_form_mi = -1, g_form_ok, g_form_live, g_form_n; static unsigned g_form_serial; static char g_form_title[96], g_form_msg[200]; static OvSetting g_form[OV_SETTINGS];
static int mods_ui_mi_of(const char *name) { for (int i = 0; i < g_mi_n; i++) if (!_stricmp(g_mi[i].name, name)) return i; return -1; }
// 0 nothing pending, 1 switches on after a restart, 2 switches off after a restart, 3 switched on live
static int mods_ui_effect(int mi, int row) {
    if (g_mi_live[mi]) return g_order_now.mods[row].enabled ? 3 : 2;
    int b = modlist_find(&g_order_boot, g_mi[mi].name), boot_on = b >= 0 && g_order_boot.mods[b].enabled, now_on = g_order_now.mods[row].enabled;
    return boot_on == now_on ? 0 : now_on ? 1 : 2;
}
static int mods_ui_write_order(char *msg, size_t cap) {                          // loadorder.txt, after a copy of the old file as loadorder.txt.bak; nothing else in Mods is written
    static char text[MODS_MAX * (MOD_NAME_MAX + 4) + 512]; int n = mo_render(&g_order_now, text, sizeof text);
    if (n <= 0 || n >= (int)sizeof text) { snprintf(msg, cap, "the load order text is too large"); return -1; }
    wchar_t path[MAX_PATH], bak[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\loadorder.txt", g_mods_dir); swprintf(bak, MAX_PATH, L"%ls\\loadorder.txt.bak", g_mods_dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && !CopyFileW(path, bak, FALSE)) { snprintf(msg, cap, "could not back up loadorder.txt (error %lu); nothing was changed", GetLastError()); return -1; }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { snprintf(msg, cap, "could not write loadorder.txt (error %lu)", GetLastError()); return -1; }
    DWORD w = 0; int ok = WriteFile(h, text, (DWORD)n, &w, NULL) && w == (DWORD)n; CloseHandle(h);
    if (!ok) { snprintf(msg, cap, "loadorder.txt was written incompletely; the previous file is loadorder.txt.bak"); return -1; }
    return 0;
}
// A mod that was off at boot and holds only rows.json / content\ files is loaded into the running game through the hot reload; returns 1 if it was.
static int mods_ui_hot_enable(int mi) {
    ModInfo *m = &g_mi[mi]; ModKind k = { m->has_pak, m->has_plugin, m->has_lua, m->requires };
    if (g_safe_mode || m->status != MI_OFF || g_mi_live[mi] || !mo_hot_enable_ok(&k) || !(m->has_rows || m->has_content)) return 0;
    int pos = 0; for (int i = 0; i < g_mi_n; i++) if (g_mi_pos[i] >= pos) pos = g_mi_pos[i] + 1;
    if (pos >= MODS_MAX) return 0;
    g_mi_pos[mi] = pos; strcpy(g_posname[pos], m->name); g_mi_live[mi] = 1;
    int b = modlist_find(&g_order_boot, m->name);                                  // the live position is the end of the list: mirror that in the boot list so only a different file order stays "pending"
    if (b >= 0) { ModEntry e = g_order_boot.mods[b]; e.enabled = 1; memmove(&g_order_boot.mods[b], &g_order_boot.mods[b + 1], (size_t)(g_order_boot.count - b - 1) * sizeof(ModEntry)); g_order_boot.mods[g_order_boot.count - 1] = e; }
    logf_("mods page: %s switched on live (rows/content only) at load position %d", m->name, pos);
    mods_reload_all(); return 1;
}
static void mods_ui_form_close(void) { g_form_row = -1; g_form_mi = -1; g_form_n = 0; g_form_msg[0] = 0; g_form_serial++; }
static void mods_ui_form_open(int row) {
    if (row < 0 || row >= g_order_now.count) { mods_ui_form_close(); return; }
    int mi = mods_ui_mi_of(g_order_now.mods[row].name); if (mi < 0 || !g_mset[mi]) { mods_ui_form_close(); return; }
    const ModSettings *set = g_mset[mi]; wchar_t cp[MAX_PATH]; swprintf(cp, MAX_PATH, L"%ls\\plugin.cfg", g_mi_dir[mi]); char *cfg = mods_read_text(cp, 65536);
    g_form_n = 0; memset(g_form, 0, sizeof g_form);
    for (int i = 0; i < set->n && g_form_n < OV_SETTINGS; i++) {
        const ModSetting *s = &set->s[i]; OvSetting *d = &g_form[g_form_n++];
        snprintf(d->key, sizeof d->key, "%s", s->key); snprintf(d->label, sizeof d->label, "%s", s->label); snprintf(d->help, sizeof d->help, "%s", s->help); snprintf(d->def, sizeof d->def, "%s", s->def);
        d->type = s->type; d->has_min = s->has_min; d->has_max = s->has_max; d->min = s->min; d->max = s->max; d->nchoice = s->nchoice; memcpy(d->choice, s->choice, sizeof d->choice);
        char v[MSET_TEXT]; if (!cfg || !mset_cfg_get(cfg, s->key, v, sizeof v)) snprintf(v, sizeof v, "%s", s->def); snprintf(d->value, sizeof d->value, "%s", v);
    }
    free(cfg);
    g_form_row = row; g_form_mi = mi; g_form_msg[0] = 0; g_form_ok = 0; g_form_serial++;
    snprintf(g_form_title, sizeof g_form_title, "Settings: %s###lnform", g_mi[mi].name);
    g_form_live = 0; for (int p = 0; p < g_plugins_n; p++) if (!_stricmp(g_plugins[p].name, g_mi[mi].name) && g_plugins[p].loaded && g_plugins[p].module && GetProcAddress(g_plugins[p].module, "gleam_settings_changed")) g_form_live = 1;
    logf_("mods page: settings form of %s opened (%d setting(s), changes %s)", g_mi[mi].name, g_form_n, g_form_live ? "apply live" : "apply after a restart");
}
// the form's "key<TAB>value<LF>" lines -> the values that differ from what cfg (or the default) holds; kp / vp point into static buffers
static int mui_form_changes(const ModSettings *set, const char *cfg, const char *text, const char **kp, const char **vp) {
    static char keys[MSET_MAX][48], vals[MSET_MAX][MSET_TEXT]; int n = 0;
    for (const char *p = text; *p && n < MSET_MAX;) {                                // "key<TAB>value<LF>" lines; only values that differ from what the file (or the default) holds are written
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p); const char *tab = memchr(p, '\t', len);
        if (tab && tab - p < 48 && len - (size_t)(tab - p) - 1 < MSET_TEXT) {
            char k[48], v[MSET_TEXT]; snprintf(k, sizeof k, "%.*s", (int)(tab - p), p); snprintf(v, sizeof v, "%.*s", (int)(len - (size_t)(tab - p) - 1), tab + 1);
            const ModSetting *st = NULL; for (int i = 0; i < set->n; i++) if (!strcmp(set->s[i].key, k)) st = &set->s[i];
            if (st) {                                                                // unchanged values are not written (an unknown key or an invalid value still goes to mset_apply, which refuses it)
                char cur[MSET_TEXT], cn[MSET_TEXT * 2], cc[MSET_TEXT * 2], w1[200], w2[200];
                if (!cfg || !mset_cfg_get(cfg, k, cur, sizeof cur)) snprintf(cur, sizeof cur, "%s", st->def);
                if (mset_validate(st, v, cn, sizeof cn, w1, sizeof w1) == 0 && mset_validate(st, cur, cc, sizeof cc, w2, sizeof w2) == 0 && !strcmp(cn, cc)) { if (!e) break; p = e + 1; continue; }
            }
            snprintf(keys[n], sizeof keys[n], "%s", k); snprintf(vals[n], sizeof vals[n], "%s", v); kp[n] = keys[n]; vp[n] = vals[n]; n++;
        }
        if (!e) break; p = e + 1;
    }
    return n;
}
static void lset_apply(const char *text);   // the loader's own settings (below)
static void mods_ui_form_apply(int row, const char *text) {
    if (row == MUI_FORM_LOADER && g_form_row == MUI_FORM_LOADER) { lset_apply(text); return; }
    if (row != g_form_row || g_form_mi < 0) return;
    int mi = g_form_mi; const ModSettings *set = g_mset[mi]; if (!set) return;
    wchar_t cp[MAX_PATH]; swprintf(cp, MAX_PATH, L"%ls\\plugin.cfg", g_mi_dir[mi]); char *cfg = mods_read_text(cp, 65536);
    const char *kp[MSET_MAX], *vp[MSET_MAX]; int n = mui_form_changes(set, cfg, text, kp, vp);
    if (n == 0) { snprintf(g_form_msg, sizeof g_form_msg, "Nothing changed."); g_form_ok = 1; free(cfg); return; }
    static char out[65536 + 4096]; char err[200] = "";
    if (mset_apply(set, cfg, n, kp, vp, out, sizeof out, err, sizeof err) < 0) {
        snprintf(g_form_msg, sizeof g_form_msg, "Nothing was saved. %.150s", err); g_form_ok = 0; free(cfg);
        logf_("mods page: settings of %s rejected: %s", g_mi[mi].name, err); return;
    }
    wchar_t tmp[MAX_PATH]; swprintf(tmp, MAX_PATH, L"%ls\\plugin.cfg.tmp", g_mi_dir[mi]);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD w = 0; int ok = 0; size_t len = strlen(out);
    if (h != INVALID_HANDLE_VALUE) { ok = WriteFile(h, out, (DWORD)len, &w, NULL) && w == (DWORD)len; CloseHandle(h); }
    if (ok) ok = MoveFileExW(tmp, cp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); else DeleteFileW(tmp);
    free(cfg);
    if (!ok) { snprintf(g_form_msg, sizeof g_form_msg, "Nothing was saved: plugin.cfg could not be written (error %lu).", GetLastError()); g_form_ok = 0; logf_("mods page: %s", g_form_msg); return; }
    int notified = 0;
    for (int p = 0; p < g_plugins_n; p++) if (!_stricmp(g_plugins[p].name, g_mi[mi].name) && g_plugins[p].loaded && g_plugins[p].module && plug_guard(p)) {
        void (*cb)(const AbsApi *) = (void (*)(const AbsApi *))GetProcAddress(g_plugins[p].module, "gleam_settings_changed");
        if (cb) { __try { cb(&g_plugins[p].api); notified = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "gleam_settings_changed", GetExceptionCode()); } }
    }
    char done[200]; snprintf(done, sizeof done, n ? "Saved %d setting(s) to plugin.cfg. %s" : "Nothing changed (%d). %s", n, n ? (notified ? "The mod applied them." : "They take effect after a restart.") : "");
    mods_ui_form_open(row); snprintf(g_form_msg, sizeof g_form_msg, "%s", done); g_form_ok = 1;
    logf_("mods page: settings of %s saved (%d value(s)); %s", g_mi[mi].name, n, notified ? "the plugin was told (gleam_settings_changed)" : "restart needed");
}

// ----- the loader's own settings (F1 > Settings). The mod-settings form (same schema as a mod.json "settings" list) over LanNative\config.txt.
// Every option below is read once when the game starts, so a change applies after a restart (the tab says so). config.txt absent: it is created, unless the
// install still has old one-file-per-setting files (config.txt replaces them all at once: the write is refused and the tab says which file to move first).
static const char g_lset_json[] = "{\"settings\":["
    "{\"key\":\"menu_style\",\"type\":\"choice\",\"choices\":[\"imgui\",\"text\"],\"default\":\"imgui\",\"label\":\"Menu style\",\"help\":\"imgui = these windows; text = the plain text menu the game draws (no tabs, no mod tabs).\"},"
    "{\"key\":\"pad_menu\",\"type\":\"choice\",\"choices\":[\"on\",\"off\"],\"default\":\"on\",\"label\":\"Gamepad in the menus\",\"help\":\"on = Back + Start opens the loader window; d-pad / A / B move in the menus.\"},"
    "{\"key\":\"keep_keys\",\"type\":\"text\",\"default\":\"F11\",\"label\":\"Keys kept from the game\",\"help\":\"F keys the game never gets in a Modded start (comma list, or none). F11 = the game's fullscreen toggle.\"},"
    "{\"key\":\"content_paks\",\"type\":\"choice\",\"choices\":[\"stock\",\"all\"],\"default\":\"stock\",\"label\":\"Paks in Content\\\\Paks\",\"help\":\"stock = only the game's own paks + Mods\\\\ (vanilla pak mods stay for Vanilla starts); all = every pak there.\"},"
    "{\"key\":\"lanfinder\",\"type\":\"choice\",\"choices\":[\"on\",\"off\"],\"default\":\"on\",\"label\":\"LAN finder\",\"help\":\"on = find LAN games at an altar / F2 > LAN games, and answer while hosting.\"},"
    "{\"key\":\"modmismatch\",\"type\":\"choice\",\"choices\":[\"warn\",\"block\",\"off\"],\"default\":\"warn\",\"label\":\"Mod check when joining\",\"help\":\"warn = say when host and joiner have different gameplay mods; block = refuse such a joiner; off = no check.\"},"
    "{\"key\":\"autoleave\",\"type\":\"choice\",\"choices\":[\"on\",\"off\"],\"default\":\"on\",\"label\":\"Auto-leave a duel\",\"help\":\"on = when the other player's connection is gone, go back to your own game.\"},"
    "{\"key\":\"perteam\",\"type\":\"choice\",\"choices\":[\"1\",\"2\",\"3\"],\"default\":\"1\",\"label\":\"3v3 maps: players per team\",\"help\":\"How many players a hosted 3v3 map waits for per team (3 = the game's own).\"},"
    "{\"key\":\"soloduel\",\"type\":\"choice\",\"choices\":[\"off\",\"on\"],\"default\":\"off\",\"label\":\"Solo duel\",\"help\":\"on = a 1v1 / 3v3 map you host starts with only you in it (map testing).\"},"
    "{\"key\":\"npc\",\"type\":\"choice\",\"choices\":[\"on\",\"off\"],\"default\":\"on\",\"label\":\"NPC spawning\",\"help\":\"on = F2 > Spawn NPC works when you are the host.\"},"
    "{\"key\":\"parryfix\",\"type\":\"choice\",\"choices\":[\"on\",\"off\"],\"default\":\"on\",\"label\":\"Parry fix\",\"help\":\"on = the Forsaken parry bug fix (in memory, the game files are not changed).\"},"
    "{\"key\":\"pvpfix\",\"type\":\"choice\",\"choices\":[\"on\",\"guard\",\"off\"],\"default\":\"on\",\"label\":\"PvP fixes\",\"help\":\"on = send-back guard + duel flow; guard = the guard only; off = neither.\"},"
    "{\"key\":\"perf_stats\",\"type\":\"choice\",\"choices\":[\"on\",\"off\"],\"default\":\"on\",\"label\":\"Frame time log\",\"help\":\"on = frame time lines in lannative.log every 30 s.\"}"
    "]}";
static ModSettings g_lset; static int g_lset_state;                                   // 0 not parsed yet, 1 ok, -1 the list above does not parse (logged)
static const wchar_t *const g_lset_legacy[] = { L"menu_style.txt", L"menu.txt", L"autoleave.txt", L"perteam.txt", L"soloduel.txt", L"npc.txt", L"npcrepl.txt", L"parryfix.txt",
    L"pvpfix.txt", L"lanfinder.txt", L"modmismatch.txt", L"lan.txt", L"hostip.txt", L"telemetry.txt", L"maxplayers.txt", L"moddownload.txt", L"allow.txt" };
static int lset_legacy_file(char *which, size_t cap) {                                // 1 = config.txt absent and an old per-setting file exists (its name in which)
    if (g_cfg_present) return 0;
    for (size_t i = 0; i < sizeof g_lset_legacy / sizeof g_lset_legacy[0]; i++) {
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%s%ls", g_dir, g_lset_legacy[i]);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { w_to_u8(g_lset_legacy[i], which, (int)cap); return 1; }
    }
    return 0;
}
static void lset_form_open(void) {
    if (!g_lset_state) { char err[200] = ""; g_lset_state = mset_parse(g_lset_json, &g_lset, err, sizeof err) == 0 && g_lset.n ? 1 : -1; logf_("settings: loader settings list: %d option(s)%s%s", g_lset.n, err[0] ? " - " : "", err); }
    g_form_n = 0; memset(g_form, 0, sizeof g_form);
    for (int i = 0; g_lset_state > 0 && i < g_lset.n && g_form_n < OV_SETTINGS; i++) {
        const ModSetting *st = &g_lset.s[i]; OvSetting *d = &g_form[g_form_n++];
        snprintf(d->key, sizeof d->key, "%s", st->key); snprintf(d->label, sizeof d->label, "%s", st->label); snprintf(d->help, sizeof d->help, "%s", st->help); snprintf(d->def, sizeof d->def, "%s", st->def);
        d->type = st->type; d->has_min = st->has_min; d->has_max = st->has_max; d->min = st->min; d->max = st->max; d->nchoice = st->nchoice; memcpy(d->choice, st->choice, sizeof d->choice);
        char v[MSET_TEXT]; if (!g_cfg_present || !mset_cfg_get(g_cfg_text, st->key, v, sizeof v)) snprintf(v, sizeof v, "%s", st->def); snprintf(d->value, sizeof d->value, "%s", v);
    }
    g_form_row = MUI_FORM_LOADER; g_form_mi = -1; g_form_ok = 0; g_form_live = 0; g_form_serial++; snprintf(g_form_title, sizeof g_form_title, "Loader settings");
    char old[64]; if (lset_legacy_file(old, sizeof old)) snprintf(g_form_msg, sizeof g_form_msg, "This install still uses old per-setting files (%s): put those values in LanNative\\config.txt first; Apply stays refused until then.", old);
    else g_form_msg[0] = 0;
    logf_("settings: loader settings form opened (%d option(s), config.txt %s)", g_form_n, g_cfg_present ? "present" : "absent - Apply creates it");
}
static void lset_apply(const char *text) {
    char old[64]; if (lset_legacy_file(old, sizeof old)) { snprintf(g_form_msg, sizeof g_form_msg, "Nothing was saved: old per-setting file %s exists; LanNative\\config.txt would replace it.", old); g_form_ok = 0; logf_("settings: %s", g_form_msg); return; }
    const char *kp[MSET_MAX], *vp[MSET_MAX]; int n = mui_form_changes(&g_lset, g_cfg_present ? g_cfg_text : NULL, text, kp, vp);
    if (!n) { snprintf(g_form_msg, sizeof g_form_msg, "Nothing changed."); g_form_ok = 1; return; }
    static char out[sizeof g_cfg_text]; char err[200] = "";
    if (mset_apply(&g_lset, g_cfg_present ? g_cfg_text : "", n, kp, vp, out, sizeof out, err, sizeof err) < 0) { snprintf(g_form_msg, sizeof g_form_msg, "Nothing was saved. %.150s", err); g_form_ok = 0; logf_("settings: rejected: %s", err); return; }
    wchar_t path[MAX_PATH], tmp[MAX_PATH]; swprintf(path, MAX_PATH, L"%sconfig.txt", g_dir); swprintf(tmp, MAX_PATH, L"%sconfig.txt.tmp", g_dir);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD w = 0; int ok = 0; size_t len = strlen(out);
    if (h != INVALID_HANDLE_VALUE) { ok = WriteFile(h, out, (DWORD)len, &w, NULL) && w == (DWORD)len; CloseHandle(h); }
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); else DeleteFileW(tmp);
    if (!ok) { snprintf(g_form_msg, sizeof g_form_msg, "Nothing was saved: config.txt could not be written (error %lu).", GetLastError()); g_form_ok = 0; logf_("settings: %s", g_form_msg); return; }
    int created = !g_cfg_present; memcpy(g_cfg_text, out, len + 1); g_cfg_present = 1;
    for (int i = 0; i < n; i++) logf_("settings: %s = %s (LanNative\\config.txt%s; applies after a restart)", kp[i], vp[i], created ? ", created" : "");
    lset_form_open(); snprintf(g_form_msg, sizeof g_form_msg, "Saved %d setting(s) to LanNative\\config.txt%s. They apply after a restart.", n, created ? " (created)" : ""); g_form_ok = 1;
}
// ----- the page
static MuiRef g_mui_ref[MUI_ACTIONS]; static int g_mui_nref, g_mui_open = -1;          // what each action line does; the open section (plugin index, -1 = all closed)
static int g_mui_nmitem;                                                              // menu items at the last refresh: MUR_ITEM refs past it are ui rows (uirows.inc)
static int mui_ui_row(int index) {                                                    // action line -> ui row index, -1 = not a ui row
    return index > 0 && index < g_mui_nref && g_mui_ref[index].kind == MUR_ITEM && g_mui_ref[index].ref >= g_mui_nmitem ? g_mui_ref[index].ref - g_mui_nmitem : -1;
}
#define MUI_UNITS 8
static const LnRows *g_mui_unit[MUI_UNITS]; static int g_mui_unit_n[MUI_UNITS], g_mui_units;   // the units' rows after the plugin sections (lnhooks.h LN_MODS_ROWS): whose, and how many each got
static ModsUiIn g_mui; static char g_mui_lines[MENU_LIST_MAX][MUI_LINE]; static int g_mui_n; static ULONGLONG g_mui_at;
// tabs of the overlay's Mods window (overlay.h OVT_*). While the overlay draws the menu, the page lists only the active tab's rows; the text menu
// (menu_style = text, or no overlay) keeps the one long list. Each unit's rows belong to one tab (by unit name); -1 = drawn by the overlay itself in tab mode.
static int g_mui_tab;                                                                 // OVT_* or OVT_PLUGIN + plugin index (API 2.33 ui_tab)
#ifdef LN_OVERLAY
#define mui_tabs_on() ov_text_hidden()
#else
#define mui_tabs_on() 0
#define OVT_MODS 0
#endif
static int mui_unit_tab(const char *unit) {
#ifdef LN_OVERLAY
    if (!strcmp(unit, "keys")) return OVT_KEYS;
    if (!strcmp(unit, "hostmods") || !strcmp(unit, "netinspect")) return OVT_HOSTING;
    if (!strcmp(unit, "logwin") || !strcmp(unit, "profiles")) return -1;              // the Log tab draws the log itself; the Mods tab has the profile bar
#endif
    (void)unit; return OVT_MODS;
}
static void modsui_refresh(void) {
    ModsUiIn *in = &g_mui; memset(in, 0, sizeof *in);
    in->mods = g_mi; in->nmods = g_mi_n; in->safe_mode = g_safe_mode; in->safe_why = g_safe_why; in->order = g_mi_order;
    in->pak_conflicts = g_mi_pakc; in->pak_conflicts_real = g_mi_pakc_real; in->row_conflicts = g_mi_rowc; in->obj_conflicts = g_mi_objc; in->split_assets = g_mi_split;
    in->c_files = g_c_nfiles; in->c_entries = g_c_inuse; in->c_invalid = g_c_invalid_n; in->ncconf = g_cconf_n; for (int i = 0; i < g_cconf_n && i < MUI_CCONF; i++) snprintf(in->cconf[i], sizeof in->cconf[i], "%s", g_cconf[i]);
    for (int i = 0; i < g_plugins_n && in->nplug < MUI_PLUG; i++) { if (g_plugins[i].builtin) continue; in->plug_name[in->nplug] = g_plugins[i].name; in->plug_faults[in->nplug] = g_plugins[i].faults; in->plug_loaded[in->nplug] = g_plugins[i].loaded; in->nplug++; }
    in->nwarn = g_mi_warn_n < MUI_WARN ? g_mi_warn_n : MUI_WARN; for (int i = 0; i < in->nwarn; i++) snprintf(in->warn[i], sizeof in->warn[i], "%s", g_mi_warn[i]);
    for (int k = 0; in->nwarn < MUI_WARN && grants_warn_line(k, in->warn[in->nwarn], sizeof in->warn[0]); k++) in->nwarn++;   // live: grants still waiting for the player's inventory (content.inc)
    in->reload_status = g_reload_status; in->info = g_vo_info;                    // modglue.inc: content paks rule info line
    static char act[MUI_ACTIONS][MUI_LINE]; in->nactions = 0;
    int tabs = mui_tabs_on();
    g_mui_nref = 0; g_mui_nmitem = g_mitem_n;
    if (!tabs || g_mui_tab == OVT_MODS) {                                          // reload + plugin sections: the Mods tab (or the text menu)
    snprintf(act[0], MUI_LINE, "Reload Mods\\*\\content and rows.json now"); in->action[in->nactions++] = act[0];
    int key[MITEM_MAX + UIR_MAX]; const char *nm[MITEM_MAX + UIR_MAX], *tx[MITEM_MAX + UIR_MAX];   // plugin items, then the plugins' ui rows (uirows.inc): one collapsible section per plugin (modsui_sections)
    g_mui_nmitem = g_mitem_n;                                                      // item index >= this = ui row (index - this)
    for (int i = 0; i < g_mitem_n; i++) { key[i] = g_mitem[i].plugin; nm[i] = plug_name_of(g_mitem[i].plugin); tx[i] = g_mitem[i].text; }
    int nui = uir_lines(); for (int i = 0; i < nui; i++) { int k = g_mitem_n + i; key[k] = uir_plugin(i); nm[k] = plug_name_of(key[k]); tx[k] = g_uir_line[i]; }
    int ns = modsui_sections(g_mitem_n + nui, key, nm, tx, g_mui_open, act + 1, g_mui_ref + 1, MUI_ACTIONS - 1);
    g_mui_ref[0].kind = 0; g_mui_ref[0].ref = 0; g_mui_nref = 1 + ns;
    for (int i = 1; i <= ns; i++) in->action[in->nactions++] = act[i];
    }
    g_mui_units = 0; int dropped = 0; static int dropped_last;                    // dropped_last: units past the cap at the previous refresh (logged already)
    LN_FOREACH(modsrows, LnRows, u) {                                             // the units' rows, in key order: host mods, Net Inspector
        if (g_mui_units >= MUI_UNITS) {                                           // never silently: each unit past the cap is logged once (the page refreshes every 0.5 s)
            dropped++;
            if (dropped > dropped_last) logf_("mods page: unit '%s' (key %s) has rows but the page holds %d units (MUI_UNITS) - its rows are NOT shown; raise MUI_UNITS in modsui.inc", u->name, u->key, MUI_UNITS);
            continue;
        }
        if (tabs && mui_unit_tab(u->name) != g_mui_tab) continue;                // tab mode: only the active tab's units (their count() is not even called)
        int got = 0; for (int i = 0, n = u->count(); i < n && in->nactions < MUI_ACTIONS; i++, got++) in->action[in->nactions++] = u->label(i);
        g_mui_unit[g_mui_units] = u; g_mui_unit_n[g_mui_units++] = got;
    }
    dropped_last = dropped;
    g_mui_n = modsui_build(in, g_mui_lines, MENU_LIST_MAX); g_mui_at = GetTickCount64();
}
static int modsui_count(void) { if (GetTickCount64() - g_mui_at > 500) modsui_refresh(); return g_mui_n; }
static const char *modsui_line(int i) { return i >= 0 && i < g_mui_n ? g_mui_lines[i] : ""; }
static void modsui_action(int index) {
    if (index == 0 && g_mui_nref > 0) { mods_reload_all(); g_mui_at = 0; return; }
    if (index >= g_mui_nref) {                                                    // a unit's row: the unit that owns it gets its own row index
        int at = index - g_mui_nref;
        for (int k = 0; k < g_mui_units; k++) { if (at < g_mui_unit_n[k]) { g_mui_unit[k]->action(at); break; } at -= g_mui_unit_n[k]; }
        g_mui_at = 0; return;
    }
    if (index < 1) return;
    if (g_mui_ref[index].kind == MUR_HEAD) {
        int k = g_mui_ref[index].ref; g_mui_open = g_mui_open == k ? -1 : k; g_mui_at = 0;
        logf_("menu: Mods section %s %s", plug_name_of(k), g_mui_open == k ? "opened" : "closed"); return;
    }
    if (mui_ui_row(index) >= 0) { uir_choose(mui_ui_row(index), 0, 0, "Mods page"); g_mui_at = 0; return; }
    if (g_mui_ref[index].kind == MUR_ITEM && g_mui_ref[index].ref < g_mitem_n) {
        MenuItemReg *m = &g_mitem[g_mui_ref[index].ref]; if (!plug_guard(m->plugin)) { logf_("menu: item '%s' is switched off (its plugin faulted)", m->label); return; }
        logf_("menu: plugin item '%s'", m->label);
        __try { m->cb(m->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(m->plugin, "menu item", GetExceptionCode()); }
        g_mui_at = 0;
    }
}
#ifdef LN_OVERLAY
static void pf_command(const char *name, int overwrite); static void pf_snapshot(OvSnap *o); static void pf_scan(void); static void pf_switch(int i);   // profiles.inc (included after this unit)
static void mui_set_tab(int t, const char *why) {
    if (!((t >= 0 && t < OVT_COUNT) || (t >= OVT_PLUGIN && t < OVT_PLUGIN + PLUG_MAX))) { logf_("mods page: tab %d refused (no such tab)", t); return; }
    if (t == g_mui_tab) return;
    logf_("mods page: tab %d -> %d%s%s (%s)", g_mui_tab, t, t >= OVT_PLUGIN ? " = plugin " : "", t >= OVT_PLUGIN ? plug_name_of(t - OVT_PLUGIN) : "", why);
    if (g_form_row == MUI_FORM_LOADER) mods_ui_form_close();                     // leaving Settings
    g_mui_tab = t; g_menu.sel = 0; g_mui_at = 0;
    if (t == OVT_SETTINGS) lset_form_open();
    if (t == OVT_LOG) { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%slannative.log", g_dir); ov_log_window(p, 0); }   // the Log tab shows the log (the separate window closes)
}
static void modsui_command(const OvCmd *c) {
    char msg[200] = "";
    switch (c->cmd) {
    case OVC_MODS_ENABLE:
        if (g_safe_mode || c->index < 0 || c->index >= g_order_now.count) break;
        { int on = c->arg != 0; mo_set_enabled(&g_order_now, g_order_now.mods[c->index].name, on);
          logf_("mods page: %s %s", g_order_now.mods[c->index].name, on ? "switched on" : "switched off");
          if (mods_ui_write_order(msg, sizeof msg)) { logf_("mods page: %s", msg); mo_set_enabled(&g_order_now, g_order_now.mods[c->index].name, !on); snprintf(g_reload_status, sizeof g_reload_status, "%s", msg); break; }
          if (on) { int mi = mods_ui_mi_of(g_order_now.mods[c->index].name); if (mi >= 0) mods_ui_hot_enable(mi); } }
        break;
    case OVC_MODS_MOVE:
        if (g_safe_mode) break;
        { char nm[MOD_NAME_MAX]; if (c->index < 0 || c->index >= g_order_now.count) break; snprintf(nm, sizeof nm, "%s", g_order_now.mods[c->index].name);
          int to = mo_move(&g_order_now, c->index, c->arg); if (to < 0) break;
          logf_("mods page: %s moved %s (now position %d of %d)", nm, c->arg < 0 ? "earlier" : "later", to + 1, g_order_now.count);
          if (mods_ui_write_order(msg, sizeof msg)) { logf_("mods page: %s", msg); mo_move(&g_order_now, to, c->arg < 0 ? 1 : -1); snprintf(g_reload_status, sizeof g_reload_status, "%s", msg); } }
        break;
    case OVC_MODS_RESTART: logf_("mods page: restart requested - the game quits now"); md_console("quit"); break;
    case OVC_MODS_FORM: if (c->index < 0) mods_ui_form_close(); else mods_ui_form_open(c->index); break;
    case OVC_MODS_APPLY: mods_ui_form_apply(c->index, c->text); break;
    case OVC_PROFILE_SAVE: pf_command(c->text, c->arg); break;
    case OVC_MODS_TAB: mui_set_tab(c->index, "tab bar"); break;
    case OVC_MODS_CLOSE: if (g_menu.open && g_menu.page == MP_MODS) { menu_key(&g_menu, MK_MODS); logf_("menu: loader window closed (X / Esc / pad B)"); } break;
    case OVC_PROFILE_SWITCH: logf_("profiles: bar: switch to profile %d", c->index); pf_scan(); pf_switch(c->index); break;
    case OVC_UI_SET: { double v; if (uir_kind(c->arg) == UIR_NUMBER && uir_parse(c->text, &v)) uir_choose(c->arg, 1, v, "Mods page slider"); else logf_("mods page: slider value '%.40s' for ui row %d ignored (not a number row)", c->text, c->arg); } break;
    default: break;
    }
    g_mui_at = 0;
}
#endif
#ifdef LN_OVERLAY
// the same data the text page is built from, copied for the overlay's render thread (overlay.h OvSnap)
static void modsui_snapshot(OvSnap *o) {
    modsui_count();                                                               // refreshes g_mui when it is older than 500 ms
    const ModsUiIn *in = &g_mui;
    o->kind = OV_MODS; snprintf(o->title, OV_LINE, "Mods"); o->tab = g_mui_tab;
    o->safe_mode = in->safe_mode; snprintf(o->safe_why, sizeof o->safe_why, "%s", in->safe_why ? in->safe_why : "");
    snprintf(o->order, sizeof o->order, "%s", in->order ? in->order : "");
    o->nmods = 0;                                                                 // one row per entry of loadorder.txt as it is now (the page can reorder it); the row index is the index in g_order_now
    for (int r = 0; r < g_order_now.count && o->nmods < OV_MODS_MAX; r++) {
        int mi = mods_ui_mi_of(g_order_now.mods[r].name); if (mi < 0) continue;
        const ModInfo *m = &in->mods[mi]; OvMod *d = &o->mods[o->nmods++]; memset(d, 0, sizeof *d);
        snprintf(d->name, sizeof d->name, "%s", m->name); snprintf(d->id, sizeof d->id, "%s", m->id); snprintf(d->version, sizeof d->version, "%s", m->version); snprintf(d->reason, sizeof d->reason, "%s", m->reason);
        snprintf(d->ldr_note, sizeof d->ldr_note, "%s", m->ldr_note);
        d->status = m->status; d->paks_ok = m->paks_ok; d->paks_total = m->paks_total; d->rows_ops = m->rows_ops; d->content_files = m->content_files; d->plugin = m->plugin; d->affects_gameplay = m->affects_gameplay; snprintf(d->net_tag, sizeof d->net_tag, "%s", m->net_tag); d->joiners_optional = m->joiners_optional;
        d->enabled = g_order_now.mods[r].enabled; d->effect = mods_ui_effect(mi, r);
        d->has_pak = m->has_pak; d->has_lua = m->has_lua; d->has_rows = m->has_rows; d->has_content = m->has_content; d->has_plugin = m->has_plugin; d->nsettings = m->nsettings; d->nconf = m->nconf;
        { ModKind k = { m->has_pak, m->has_plugin, m->has_lua, m->requires }; d->hot_ok = mo_hot_enable_ok(&k) && (m->has_rows || m->has_content); }
        memcpy(d->conf, m->conf, sizeof d->conf);
    }
    { ModDiff df = mo_diff(&g_order_boot, &g_order_now); o->pending_n = mo_pending(&df); if (o->pending_n) mo_describe(&df, o->pending_text, sizeof o->pending_text); else o->pending_text[0] = 0; }
    o->form_mod = g_form_row; o->form_serial = g_form_serial; snprintf(o->form_title, sizeof o->form_title, "%s", g_form_title); snprintf(o->form_msg, sizeof o->form_msg, "%s", g_form_msg);
    o->form_ok = g_form_ok; o->form_live = g_form_live; o->nform = g_form_n; memcpy(o->form, g_form, sizeof(OvSetting) * (size_t)g_form_n);
    o->pak_conflicts = in->pak_conflicts; o->pak_conflicts_real = in->pak_conflicts_real; o->row_conflicts = in->row_conflicts; o->obj_conflicts = in->obj_conflicts; o->split_assets = in->split_assets;
    o->nwarn = in->nwarn < OV_WARN ? in->nwarn : OV_WARN; for (int i = 0; i < o->nwarn; i++) snprintf(o->warn[i], sizeof o->warn[i], "%s", in->warn[i]);
    snprintf(o->info, sizeof o->info, "%s", in->info ? in->info : "");
    o->c_files = in->c_files; o->c_entries = in->c_entries; o->c_invalid = in->c_invalid;
    o->ncconf = in->ncconf < OV_CCONF ? in->ncconf : OV_CCONF; for (int i = 0; i < o->ncconf; i++) snprintf(o->cconf[i], sizeof o->cconf[i], "%s", in->cconf[i]);
    o->nplug = in->nplug < OV_PLUG ? in->nplug : OV_PLUG;
    for (int i = 0; i < o->nplug; i++) { snprintf(o->plug_name[i], sizeof o->plug_name[i], "%s", in->plug_name[i]); o->plug_faults[i] = in->plug_faults[i]; o->plug_loaded[i] = in->plug_loaded[i]; }
    snprintf(o->reload_status, sizeof o->reload_status, "%s", in->reload_status ? in->reload_status : "");
    o->nactions = in->nactions < OV_ACTIONS ? in->nactions : OV_ACTIONS;
    for (int i = 0; i < o->nactions; i++) {
        snprintf(o->action[i], OV_LINE, "%s", in->action[i]); o->action_kind[i] = i < g_mui_nref ? g_mui_ref[i].kind : 0;
        int u = mui_ui_row(i), k = uir_kind(u);                                   // ui rows: 3 = number (slider), 4 = hint (grey text)
        if (k == UIR_NUMBER) { o->action_kind[i] = 3; uir_range(u, o->action_num[i]); } else if (k == UIR_HINT) o->action_kind[i] = 4;
        // the reload row, plugin menu items and ui buttons are buttons (14); ui toggles are check boxes ("[x] label", 6 / 7)
        else if (k == UIR_TOGGLE) { double r[5]; uir_range(u, r); EnterCriticalSection(&g_lock); snprintf(o->action[i], OV_LINE, "[%c] %s", r[0] != 0 ? 'x' : ' ', u >= 0 && u < g_uir_n ? g_uir[u].label : ""); LeaveCriticalSection(&g_lock); o->action_kind[i] = r[0] != 0 ? 6 : 7; }
        else if (k == UIR_BUTTON || (i == 0 && g_mui_nref > 0) || (i < g_mui_nref && g_mui_ref[i].kind == MUR_ITEM)) o->action_kind[i] = 14;
        if (i >= g_mui_nref) {                                                    // units' rows: a marker at the start says how the overlay draws the row
            char *t = o->action[i];
            if (t[0] == '\x01') { o->action_kind[i] = 5; memmove(t, t + 1, strlen(t)); }          // group header
            else if (t[0] == '\x02') { o->action_kind[i] = 9; memmove(t, t + 1, strlen(t)); }     // grey text
            else if (t[0] == '\x03') { memmove(t, t + 1, strlen(t)); o->action_kind[i] = !strncmp(t, "[x] ", 4) ? 10 : 11; }   // big switch on / off (Hosting)
            else if (!strncmp(t, "[x] ", 4)) o->action_kind[i] = 6;                                  // check box on
            else if (!strncmp(t, "[ ] ", 4)) o->action_kind[i] = 7;                                  // check box off
            else if (strchr(t, '\t')) o->action_kind[i] = 8;                                        // columns
        }
    }
    pf_snapshot(o);
}
#endif
// Left on the Mods page while a section is open: close it; returns the line to select (the section's header)
static int modsui_back(int sel) {
    int k = g_mui_open; if (k < 0) return sel;
    g_mui_open = -1; logf_("menu: Mods section %s closed (Left)", plug_name_of(k)); modsui_refresh();
    for (int i = 1; i < g_mui_nref; i++) if (g_mui_ref[i].kind == MUR_HEAD && g_mui_ref[i].ref == k) return i;
    return 0;
}
static int modsui_open_section(void) { return g_mui_open >= 0; }
static int modsui_form_close_if_open(void) { if (g_form_row < 0) return 0; logf_("mods page: settings form closed (Back)"); mods_ui_form_close(); g_mui_at = 0; return 1; }
static void modsui_collapse(void) { if (g_mui_open >= 0) { g_mui_open = -1; g_mui_at = 0; } if (g_mui_tab != OVT_MODS) { g_mui_tab = OVT_MODS; g_mui_at = 0; } if (g_form_row == MUI_FORM_LOADER) mods_ui_form_close(); }   // F1 opens on the Mods tab
// called every game tick with the window focus state: LanNative\reload.txt
static void modsui_tick(int focus) {
    static ULONGLONG next_poll; ULONGLONG now = GetTickCount64();
    if (now >= next_poll) {
        next_poll = now + 1000; wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sreload.txt", g_dir);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(p); logf_("reload: LanNative\\reload.txt found"); mods_reload_all(); }
    }
}
LN_TICK(0800, modsui) { modsui_tick(g_ln_focus); }
#ifdef LN_OVERLAY
// ----- API 2.33 : mod tabs in the Mods window (Cyber Engine Tweaks style; docs/API-REFERENCE.md)
static int api_p_ui_tab(const AbsApi *self, const char *title, void (*draw)(void *user), void *user) {
    int p = plug_of(self); if (p < 0 || p >= PLUG_MAX || !draw) return 1;
    int rc = ovapi_up(p, "ui_tab"); if (rc) return rc;
    char t[40]; snprintf(t, sizeof t, "%s", title && title[0] ? title : plug_name_of(p));
    rc = ov_tab_register(p, t, draw, user); logf_("plugin %s: ui_tab '%s' -> %d", plug_name_of(p), t, rc); return rc;
}
static int api_p_ui_tab_show(const AbsApi *self, int show) {                     // game thread: 1 open the Mods window on the caller's tab, 0 close it (only when it shows that tab), 2 toggle
    int p = plug_of(self); if (p < 0 || p >= PLUG_MAX || show < 0 || show > 2) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: ui_tab_show(%d) refused - not on the game thread", plug_name_of(p), show); return -1; }
    { char t[40]; int has = 0; for (int i = 0, o; (o = ov_tab_owner_at(i, t, sizeof t)) >= 0; i++) if (o == p) has = 1;
      if (!has) { logf_("plugin %s: ui_tab_show refused - no ui_tab registered", plug_name_of(p)); return -1; } }
    if (!mui_tabs_on() && ov_state() != OV_STARTING) { logf_("plugin %s: ui_tab_show refused - the overlay does not draw the menu (menu_style = text?)", plug_name_of(p)); return -1; }
    int mine = g_menu.open && g_menu.page == MP_MODS && g_mui_tab == OVT_PLUGIN + p;
    if (show == 2) show = !mine;
    if (!show) { if (mine) { menu_key(&g_menu, MK_MODS); logf_("menu: closed by plugin %s (ui_tab_show 0)", plug_name_of(p)); } return 0; }
    if (!(g_menu.open && g_menu.page == MP_MODS)) { menu_key(&g_menu, MK_MODS); logf_("menu: opened on the Mods window by plugin %s (ui_tab_show)", plug_name_of(p)); }
    mui_set_tab(OVT_PLUGIN + p, "ui_tab_show");
    return 1;
}
#endif

// ----- host notices to joiners (API 2.35 net_notice). A host plugin publishes one short line (Cheats: "Host has cheats enabled"); a joiner
// asks its host over the host-mods side channel (verb NOTE, answered even with moddownload = off) when it joins and every 10 s while joined, shows the lines
// at the top of the loader windows (ov_set_notice) and as a toast when they change. No lines = nothing shown. Old hosts answer "ERR bad request" = none.
#define NN_LINE 100
#define NN_MAX 16
static CRITICAL_SECTION g_nn_cs; static INIT_ONCE g_nn_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK nn_init_cb(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_nn_cs); return TRUE; }
static void nn_init(void) { InitOnceExecuteOnce(&g_nn_once, nn_init_cb, NULL, NULL); }
static char g_nn_text[PLUG_MAX][NN_LINE];                                            // host side: each plugin's notice ("" = none)
static char g_nn_got[NN_MAX][NN_LINE + 64]; static int g_nn_got_n; static volatile LONG g_nn_fetching, g_nn_changed; static char g_nn_from[64];
static int nn_any(void) { nn_init(); EnterCriticalSection(&g_nn_cs); int a = 0; for (int i = 0; i < PLUG_MAX && !a; i++) a = g_nn_text[i][0] != 0; LeaveCriticalSection(&g_nn_cs); return a; }
static void nn_serve(UINT_PTR c, const char *masked) {                               // host, server thread: "LNNOTE1 <n>\n" + n lines "<plugin>\t<text>\n"
    enum { NN_WORST = NN_MAX * (MOD_NAME_MAX + NN_LINE + 2) };                      // every line fits: name <= 95 + tab + text <= 99 + newline
    nn_init(); static char out[NN_WORST + 32]; static char body[NN_WORST + 1]; int n = 0, w = 0; body[0] = 0;   // static: one server thread answers at a time
    EnterCriticalSection(&g_nn_cs);
    for (int i = 0; i < PLUG_MAX && n < NN_MAX; i++) if (g_nn_text[i][0]) { int w2 = lm_note_append(body, sizeof body, w, plug_name_of(i), g_nn_text[i]); if (w2 == w) break; w = w2; n++; }
    LeaveCriticalSection(&g_nn_cs);
    int len = snprintf(out, sizeof out, "LNNOTE1 %d\n%s", n, body); if (len < 0) len = 0; if (len >= (int)sizeof out) len = (int)sizeof out - 1;
    int ok = hs_send_all((SOCKET)c, out, len);
    logf_("net notice: NOTE from %s -> %d line(s)%s", masked, n, ok ? "" : " - send failed");
}
LN_HS_VERB(netnotice, "NOTE", nn_serve)
static DWORD WINAPI nn_fetch_thread(LPVOID u) {                                      // joiner, worker thread
    (void)u; char addr[64] = "", line[NN_LINE + 80], got[NN_MAX][NN_LINE + 64]; int n = -1; uint32_t ip = 0; char why[120] = "";
    if (hj_host_addr(addr, sizeof addr, &ip)) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) { DWORD to = 5000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
            struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)g_hm_port); a.sin_addr.s_addr = htonl(ip);
            if (connect(s, (struct sockaddr *)&a, sizeof a)) snprintf(why, sizeof why, "cannot reach TCP %u (%d)", g_hm_port, WSAGetLastError());
            else if (!hs_send_all(s, "NOTE\n", 5) || !hs_recv_line(s, line, sizeof line)) snprintf(why, sizeof why, "no answer");
            else if (sscanf(line, "LNNOTE1 %d", &n) != 1 || n < 0 || n > NN_MAX) { snprintf(why, sizeof why, "answered '%.40s' (an older loader: no notices)", line); n = 0; }
            else for (int i = 0; i < n; i++) {
                if (!hs_recv_line(s, line, sizeof line)) { n = i; break; }
                char *tab = strchr(line, '\t'); if (tab) *tab = 0;
                snprintf(got[i], sizeof got[i], "%.40s: %.*s", line, NN_LINE - 1, tab ? tab + 1 : "");
                for (unsigned char *q = (unsigned char *)got[i]; *q; q++) if (*q < 0x20 || *q == 0x7F) *q = ' ';   // host-supplied network text: no control characters
            }
            closesocket(s); }
    } else snprintf(why, sizeof why, "no host address (ip.txt)");
    if (n < 0) n = 0;
    nn_init(); EnterCriticalSection(&g_nn_cs);
    int changed = n != g_nn_got_n; for (int i = 0; i < n && !changed; i++) changed = strcmp(got[i], g_nn_got[i]) != 0;
    for (int i = 0; i < n; i++) snprintf(g_nn_got[i], sizeof g_nn_got[i], "%s", got[i]); g_nn_got_n = n; snprintf(g_nn_from, sizeof g_nn_from, "%s", addr);
    LeaveCriticalSection(&g_nn_cs);
    if (changed) { InterlockedExchange(&g_nn_changed, 1); logf_("net notice: host %s: %d notice(s)%s%s", addr[0] ? addr : "?", n, why[0] ? " - " : "", why); }
    InterlockedExchange(&g_nn_fetching, 0);
    return 0;
}
// A refused / failed join on this joiner (the host's PreLogin text arrives in UEngine::HandleNetworkFailure): a toast at once and the first line of
// the loader window's notice for 60 s. Game thread. RVA = USCGameEngine::HandleNetworkFailure (the game's override; the engine's delegate calls it).
static char g_jf_text[LM_LINE_MAX]; static ULONGLONG g_jf_until;
typedef void (*NetFail_t)(void *engine, void *world, void *netdriver, int type, const FStringView *err);
static NetFail_t o_netfail;
static void nn_publish_overlay(int toast_too);
static void hk_netfail(void *engine, void *world, void *netdriver, int type, const FStringView *err) {
    __try {
        char e[LM_LINE_MAX] = ""; if (err && err->data && err->num > 1 && err->num < 2000) narrow(err->data, err->num - 1, e, sizeof e);
        char l[1][LM_LINE_MAX];
        logf_("join: network failure type %d '%s'", type, e);
        if (lm_join_fail_text(type, e, l[0], LM_LINE_MAX)) {
            snprintf(g_jf_text, sizeof g_jf_text, "%s", l[0]); g_jf_until = GetTickCount64() + 60000;
            toast_show(l, 1, 12); nn_publish_overlay(0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("join: EXCEPTION 0x%08lX reading a network failure (the game handles it as usual)", GetExceptionCode()); }
    o_netfail(engine, world, netdriver, type, err);
}
LN_INIT(0520, joinfail) {
    if (g_safe_mode) return;
    static const uint8_t e[15] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18 };
    o_netfail = (NetFail_t)api_p_hook_rva(NULL, "USCGameEngine::HandleNetworkFailure", 0x398AA0u, e, 15, (void *)hk_netfail);
    logf_("join: refused-join message %s", o_netfail ? "installed (a joiner sees the host's reason)" : "NOT installed - a refused joiner sees no reason");
}
static void nn_publish_overlay(int toast_too) {                                      // game thread: the lines into the loader windows (+ a toast when they changed)
    char all[400] = ""; int w = 0; char toast[2][LM_LINE_MAX]; int nt = 0;
    if (g_jf_text[0] && GetTickCount64() < g_jf_until) w += snprintf(all, sizeof all, "%s", g_jf_text);
    nn_init(); EnterCriticalSection(&g_nn_cs);
    for (int i = 0; i < g_nn_got_n && w < (int)sizeof all - 8; i++) { w += snprintf(all + w, sizeof all - (size_t)w, "%s%s", w ? "   |   " : "", g_nn_got[i]); if (nt < 2) snprintf(toast[nt++], LM_LINE_MAX, "HOST: %.70s", g_nn_got[i]); }
    LeaveCriticalSection(&g_nn_cs);
#ifdef LN_OVERLAY
    ov_set_notice(all);
#endif
    if (nt && toast_too) toast_show(toast, nt, 8);
}
LN_TICK(0810, netnotice) {
    static ULONGLONG next; static int joined_was; ULONGLONG now = GetTickCount64();
    if (InterlockedExchange(&g_nn_changed, 0)) nn_publish_overlay(1);
    if (now < next) return; next = now + 10000;
    nn_publish_overlay(0);                                                            // again every 10 s: the overlay may have started after the last change
    int joined = hj_server_conn() != NULL;
    if (joined && !g_nn_fetching) { InterlockedExchange(&g_nn_fetching, 1); HANDLE th = CreateThread(NULL, 0, nn_fetch_thread, NULL, 0, NULL); if (th) CloseHandle(th); else InterlockedExchange(&g_nn_fetching, 0); }
    if (!joined && joined_was) { nn_init(); EnterCriticalSection(&g_nn_cs); int had = g_nn_got_n; g_nn_got_n = 0; LeaveCriticalSection(&g_nn_cs); if (had) { logf_("net notice: left the host - notices cleared"); InterlockedExchange(&g_nn_changed, 1); } }
    joined_was = joined;
}
#ifdef ABS_HAS_NET_NOTICE
static int api_p_net_notice(const AbsApi *self, const char *text) {                  // any thread
    int p = plug_of(self); if (p < 0 || p >= PLUG_MAX) return -1;
    char t[NN_LINE]; snprintf(t, sizeof t, "%s", text ? text : ""); for (char *c = t; *c; c++) if (*c == '\t' || *c == '\r' || *c == '\n') *c = ' ';
    nn_init(); EnterCriticalSection(&g_nn_cs); int changed = strcmp(g_nn_text[p], t) != 0; snprintf(g_nn_text[p], sizeof g_nn_text[p], "%s", t); LeaveCriticalSection(&g_nn_cs);
    if (changed) logf_("plugin %s: net_notice '%s'%s", plug_name_of(p), t, t[0] ? " (joiners see it at the top of their loader window)" : " (cleared)");
    if (t[0]) hm_init_once();                                                          // the side channel answers NOTE even with moddownload = off
    return 0;
}
#endif
#ifdef ABS_HAS_NPC_LIST
// API 2.34 npc_list: the F2 > Spawn NPC list (lannative.c npc_load_list: built-in groups + content npcs / bots), read by entry. Game thread (the list is rebuilt there).
static ULONGLONG g_nl_built;
static int api_p_npc_list(const AbsApi *self, int i, char *group, int gcap, char *label, int lcap, char *spawn, int scap, char *cls, int ccap) {
    int p = plug_of(self); if (p < 0) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: npc_list refused - not on the game thread", plug_name_of(p)); return -1; }
    if (i < 0 || !g_nl_built) { npc_load_list(L"npcs.txt"); g_nl_built = GetTickCount64(); logf_("plugin %s: npc_list: %d entries in %d groups", plug_name_of(p), g_npcs_n, g_npc_ngroups); if (i < 0) return g_npcs_n; }
    if (i >= g_npcs_n) return 0;
    int g = 0, start = 0; while (g < g_npc_ngroups && i >= start + g_npc_group_count[g]) start += g_npc_group_count[g++];
    if (group && gcap > 0) snprintf(group, (size_t)gcap, "%s", g < g_npc_ngroups ? g_npc_group_name[g] : "");
    if (label && lcap > 0) snprintf(label, (size_t)lcap, "%s", g_npc_label[i]);
    if (spawn && scap > 0) snprintf(spawn, (size_t)scap, "%s", g_npc_path[i][0] == NPC_ENTRY_MARK ? g_npc_path[i] + 1 : g_npc_path[i]);   // a content entry: its plain id
    if (cls && ccap > 0) snprintf(cls, (size_t)ccap, "%s", g_npc_class[i]);
    return 1;
}
#endif
LN_API(modsui) {
    a->add_menu_item = api_p_add_menu_item; a->reload_content = api_p_reload_content;
#ifdef LN_OVERLAY
    a->ui_tab = api_p_ui_tab; a->ui_tab_show = api_p_ui_tab_show;
#endif
#ifdef ABS_HAS_NPC_LIST
    a->npc_list = api_p_npc_list;
#endif
#ifdef ABS_HAS_NET_NOTICE
    a->net_notice = api_p_net_notice;
#endif
}

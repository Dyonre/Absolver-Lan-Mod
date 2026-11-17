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
static void mods_ui_form_apply(int row, const char *text) {
    if (row != g_form_row || g_form_mi < 0) return;
    int mi = g_form_mi; const ModSettings *set = g_mset[mi]; if (!set) return;
    wchar_t cp[MAX_PATH]; swprintf(cp, MAX_PATH, L"%ls\\plugin.cfg", g_mi_dir[mi]); char *cfg = mods_read_text(cp, 65536);
    static char keys[MSET_MAX][48], vals[MSET_MAX][MSET_TEXT]; const char *kp[MSET_MAX], *vp[MSET_MAX]; int n = 0;
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
// ----- the page
static MuiRef g_mui_ref[MUI_ACTIONS]; static int g_mui_nref, g_mui_open = -1;          // what each action line does; the open section (plugin index, -1 = all closed)
static ModsUiIn g_mui; static char g_mui_lines[MENU_LIST_MAX][MUI_LINE]; static int g_mui_n; static ULONGLONG g_mui_at;
static void modsui_refresh(void) {
    ModsUiIn *in = &g_mui; memset(in, 0, sizeof *in);
    in->mods = g_mi; in->nmods = g_mi_n; in->safe_mode = g_safe_mode; in->safe_why = g_safe_why; in->order = g_mi_order;
    in->pak_conflicts = g_mi_pakc; in->pak_conflicts_real = g_mi_pakc_real; in->row_conflicts = g_mi_rowc; in->obj_conflicts = g_mi_objc; in->split_assets = g_mi_split;
    in->c_files = g_c_nfiles; in->c_entries = g_c_inuse; in->c_invalid = g_c_invalid_n; in->ncconf = g_cconf_n; for (int i = 0; i < g_cconf_n && i < MUI_CCONF; i++) snprintf(in->cconf[i], sizeof in->cconf[i], "%s", g_cconf[i]);
    for (int i = 0; i < g_plugins_n && in->nplug < MUI_PLUG; i++) { if (g_plugins[i].builtin) continue; in->plug_name[in->nplug] = g_plugins[i].name; in->plug_faults[in->nplug] = g_plugins[i].faults; in->plug_loaded[in->nplug] = g_plugins[i].loaded; in->nplug++; }
    in->nwarn = g_mi_warn_n < MUI_WARN ? g_mi_warn_n : MUI_WARN; for (int i = 0; i < in->nwarn; i++) snprintf(in->warn[i], sizeof in->warn[i], "%s", g_mi_warn[i]);
    for (int k = 0; in->nwarn < MUI_WARN && grants_warn_line(k, in->warn[in->nwarn], sizeof in->warn[0]); k++) in->nwarn++;   // live: grants still waiting for the player's inventory (content.inc)
    in->reload_status = g_reload_status;
    static char act[MUI_ACTIONS][MUI_LINE]; in->nactions = 0;
    snprintf(act[0], MUI_LINE, "Reload Mods\\*\\content and rows.json now"); in->action[in->nactions++] = act[0];
    int key[MITEM_MAX]; const char *nm[MITEM_MAX], *tx[MITEM_MAX];                     // plugin items: one collapsible section per plugin (modsui_sections)
    for (int i = 0; i < g_mitem_n; i++) { key[i] = g_mitem[i].plugin; nm[i] = plug_name_of(g_mitem[i].plugin); tx[i] = g_mitem[i].text; }
    int ns = modsui_sections(g_mitem_n, key, nm, tx, g_mui_open, act + 1, g_mui_ref + 1, MUI_ACTIONS - 1);
    g_mui_ref[0].kind = 0; g_mui_ref[0].ref = 0; g_mui_nref = 1 + ns;
    for (int i = 1; i <= ns; i++) in->action[in->nactions++] = act[i];
    for (int i = 0, n = hm_menu_count(); i < n && in->nactions < MUI_ACTIONS; i++) in->action[in->nactions++] = hm_menu_label(i);   // hostmods.inc rows (after the plugin sections)
    g_mui_n = modsui_build(in, g_mui_lines, MENU_LIST_MAX); g_mui_at = GetTickCount64();
}
static int modsui_count(void) { if (GetTickCount64() - g_mui_at > 500) modsui_refresh(); return g_mui_n; }
static const char *modsui_line(int i) { return i >= 0 && i < g_mui_n ? g_mui_lines[i] : ""; }
static void modsui_action(int index) {
    if (index == 0) { mods_reload_all(); g_mui_at = 0; return; }
    if (index >= g_mui_nref) { hm_menu_action(index - g_mui_nref); g_mui_at = 0; return; }   // hostmods.inc rows
    if (index < 1) return;
    if (g_mui_ref[index].kind == MUR_HEAD) {
        int k = g_mui_ref[index].ref; g_mui_open = g_mui_open == k ? -1 : k; g_mui_at = 0;
        logf_("menu: Mods section %s %s", plug_name_of(k), g_mui_open == k ? "opened" : "closed"); return;
    }
    if (g_mui_ref[index].kind == MUR_ITEM && g_mui_ref[index].ref < g_mitem_n) {
        MenuItemReg *m = &g_mitem[g_mui_ref[index].ref]; if (!plug_guard(m->plugin)) { logf_("menu: item '%s' is switched off (its plugin faulted)", m->label); return; }
        logf_("menu: plugin item '%s'", m->label);
        __try { m->cb(m->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(m->plugin, "menu item", GetExceptionCode()); }
        g_mui_at = 0;
    }
}
#ifdef LN_OVERLAY
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
    o->kind = OV_MODS; snprintf(o->title, OV_LINE, "Mods");
    o->safe_mode = in->safe_mode; snprintf(o->safe_why, sizeof o->safe_why, "%s", in->safe_why ? in->safe_why : "");
    snprintf(o->order, sizeof o->order, "%s", in->order ? in->order : "");
    o->nmods = 0;                                                                 // one row per entry of loadorder.txt as it is now (the page can reorder it); the row index is the index in g_order_now
    for (int r = 0; r < g_order_now.count && o->nmods < OV_MODS_MAX; r++) {
        int mi = mods_ui_mi_of(g_order_now.mods[r].name); if (mi < 0) continue;
        const ModInfo *m = &in->mods[mi]; OvMod *d = &o->mods[o->nmods++]; memset(d, 0, sizeof *d);
        snprintf(d->name, sizeof d->name, "%s", m->name); snprintf(d->id, sizeof d->id, "%s", m->id); snprintf(d->version, sizeof d->version, "%s", m->version); snprintf(d->reason, sizeof d->reason, "%s", m->reason);
        d->status = m->status; d->paks_ok = m->paks_ok; d->paks_total = m->paks_total; d->rows_ops = m->rows_ops; d->content_files = m->content_files; d->plugin = m->plugin; d->affects_gameplay = m->affects_gameplay;
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
    o->c_files = in->c_files; o->c_entries = in->c_entries; o->c_invalid = in->c_invalid;
    o->ncconf = in->ncconf < OV_CCONF ? in->ncconf : OV_CCONF; for (int i = 0; i < o->ncconf; i++) snprintf(o->cconf[i], sizeof o->cconf[i], "%s", in->cconf[i]);
    o->nplug = in->nplug < OV_PLUG ? in->nplug : OV_PLUG;
    for (int i = 0; i < o->nplug; i++) { snprintf(o->plug_name[i], sizeof o->plug_name[i], "%s", in->plug_name[i]); o->plug_faults[i] = in->plug_faults[i]; o->plug_loaded[i] = in->plug_loaded[i]; }
    snprintf(o->reload_status, sizeof o->reload_status, "%s", in->reload_status ? in->reload_status : "");
    o->nactions = in->nactions < OV_ACTIONS ? in->nactions : OV_ACTIONS;
    for (int i = 0; i < o->nactions; i++) { snprintf(o->action[i], OV_LINE, "%s", in->action[i]); o->action_kind[i] = i < g_mui_nref ? g_mui_ref[i].kind : 0; }
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
static void modsui_collapse(void) { if (g_mui_open >= 0) { g_mui_open = -1; g_mui_at = 0; } }
// called every game tick with the window focus state: LanNative\reload.txt
static void modsui_tick(int focus) {
    static ULONGLONG next_poll; ULONGLONG now = GetTickCount64();
    if (now >= next_poll) {
        next_poll = now + 1000; wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sreload.txt", g_dir);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(p); logf_("reload: LanNative\\reload.txt found"); mods_reload_all(); }
    }
}

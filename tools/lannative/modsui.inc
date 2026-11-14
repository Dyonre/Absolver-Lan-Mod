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
    g_mui_n = modsui_build(in, g_mui_lines, MENU_LIST_MAX); g_mui_at = GetTickCount64();
}
static int modsui_count(void) { if (GetTickCount64() - g_mui_at > 500) modsui_refresh(); return g_mui_n; }
static const char *modsui_line(int i) { return i >= 0 && i < g_mui_n ? g_mui_lines[i] : ""; }
static void modsui_action(int index) {
    if (index == 0) { mods_reload_all(); g_mui_at = 0; return; }
    if (index < 1 || index >= g_mui_nref) return;
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
// the same data the text page is built from, copied for the overlay's render thread (overlay.h OvSnap)
static void modsui_snapshot(OvSnap *o) {
    modsui_count();                                                               // refreshes g_mui when it is older than 500 ms
    const ModsUiIn *in = &g_mui;
    o->kind = OV_MODS; snprintf(o->title, OV_LINE, "Mods");
    o->safe_mode = in->safe_mode; snprintf(o->safe_why, sizeof o->safe_why, "%s", in->safe_why ? in->safe_why : "");
    snprintf(o->order, sizeof o->order, "%s", in->order ? in->order : "");
    o->nmods = in->nmods < OV_MODS_MAX ? in->nmods : OV_MODS_MAX;
    for (int i = 0; i < o->nmods; i++) {
        const ModInfo *m = &in->mods[i]; OvMod *d = &o->mods[i];
        snprintf(d->name, sizeof d->name, "%s", m->name); snprintf(d->id, sizeof d->id, "%s", m->id); snprintf(d->version, sizeof d->version, "%s", m->version); snprintf(d->reason, sizeof d->reason, "%s", m->reason);
        d->status = m->status; d->paks_ok = m->paks_ok; d->paks_total = m->paks_total; d->rows_ops = m->rows_ops; d->content_files = m->content_files; d->plugin = m->plugin; d->affects_gameplay = m->affects_gameplay;
    }
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

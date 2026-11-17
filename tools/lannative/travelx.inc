// ---------------------------------------------------------------------------------------------------------------------
// travelx.inc - plugin travel entries in the F1 Map list (API 2.25 add_travel_entry; for example a map reached by a plugin's own travel, such as one
// opened without its cutscene). A plugin names a label, a map path that decides the category / sort place ("like"), and
// a callback that does its own travel; the entry is listed as "plugin-travel:<k>:<like>" (lannative.c discovery) and choosing it runs the callback on
// the game thread inside the plugin's guard.
#define TRAVELX_MAX 16
typedef struct { char label[96], like[256]; int plugin; void (*cb)(void *user); void *user; } TravelEntry;
static TravelEntry g_travelx[TRAVELX_MAX]; static int g_travelx_n;

static int travelx_index(const char *url) {                     // the entry index of a "plugin-travel:<k>:..." url, -1 = a normal map
    if (!url || strncmp(url, "plugin-travel:", 14)) return -1;
    int k = atoi(url + 14); return k >= 0 && k < g_travelx_n ? k : -1;
}
static int travelx_urls(char (*urls)[MENU_URL_MAX], int cap) {  // discovery thread: one url per entry, returns how many were written
    EnterCriticalSection(&g_lock); int n = 0;
    for (int k = 0; k < g_travelx_n && n < cap; k++) snprintf(urls[n++], MENU_URL_MAX, "plugin-travel:%d:%s", k, g_travelx[k].like);
    LeaveCriticalSection(&g_lock); return n;
}
static const char *travelx_label(const char *url) { int k = travelx_index(url); return k >= 0 ? g_travelx[k].label : NULL; }
static int travelx_run(const char *url) {                       // game thread (menu action); 1 = it was a plugin entry
    int k = travelx_index(url); if (k < 0) return 0;
    TravelEntry *e = &g_travelx[k];
    if (!plug_guard(e->plugin)) { logf_("menu: travel entry '%s' is switched off (its plugin %s faulted)", e->label, plug_name_of(e->plugin)); return 1; }
    logf_("menu: plugin travel entry '%s' (%s)", e->label, plug_name_of(e->plugin));
    __try { e->cb(e->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(e->plugin, "travel entry", GetExceptionCode()); }
    return 1;
}
static int api_p_add_travel_entry(const AbsApi *self, const char *label, const char *like_map, void (*cb)(void *user), void *user) {
    int p = plug_of(self); if (p < 0 || !label || !label[0] || !like_map || like_map[0] != '/' || !cb) return 1;
    EnterCriticalSection(&g_lock); int ok = g_travelx_n < TRAVELX_MAX;
    if (ok) { TravelEntry *e = &g_travelx[g_travelx_n]; snprintf(e->label, sizeof e->label, "%s", label); snprintf(e->like, sizeof e->like, "%s", like_map); e->plugin = p; e->cb = cb; e->user = user; g_travelx_n++; }
    LeaveCriticalSection(&g_lock);
    logf_("plugin %s: add_travel_entry('%s', like %s) -> %d%s", plug_name_of(p), label, like_map, ok ? 0 : 1, ok ? "" : " (16 entries in all)");
    return ok ? 0 : 1;                                           // the Map list picks it up at its next scan (every F1 open)
}
LN_API(travelx) { a->add_travel_entry = api_p_add_travel_entry; }

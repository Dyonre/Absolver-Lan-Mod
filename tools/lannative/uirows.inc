// uirows.inc - API 2.27 loader-drawn plugin rows (docs/API-REFERENCE.md). A plugin declares toggle / number / button / hint
// rows; the loader draws them in the plugin's own section of the loader window's Mods tab (text menu and ImGui, next to its add_menu_item entries), keeps toggle /
// number values in the plugin's kv store ("ui:<key>", LanNative\data\<plugin>.txt) and gives every toggle / button an Keybinds-tab action ("ui_<key>",
// unbound by default). Callbacks run on the game thread inside the plugin guard. Pure rules: uirows.h.
// Included by units.inc after keys.inc (binds_declare) and before modsui.inc (which lists the rows).
#include "uirows.h"
static UiRow g_uir[UIR_MAX]; static int g_uir_n;

static int uir_find_locked(int p, const char *key) { for (int i = 0; i < g_uir_n; i++) if (g_uir[i].plugin == p && !strcmp(g_uir[i].key, key)) return i; return -1; }
static int uir_stored(int p, const char *key, double *v) {          // the kv value of a toggle / number, 1 = found and a number
    char k[UIR_KEY + 4], t[64]; snprintf(k, sizeof k, "ui:%s", key);
    return api_p_kv_get(&g_plugins[p].api, k, t, sizeof t) && uir_parse(t, v);
}
static void uir_store(int p, const char *key, double v) {
    char k[UIR_KEY + 4], t[40]; snprintf(k, sizeof k, "ui:%s", key); snprintf(t, sizeof t, "%.17g", v);
    if (api_p_kv_set(&g_plugins[p].api, k, t)) logf_("ui: %s/%s: could not write the kv store (value %s kept for this run only)", plug_name_of(p), key, t);
}
// add or update a row; returns its index (-1 = full / the key is another kind of row). *fresh = 1 when it is new. g_lock held.
static int uir_upsert_locked(int p, int kind, const char *key, const char *label, int *fresh) {
    int i = uir_find_locked(p, key); *fresh = 0;
    if (i >= 0) { if (g_uir[i].kind != kind) return -1; }
    else { if (g_uir_n >= UIR_MAX) return -1; i = g_uir_n++; memset(&g_uir[i], 0, sizeof g_uir[i]); g_uir[i].plugin = p; g_uir[i].kind = kind; g_uir[i].bind = -1; snprintf(g_uir[i].key, UIR_KEY, "%s", key); *fresh = 1; }
    if (label) snprintf(g_uir[i].label, UIR_LABEL, "%s", label);
    return i;
}
static void uir_bind(int i) {                                          // toggle / button: an Keybinds-tab action "ui_<key>" (unbound unless the player binds it)
    char id[UIR_KEY + 4], mod[MOD_NAME_MAX], label[UIR_LABEL]; int p;
    EnterCriticalSection(&g_lock); p = g_uir[i].plugin; snprintf(id, sizeof id, "ui_%s", g_uir[i].key); snprintf(label, sizeof label, "%s", g_uir[i].label); LeaveCriticalSection(&g_lock);
    snprintf(mod, sizeof mod, "%s", plug_name_of(p));
    int h = binds_declare(mod, id, label, 0, "always");
    EnterCriticalSection(&g_lock); g_uir[i].bind = h; LeaveCriticalSection(&g_lock);
    if (h < 0) logf_("ui: %s/%s: no Keybinds-tab action (bind_action refused '%s')", mod, g_uir[i].key, id);
}
static int uir_refuse(int p, const char *what, const char *key, const char *why) { logf_("ui: %s %s '%s' refused: %s", plug_name_of(p), what, key ? key : "(null)", why); return 1; }

static int api_p_ui_toggle(const AbsApi *self, const char *key, const char *label, int def, void (*cb)(int on, void *user), void *user) {
    int p = plug_of(self); if (p < 0) return 1;
    if (!uir_key_ok(key)) return uir_refuse(p, "ui_toggle", key, "key must be 1-39 characters A-Z a-z 0-9 _ . -");
    if (!label || !*label) return uir_refuse(p, "ui_toggle", key, "no label");
    double s = 0; int has = uir_stored(p, key, &s), fresh;
    EnterCriticalSection(&g_lock);
    int i = uir_upsert_locked(p, UIR_TOGGLE, key, label, &fresh);
    if (i >= 0) { UiRow *r = &g_uir[i]; r->cb_toggle = cb; r->user = user; if (fresh) { r->v = has ? (s != 0) : (def != 0); r->restore = cb != NULL; } }
    LeaveCriticalSection(&g_lock);
    if (i < 0) return uir_refuse(p, "ui_toggle", key, g_uir_n >= UIR_MAX ? "64 rows in all" : "the key is another kind of row");
    uir_bind(i);
    if (fresh) logf_("ui: %s toggle '%s' (%s) = %s%s", plug_name_of(p), key, label, g_uir[i].v ? "on" : "off", has ? " (stored)" : " (default)");
    return 0;
}
static int api_p_ui_number(const AbsApi *self, const char *key, const char *label, double def, double min, double max, double step, void (*cb)(double v, void *user), void *user) {
    int p = plug_of(self); if (p < 0) return 1;
    if (!uir_key_ok(key)) return uir_refuse(p, "ui_number", key, "key must be 1-39 characters A-Z a-z 0-9 _ . -");
    if (!label || !*label) return uir_refuse(p, "ui_number", key, "no label");
    if (!(min < max) || !(step > 0) || step > max - min || !(def == def)) return uir_refuse(p, "ui_number", key, "needs min < max and 0 < step <= max - min");
    double s = 0; int has = uir_stored(p, key, &s), fresh;
    EnterCriticalSection(&g_lock);
    int i = uir_upsert_locked(p, UIR_NUMBER, key, label, &fresh);
    if (i >= 0) {
        UiRow *r = &g_uir[i]; r->min = min; r->max = max; r->step = step; r->cb_number = cb; r->user = user;
        if (fresh) { r->v = uir_clamp(has ? s : def, min, max, step); r->restore = cb != NULL; } else r->v = uir_clamp(r->v, min, max, step);
    }
    LeaveCriticalSection(&g_lock);
    if (i < 0) return uir_refuse(p, "ui_number", key, g_uir_n >= UIR_MAX ? "64 rows in all" : "the key is another kind of row");
    if (fresh) { char a[32]; uir_fmt(g_uir[i].v, a, sizeof a); logf_("ui: %s number '%s' (%s) = %s%s", plug_name_of(p), key, label, a, has ? " (stored)" : " (default)"); }
    return 0;
}
static int api_p_ui_button(const AbsApi *self, const char *key, const char *label, void (*cb)(void *user), void *user) {
    int p = plug_of(self); if (p < 0) return 1;
    if (!uir_key_ok(key)) return uir_refuse(p, "ui_button", key, "key must be 1-39 characters A-Z a-z 0-9 _ . -");
    if (!label || !*label || !cb) return uir_refuse(p, "ui_button", key, "no label or no callback");
    int fresh; EnterCriticalSection(&g_lock);
    int i = uir_upsert_locked(p, UIR_BUTTON, key, label, &fresh); if (i >= 0) { g_uir[i].cb_button = cb; g_uir[i].user = user; }
    LeaveCriticalSection(&g_lock);
    if (i < 0) return uir_refuse(p, "ui_button", key, g_uir_n >= UIR_MAX ? "64 rows in all" : "the key is another kind of row");
    uir_bind(i);
    if (fresh) logf_("ui: %s button '%s' (%s)", plug_name_of(p), key, label);
    return 0;
}
static int api_p_ui_hint(const AbsApi *self, const char *key, const char *text) {
    int p = plug_of(self); if (p < 0) return 1;
    if (!uir_key_ok(key)) return uir_refuse(p, "ui_hint", key, "key must be 1-39 characters A-Z a-z 0-9 _ . -");
    if (!text) return uir_refuse(p, "ui_hint", key, "no text");
    int fresh; EnterCriticalSection(&g_lock);
    int i = uir_upsert_locked(p, UIR_HINT, key, NULL, &fresh); if (i >= 0) snprintf(g_uir[i].text, UIR_TEXT, "%s", text);
    LeaveCriticalSection(&g_lock);
    if (i < 0) return uir_refuse(p, "ui_hint", key, g_uir_n >= UIR_MAX ? "64 rows in all" : "the key is another kind of row");
    if (fresh) logf_("ui: %s hint '%s'", plug_name_of(p), key);            // updates are not logged (a plugin may refresh a status line often)
    return 0;
}
static int api_p_ui_set(const AbsApi *self, const char *key, double value) {
    int p = plug_of(self); if (p < 0 || !key) return 1;
    EnterCriticalSection(&g_lock);
    int i = uir_find_locked(p, key), kind = i >= 0 ? g_uir[i].kind : 0; double v = 0, old = 0;
    if (kind == UIR_TOGGLE) { old = g_uir[i].v; v = g_uir[i].v = value != 0; }
    else if (kind == UIR_NUMBER) { old = g_uir[i].v; v = g_uir[i].v = uir_clamp(value, g_uir[i].min, g_uir[i].max, g_uir[i].step); }
    LeaveCriticalSection(&g_lock);
    if (kind != UIR_TOGGLE && kind != UIR_NUMBER) { logf_("ui: %s ui_set '%s' refused: no toggle / number row of that key", plug_name_of(p), key); return 1; }
    if (v != old) { char a[32]; uir_fmt(v, a, sizeof a); uir_store(p, key, v); logf_("ui: %s/%s = %s (set by the plugin)", plug_name_of(p), key, a); }
    return 0;
}

// the player chose row i (Mods page select / click, a bound key) or set a number (ImGui slider: set = 1, value). Game thread.
static void uir_choose(int i, int set, double value, const char *how) {
    EnterCriticalSection(&g_lock);
    if (i < 0 || i >= g_uir_n) { LeaveCriticalSection(&g_lock); return; }
    UiRow r = g_uir[i];
    if (r.kind == UIR_NUMBER && set) r.v = uir_clamp(value, r.min, r.max, r.step); else r.v = uir_next(&g_uir[i]);
    if (r.kind == UIR_TOGGLE || r.kind == UIR_NUMBER) { g_uir[i].v = r.v; g_uir[i].restore = 0; }      // the change carries the current value: no separate first call
    LeaveCriticalSection(&g_lock);
    const char *mod = plug_name_of(r.plugin); char a[32]; uir_fmt(r.v, a, sizeof a);
    if (r.kind == UIR_HINT) { logf_("ui: %s/%s is a hint (nothing to do)", mod, r.key); return; }
    if (r.kind != UIR_BUTTON) uir_store(r.plugin, r.key, r.v);
    if (!plug_guard(r.plugin)) { logf_("ui: %s/%s = %s (%s) - not passed on: the plugin is switched off (3 faults)", mod, r.key, a, how); return; }
    logf_("ui: %s/%s %s (%s)", mod, r.key, r.kind == UIR_BUTTON ? "pressed" : a, how);
    __try {
        if (r.kind == UIR_TOGGLE && r.cb_toggle) r.cb_toggle(r.v != 0, r.user);
        else if (r.kind == UIR_NUMBER && r.cb_number) r.cb_number(r.v, r.user);
        else if (r.kind == UIR_BUTTON && r.cb_button) r.cb_button(r.user);
    } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(r.plugin, "ui row callback", GetExceptionCode()); }
}
// every game tick: the first callback of each new toggle / number (stored or default value), then bound keys of toggles / buttons
static void uir_tick(void) {
    for (int i = 0; i < g_uir_n; i++) {
        EnterCriticalSection(&g_lock); UiRow r = g_uir[i]; int go = r.restore && r.plugin < g_plugins_n && g_plugins[r.plugin].loaded; if (go) g_uir[i].restore = 0; LeaveCriticalSection(&g_lock);
        if (go && plug_guard(r.plugin)) {
            char a[32]; uir_fmt(r.v, a, sizeof a); logf_("ui: %s/%s first call = %s", plug_name_of(r.plugin), r.key, a);
            __try { if (r.kind == UIR_TOGGLE) r.cb_toggle(r.v != 0, r.user); else if (r.kind == UIR_NUMBER) r.cb_number(r.v, r.user); }
            __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(r.plugin, "ui row callback (first call)", GetExceptionCode()); }
        }
        if (r.bind >= 0 && (r.kind == UIR_TOGGLE || r.kind == UIR_BUTTON)) {
            const AbsApi *a = &g_plugins[r.plugin].api; int pressed = 0;
            if (api_p_bind_state(a, r.bind, &pressed) >= 0 && pressed && api_p_input_owner(a) == 0) uir_choose(i, 0, 0, "bound key");
        }
    }
}
LN_TICK(0850, uirows) { uir_tick(); }

// ----- for modsui.inc: the rows as section items of their plugin
static char g_uir_line[UIR_MAX][UIR_TEXT + 64];
static int uir_lines(void) {                                            // refreshes the row texts; returns the row count
    EnterCriticalSection(&g_lock); int n = g_uir_n; for (int i = 0; i < n; i++) uir_line(&g_uir[i], g_uir_line[i], sizeof g_uir_line[i]); LeaveCriticalSection(&g_lock);
    return n;
}
static int uir_plugin(int i) { return i >= 0 && i < g_uir_n ? g_uir[i].plugin : -1; }
static int uir_kind(int i) { return i >= 0 && i < g_uir_n ? g_uir[i].kind : 0; }
static void uir_range(int i, double out[5]) { EnterCriticalSection(&g_lock); if (i >= 0 && i < g_uir_n) { out[0] = g_uir[i].v; out[1] = g_uir[i].min; out[2] = g_uir[i].max; out[3] = g_uir[i].step; out[4] = i; } LeaveCriticalSection(&g_lock); }

LN_API(uirows) { a->ui_toggle = api_p_ui_toggle; a->ui_number = api_p_ui_number; a->ui_button = api_p_ui_button; a->ui_hint = api_p_ui_hint; a->ui_set = api_p_ui_set; }

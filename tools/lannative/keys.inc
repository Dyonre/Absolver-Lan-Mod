// ---------------------------------------------------------------------------------------------------------------------
// keys.inc - key ownership on the F2 Mods page and the API 2.23 key / focus calls (registry core: keyreg.h + keys_note in lannative.c).
//   F2 rows "Keys: N in use, M clash(es)" (Enter / click shows or hides the list; open by itself when there is a clash), one row per key.
//   AbsApi::declare_key  - a plugin that polls a key itself (GetAsyncKeyState) names it; on_key registers its key by itself (secore.inc).
//   AbsApi::input_owner  - who has the keyboard / pad now (the game, a loader menu, another mod's window, your window).
static char g_keyrows[96][128]; static int g_keyrows_n, g_keys_shown = -1;          // -1 = not decided yet (shown when there is a clash)
static char g_keyhead[128];
static int keys_rows_count(void) {
    EnterCriticalSection(&g_lock);
    int clashes = keyreg_clash_count(&g_keys), n = keyreg_lines(&g_keys, g_keyrows, 96), used = 0;
    for (int vk = 1; vk < 255; vk++) for (int i = 0; i < g_keys.n; i++) if (g_keys.u[i].vk == vk) { used++; break; }
    LeaveCriticalSection(&g_lock);
    if (g_keys_shown < 0) g_keys_shown = clashes > 0;
    snprintf(g_keyhead, sizeof g_keyhead, "Keys: %d in use, %d clash(es)  (%s)", used, clashes, g_keys_shown ? "Enter / click: hide" : "Enter / click: show which mod uses which key");
    g_keyrows_n = n;
    return 1 + (g_keys_shown ? n : 0);
}
static const char *keys_rows_label(int i) { return i == 0 ? g_keyhead : i - 1 < g_keyrows_n ? g_keyrows[i - 1] : ""; }
static void keys_rows_action(int i) { if (i == 0) { g_keys_shown = !g_keys_shown; logf_("mods page: key list %s", g_keys_shown ? "shown" : "hidden"); } }
LN_MODS_ROWS(0300, keys, keys_rows_count, keys_rows_label, keys_rows_action)

static int api_p_declare_key(const AbsApi *self, int vk, const char *what) {
    int p = plug_of(self); if (p < 0 || vk < 1 || vk > 254) return -1;
    char w[64]; snprintf(w, sizeof w, "%s", what && what[0] ? what : "declared");
    return keys_note(vk, KR_DECLARED, plug_name_of(p), w);
}
static int api_p_input_owner(const AbsApi *self) {
    int p = plug_of(self); if (p < 0) return 0;
#ifdef LN_OVERLAY
    if (ov_state() == OV_READY) { int fo = ov_focus_owner(); if (fo >= 0) return fo == p ? 3 : 2; }
#endif
    return g_menu.open ? 1 : 0;
}
LN_API(keys) { a->declare_key = api_p_declare_key; a->input_owner = api_p_input_owner; }

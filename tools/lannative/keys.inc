// ---------------------------------------------------------------------------------------------------------------------
// keys.inc - key binding and key ownership on the F2 Mods page, and the key / focus / bind calls of the API (registry cores: bindreg.h, keyreg.h + keys_note in
// lannative.c).
//   Bindable actions (API 2.24 bind_action, and every on_key registration): the player rebinds them on the F2 "Keys" rows - choose the action, press the new
//   key, mouse button or pad button (Esc cancels, Delete unbinds, Backspace = back to the default). Saved in LanNative\binds.txt (only what differs from the
//   defaults). Conflicts (same input, overlapping context) are WARNED on the rows and in the log, never refused.
//   Keys a mod reads itself (declare_key) and the loader's / game's / Steam's keys are listed below them as fixed keys.
//   AbsApi::input_owner - who has the keyboard / pad now (the game, a loader menu, another mod's window, your window).
// Test switch: -LNPadSim on the command line makes NumPad 0-9 act as pad buttons for binds (testing without a pad): 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back,
// 7 Start, 8 LT, 9 RT.
#include "bindreg.h"
static BindReg g_binds; static int g_binds_loaded;
static unsigned char g_bind_was[1024];                           // per action: down at the last bind_state call (edge); ponytail: actions past 1024 get no edges
static int binds_capturing(void);

// ----- inputs: keyboard and mouse through GetAsyncKeyState, the pad through XInput's own export (as the menus read it), -LNPadSim
static int g_padsim = -1;
typedef DWORD (WINAPI *BindXiGet_t)(DWORD, void *);
static unsigned binds_pad_bits(void) {                          // XInput bits 0..15, bit 16 LT, bit 17 RT (triggers past half way)
    if (g_padsim < 0) { g_padsim = cmdline_has_flag(GetCommandLineW(), L"lnpadsim") ? 1 : 0; if (g_padsim) logf_("binds: -LNPadSim: NumPad 0-9 act as pad buttons (0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 8 LT, 9 RT)"); }
    unsigned bits = 0;
    if (g_padsim) { static const int bit[10] = { 12, 13, 14, 15, 8, 9, 5, 4, 16, 17 }; for (int i = 0; i < 10; i++) if (LN_BG_INPUT() ? LN_BG_KEY(VK_NUMPAD0 + i) : (GetAsyncKeyState(VK_NUMPAD0 + i) & 0x8000)) bits |= 1u << bit[i]; }
    if (LN_BG_INPUT()) return bits;                                // -LNBgInput: never the user's real pad (only the NumPad simulation above, from the window's messages)
    static BindXiGet_t get; static int tried, user = -1; static ULONGLONG rescan_at;
    if (!tried) {
        tried = 1; static const wchar_t *dll[] = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" };
        for (int k = 0; k < 3 && !get; k++) { HMODULE m = GetModuleHandleW(dll[k]); if (!m) m = LoadLibraryW(dll[k]); if (m) get = (BindXiGet_t)(void *)GetProcAddress(m, "XInputGetState"); }
        logf_("binds: pad buttons %s", get ? "read through XInput" : "unavailable (no XInputGetState)");
    }
    if (!get) return bits;
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
    if (user < 0) { ULONGLONG now = GetTickCount64(); if (now < rescan_at) return bits; rescan_at = now + 2000; for (int u = 0; u < 4 && user < 0; u++) if (get((DWORD)u, &st) == ERROR_SUCCESS) user = u; if (user < 0) return bits; }
    if (get((DWORD)user, &st) != ERROR_SUCCESS) { user = -1; return bits; }
    return bits | st.buttons | (st.lt > 128 ? 1u << 16 : 0) | (st.rt > 128 ? 1u << 17 : 0);
}
static int binds_numpad_is_pad(int vk) { return g_padsim > 0 && vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9; }
static int binds_down(int code, unsigned pad) {                // is this input held now (game window focus is the caller's business)
    if (code <= 0) return 0;
    if (code >= BIND_PAD) return (pad >> (code - BIND_PAD)) & 1;
    if (LN_BG_INPUT()) return code > 6 && !binds_numpad_is_pad(code) && LN_BG_KEY(code);   // -LNBgInput: keys from the window's own messages; never the real mouse (codes 1-6) or keyboard
    return binds_numpad_is_pad(code) ? 0 : (GetAsyncKeyState(code) & 0x8000) != 0;
}
static int binds_first_down(unsigned pad) {                     // the first input held now: a key / mouse button, else a pad button; 0 = none
    for (int vk = 1; vk < 255; vk++) {
        if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || binds_numpad_is_pad(vk)) continue;   // the generic Shift / Ctrl / Alt come first
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    for (int b = 0; b < 18; b++) if ((pad >> b) & 1) return BIND_PAD + b;
    return 0;
}

// ----- the stored file LanNative\binds.txt (g_lock held by the callers)
static void binds_load_locked(void) {
    if (g_binds_loaded) return; g_binds_loaded = 1;
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sbinds.txt", g_dir);
    FILE *f = _wfopen(p, L"rb"); if (!f) { logf_("binds: no LanNative\\binds.txt - every action uses its default input"); return; }
    char *t = (char *)calloc(1, 65537); size_t n = t ? fread(t, 1, 65536, f) : 0; int more = n == 65536 && fgetc(f) != EOF; fclose(f); if (!t) return; t[n] = 0;
    if (more) logf_("binds: LanNative\binds.txt is over 64 KB - only its first 64 KB are read (a cut last line is ignored)");
    int bad = 0, used = bindreg_load(&g_binds, t, &bad); free(t);
    logf_("binds: LanNative\\binds.txt: %d bind(s) read%s", used, bad ? " - some lines were not understood and are ignored (format: docs/MODS.md)" : "");
    if (bad) logf_("binds: %d line(s) of binds.txt ignored (format: Mod/action = K | F8 | Mouse 4 | Pad A | none)", bad);
}
static void binds_save_locked(void) {
    char *t = (char *)malloc(65536); if (!t) return;
    int len = bindreg_save(&g_binds, t, 65536);
    if (len < 0) { logf_("binds: binds.txt would be over 64 KB - NOT saved"); free(t); return; }
    wchar_t p[MAX_PATH], tmp[MAX_PATH]; swprintf(p, MAX_PATH, L"%sbinds.txt", g_dir); swprintf(tmp, MAX_PATH, L"%sbinds.txt.tmp", g_dir);
    FILE *f = _wfopen(tmp, L"wb"); int ok = f && fwrite(t, 1, (size_t)len, f) == (size_t)len; if (f) ok = fclose(f) == 0 && ok;
    ok = ok && MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) { DeleteFileW(tmp); logf_("binds: saving LanNative\\binds.txt FAILED (error %lu) - the bind holds for this run only", GetLastError()); }
    else logf_("binds: saved LanNative\\binds.txt (%d stored bind(s))", g_binds.no);
    free(t);
}
// a fixed key (keyreg: loader, game, Steam, a mod that reads it itself) on the same keyboard / mouse input: its text in out, 1 = there is one
static int binds_fixed_clash_locked(int code, const char *mod, char *out, size_t cap) {
    if (code <= 0 || code > 254) return 0;
    for (int i = 0; i < g_keys.n; i++) if (g_keys.u[i].vk == code && _stricmp(g_keys.u[i].who, mod)) { snprintf(out, cap, "%s: %s", g_keys.u[i].who, g_keys.u[i].what); return 1; }
    return 0;
}
// one line in the log for a new input of action i, with its conflict warnings
static void binds_log_conflicts_locked(int i, const char *why) {
    int who[8], n = bindreg_conflicts(&g_binds, i, who, 8); char nm[24], fx[128]; BindAct *a = &g_binds.a[i];
    bind_code_name(a->code, nm, sizeof nm);
    for (int k = 0; k < n && k < 8; k++) logf_("binds: WARNING (%s): %s is bound to %s: %s [%s] and to %s: %s [%s] - both stay bound; both act while both contexts are active", why, nm, a->mod, a->label, a->ctx, g_binds.a[who[k]].mod, g_binds.a[who[k]].label, g_binds.a[who[k]].ctx);
    if (binds_fixed_clash_locked(a->code, a->mod, fx, sizeof fx)) logf_("binds: WARNING (%s): %s for %s: %s is also a fixed key (%s)", why, nm, a->mod, a->label, fx);
}
// declares an action of mod (loader side of bind_action and of on_key). Returns the handle, -1 = refused.
static int binds_declare(const char *mod, const char *id, const char *label, int def, const char *ctx) {
    EnterCriticalSection(&g_lock); binds_load_locked();
    int had = bindreg_find(&g_binds, mod, id), i = bindreg_add(&g_binds, mod, id, label, def, ctx, 0);
    if (i >= 0 && had < 0) {
        char nm[24], dn[24]; BindAct *a = &g_binds.a[i]; bind_code_name(a->code, nm, sizeof nm); bind_code_name(a->def, dn, sizeof dn);
        logf_("binds: %s/%s '%s' [%s] = %s%s%s%s", mod, id, a->label, a->ctx, nm, a->code != a->def ? " (player's bind from binds.txt; default " : "", a->code != a->def ? dn : "", a->code != a->def ? ")" : "");
        binds_log_conflicts_locked(i, "declared");
#ifdef LN_OVERLAY
        if (a->code >= VK_F1 && a->code <= VK_F24) ov_keep_key(a->code, 1);   // a mod's F key: the game does not get it as well (F11 is its fullscreen toggle)
#endif
    }
    LeaveCriticalSection(&g_lock);
    return i;
}

static int binds_code(int h) { EnterCriticalSection(&g_lock); int c = h >= 0 && h < g_binds.n ? g_binds.a[h].code : -1; LeaveCriticalSection(&g_lock); return c; }

// ----- capture: choose a row, press the new input (states: arm = wait until nothing is held, listen, release = wait again so the menu never sees it)
static volatile LONG g_cap_state; static int g_cap_act = -1; static ULONGLONG g_cap_t;
static int binds_capturing(void) { return g_cap_state != 0; }
static void binds_capture_start(int i) {
    EnterCriticalSection(&g_lock); int ok = i >= 0 && i < g_binds.n; char line[160] = "";
    if (ok) snprintf(line, sizeof line, "%s: %s", g_binds.a[i].mod, g_binds.a[i].label);
    LeaveCriticalSection(&g_lock); if (!ok) return;
    g_cap_act = i; g_cap_t = GetTickCount64(); InterlockedExchange(&g_cap_state, 1);
    logf_("binds: capture for %s - press a key, mouse button or pad button (Esc cancels, Delete unbinds, Backspace = default; 10 s)", line);
}
static void binds_capture_tick(int focus) {
    if (!g_cap_state) return;
    unsigned pad = binds_pad_bits(); ULONGLONG now = GetTickCount64();
    int held = binds_first_down(pad);
    if (g_cap_state == 1) { if (!held) { InterlockedExchange(&g_cap_state, 2); g_cap_t = now; } return; }
    if (g_cap_state == 3) { if (!held) { InterlockedExchange(&g_cap_state, 0); g_cap_act = -1; } return; }
    if (now - g_cap_t > 10000) { logf_("binds: capture timed out (10 s) - nothing changed"); InterlockedExchange(&g_cap_state, 3); return; }
    if (!focus || !held) return;                                  // (the F2 rebind capture is a player feature: it reads the real keyboard, also under -LNBgInput)
    EnterCriticalSection(&g_lock);
    int i = g_cap_act; BindAct *a = i >= 0 && i < g_binds.n ? &g_binds.a[i] : NULL; char nm[24], was[24];
    if (!a) { LeaveCriticalSection(&g_lock); InterlockedExchange(&g_cap_state, 3); return; }
    int code = held == VK_ESCAPE ? -1 : held == VK_DELETE ? 0 : held == VK_BACK ? a->def : held;
    bind_code_name(a->code, was, sizeof was);
    if (code < 0) logf_("binds: capture cancelled (Esc) - %s: %s stays %s", a->mod, a->label, was);
    else {
        int rc = bindreg_set(&g_binds, i, code); bind_code_name(code, nm, sizeof nm);
        logf_("binds: %s: %s [%s]: %s -> %s%s", a->mod, a->label, a->ctx, was, nm, rc == 0 ? " (unchanged)" : held == VK_DELETE ? " (unbound)" : held == VK_BACK ? " (default)" : "");
        if (rc > 0) { binds_save_locked(); binds_log_conflicts_locked(i, "rebound");
#ifdef LN_OVERLAY
            if (code >= VK_F1 && code <= VK_F24) ov_keep_key(code, 1);
#endif
        }
    }
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_cap_state, 3);
}
LN_TICK(0690, binds) { binds_capture_tick(g_ln_focus); }       // before the menu (0700): the menu ignores every key while a capture runs (menu_tick)

// ----- F2 rows
static char g_keyrows[160][128]; static int g_keyrows_n, g_keys_shown = -1;          // -1 = not decided yet (shown when there is a conflict)
static int g_keyrow_act[160];                                                       // row -> action index, -1 = not an action
static char g_keyhead[128];
static int keys_rows_count(void) {
    EnterCriticalSection(&g_lock); binds_load_locked();
    int warn = 0, fixed_n = 0, rows = 0; char fx[128];
    for (int i = 0; i < g_binds.n; i++) if (bindreg_conflicts(&g_binds, i, NULL, 0) || binds_fixed_clash_locked(g_binds.a[i].code, g_binds.a[i].mod, fx, sizeof fx)) warn++;
    char kl[96][128]; int nk = keyreg_lines(&g_keys, kl, 96); fixed_n = nk;
    if (g_keys_shown < 0) g_keys_shown = 1;                                         // shown by default (the binds live here); Enter on the header hides it
#ifdef LN_OVERLAY
    if (ov_text_hidden()) {                                                         // the overlay's Keybinds tab - grouped under each mod's name, columns label / when / key / warning
        // row markers (modsui_snapshot): \x01 group header, \x02 grey text, '\t' between columns. No header row: the tab is the list.
        snprintf(g_keyrows[rows], 128, "\x02%d bindable, %d warning(s). Click a row (or Enter), then press a key, mouse or pad button.", g_binds.n, warn); g_keyrow_act[rows++] = -1;
        snprintf(g_keyrows[rows], 128, "\x02" "Esc cancels, Del unbinds, Backspace = default. A key used twice is only warned (!)."); g_keyrow_act[rows++] = -1;
        int done[160] = { 0 };
        for (int i = 0; i < g_binds.n && i < 160 && rows < 150; i++) {             // mods in order of their first action; each mod's actions in declaration order
            if (done[i]) continue;
            snprintf(g_keyrows[rows], 128, "\x01%s", g_binds.a[i].mod); g_keyrow_act[rows++] = -1;
            for (int j = i; j < g_binds.n && j < 160 && rows < 155; j++) {
                if (done[j] || _stricmp(g_binds.a[j].mod, g_binds.a[i].mod)) continue;
                done[j] = 1; BindAct *a = &g_binds.a[j]; char nm[24], dn[24], wtxt[64] = ""; bind_code_name(a->code, nm, sizeof nm); bind_code_name(a->def, dn, sizeof dn);
                int who[2], nc = bindreg_conflicts(&g_binds, j, who, 2);
                if (nc) snprintf(wtxt, sizeof wtxt, "! also %s: %.24s%s", g_binds.a[who[0]].mod, g_binds.a[who[0]].label, nc > 1 ? " +more" : "");
                else if (binds_fixed_clash_locked(a->code, a->mod, fx, sizeof fx)) snprintf(wtxt, sizeof wtxt, "! also %.40s", fx);
                if (g_cap_state && g_cap_act == j) snprintf(g_keyrows[rows], 128, "%.44s\t%s\tpress a key ...\tEsc cancels", a->label, a->ctx);
                else snprintf(g_keyrows[rows], 128, "%.44s\t%s\t%s%s%s%s\t%.40s", a->label, a->ctx, nm, a->code != a->def ? " (default " : "", a->code != a->def ? dn : "", a->code != a->def ? ")" : "", wtxt);
                g_keyrow_act[rows++] = j;
            }
        }
        if (nk && rows < 158) { snprintf(g_keyrows[rows], 128, "\x01" "Fixed keys (the loader, the game, Steam, mods that read a key themselves)"); g_keyrow_act[rows++] = -1; }
        for (int k = 0; k < nk && rows < 160; k++) { snprintf(g_keyrows[rows], 128, "\x02%s", kl[k]); g_keyrow_act[rows++] = -1; }
        LeaveCriticalSection(&g_lock);
        g_keyrows_n = rows; g_keyhead[0] = 0;
        return rows;                                                                // no header row: row i = g_keyrows[i] (keys_rows_label / _action check g_keyhead)
    }
#endif
    if (g_keys_shown) {
        // fixed keys first, then the bindable actions (in declaration order)
        if (nk) { snprintf(g_keyrows[rows], 128, "  Fixed keys (the loader, the game, Steam, or a mod that reads the key itself):"); g_keyrow_act[rows++] = -1; }
        for (int k = 0; k < nk && rows < 100; k++) { snprintf(g_keyrows[rows], 128, "    %s", kl[k]); g_keyrow_act[rows++] = -1; }
        snprintf(g_keyrows[rows], 128, "  Rebind: choose a row, press key/mouse/pad (Esc cancel, Del unbind, Bksp default)"); g_keyrow_act[rows++] = -1;
        for (int i = 0; i < g_binds.n && rows < 160; i++) {
            BindAct *a = &g_binds.a[i]; char nm[24], dn[24], wtxt[64] = ""; bind_code_name(a->code, nm, sizeof nm); bind_code_name(a->def, dn, sizeof dn);
            int who[2], nc = bindreg_conflicts(&g_binds, i, who, 2);
            if (nc) snprintf(wtxt, sizeof wtxt, "  ! also %s: %.24s%s", g_binds.a[who[0]].mod, g_binds.a[who[0]].label, nc > 1 ? " +more" : "");
            else if (binds_fixed_clash_locked(a->code, a->mod, fx, sizeof fx)) snprintf(wtxt, sizeof wtxt, "  ! also %.40s", fx);
            if (g_cap_state && g_cap_act == i) snprintf(g_keyrows[rows], 128, "  %s: %.32s = ... press a key, mouse or pad button (Esc cancels)", a->mod, a->label);
            else snprintf(g_keyrows[rows], 128, "  %s: %.32s [%s] = %s%s%s%s%s", a->mod, a->label, a->ctx, nm, a->code != a->def ? " (default " : "", a->code != a->def ? dn : "", a->code != a->def ? ")" : "", wtxt);
            g_keyrow_act[rows++] = i;
        }
    }
    int nb = g_binds.n;
    LeaveCriticalSection(&g_lock);
    snprintf(g_keyhead, sizeof g_keyhead, "Keys: %d bindable, %d warning(s), %d fixed  (%s)", nb, warn, fixed_n, g_keys_shown ? "Enter / click: hide" : "Enter / click: show and rebind");
    g_keyrows_n = rows;
    return 1 + rows;
}
static const char *keys_rows_label(int i) { if (!g_keyhead[0]) return i >= 0 && i < g_keyrows_n ? g_keyrows[i] : ""; return i == 0 ? g_keyhead : i - 1 < g_keyrows_n ? g_keyrows[i - 1] : ""; }
static void keys_rows_action(int i) {
    if (!g_keyhead[0]) { if (i >= 0 && i < g_keyrows_n && g_keyrow_act[i] >= 0 && !g_cap_state) binds_capture_start(g_keyrow_act[i]); return; }   // tab mode (no header row)
    if (i == 0) { g_keys_shown = !g_keys_shown; logf_("mods page: key list %s", g_keys_shown ? "shown" : "hidden"); return; }
    if (i - 1 < g_keyrows_n && g_keyrow_act[i - 1] >= 0 && !g_cap_state) binds_capture_start(g_keyrow_act[i - 1]);
}
LN_MODS_ROWS(0300, keys, keys_rows_count, keys_rows_label, keys_rows_action)

// ----- API
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
static int api_p_bind_action(const AbsApi *self, const char *id, const char *label, const char *default_input, const char *context) {
    int p = plug_of(self); if (p < 0 || !id || !id[0]) return -1;
    int def = bind_code_parse(default_input ? default_input : "");
    if (def < 0) { logf_("plugin %s: bind_action('%s') refused - default input '%s' is not a key, mouse or pad button name", plug_name_of(p), id, default_input); return -1; }
    int h = binds_declare(plug_name_of(p), id, label, def, context);
    if (h < 0) logf_("plugin %s: bind_action('%s') refused - bad id (no '/' or '=') or out of memory", plug_name_of(p), id);
    return h;
}
static int binds_owned(int p, int h) {                          // g_lock held
    return p >= 0 && h >= 0 && h < g_binds.n && !_stricmp(g_binds.a[h].mod, plug_name_of(p));
}
static int api_p_bind_state(const AbsApi *self, int handle, int *pressed) {
    int p = plug_of(self); if (pressed) *pressed = 0;
    EnterCriticalSection(&g_lock); int ok = binds_owned(p, handle), code = ok ? g_binds.a[handle].code : 0; LeaveCriticalSection(&g_lock);
    if (!ok) return -1;
    int d = (g_ln_focus || LN_BG_INPUT()) && !binds_capturing() && binds_down(code, code >= BIND_PAD ? binds_pad_bits() : 0);
    if (handle < (int)sizeof g_bind_was) { if (pressed) *pressed = d && !g_bind_was[handle]; g_bind_was[handle] = (unsigned char)d; }
    return d;
}
static int api_p_bind_name(const AbsApi *self, int handle, char *out, int cap) {
    int p = plug_of(self);
    EnterCriticalSection(&g_lock); int ok = binds_owned(p, handle), code = ok ? g_binds.a[handle].code : -1; LeaveCriticalSection(&g_lock);
    if (out && cap > 0) { if (ok) bind_code_name(code, out, (size_t)cap); else out[0] = 0; }
    return code;
}
LN_API(keys) { a->declare_key = api_p_declare_key; a->input_owner = api_p_input_owner; a->bind_action = api_p_bind_action; a->bind_state = api_p_bind_state; a->bind_name = api_p_bind_name; }

// ---------------------------------------------------------------------------------------------------------------------
// profiles.inc - mod profiles on the F2 Mods page (logic: modprofile.h).
//   Mods\profiles\<name>.txt = a named copy of the on/off + order set (loadorder.txt's format). Rows: which profile the current set is, "Switch to '<name>'"
//   per profile, "Save the current set as '<name>'" for the suggested names that are free and for the next "Profile N". Nothing is created by default.
//   Switch = writes the profile's set into Mods\loadorder.txt (the old file kept as loadorder.txt.bak, as for every Mods page change); the page then lists
//   the change as pending and offers Restart: it takes effect at the next start. Safe mode: no writes. Mods\profiles is never scanned as a mod (modglue.inc).
//   Profile box at the top of the overlay page (OVC_PROFILE_SAVE): a typed name -> "Save as" (new) or "Update" (overwrites that profile, old file kept as
//   <name>.txt.bak). The box starts with the current profile, else the last one used this session (switched to, saved, or the set at the first scan).
#include "modprofile.h"
static char g_pf_names[32][PROFILE_NAME_MAX + 1]; static int g_pf_n, g_pf_current = -1;
static char g_pf_rows[48][128]; static int g_pf_act[48], g_pf_rows_n;               // g_pf_act: >= 0 switch to that profile, -1 none, -100 - k save as the name in g_pf_save[k]
static char g_pf_save[5][PROFILE_NAME_MAX + 1]; static char g_pf_msg[160];
static char g_pf_lastname[PROFILE_NAME_MAX + 1]; static unsigned g_pf_serial;                 // the profile box's starting name; serial++ = the box reloads it

static void pf_path(const char *name, wchar_t *out) { wchar_t wn[64]; u8_to_w(name, wn, 64); swprintf(out, MAX_PATH, L"%ls\\profiles\\%ls.txt", g_mods_dir, wn); }
static int pf_read(const char *name, ModList *l) {
    wchar_t p[MAX_PATH]; pf_path(name, p); char *t = mods_read_text(p, 65536); if (!t) return 0;
    modlist_parse(l, t); free(t); return 1;
}
static void pf_scan(void) {                                     // profile names (sorted) and which one the current set is
    g_pf_n = 0; g_pf_current = -1;
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\profiles\\*.txt", g_mods_dir); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char u[MAX_PATH]; w_to_u8(fd.cFileName, u, sizeof u); size_t n = strlen(u); if (n < 5) continue; u[n - 4] = 0;
            if (profile_name_ok(u) && g_pf_n < 32) snprintf(g_pf_names[g_pf_n++], PROFILE_NAME_MAX + 1, "%s", u);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int i = 1; i < g_pf_n; i++) for (int j = i; j > 0 && _stricmp(g_pf_names[j - 1], g_pf_names[j]) > 0; j--) { char t[PROFILE_NAME_MAX + 1]; strcpy(t, g_pf_names[j]); strcpy(g_pf_names[j], g_pf_names[j - 1]); strcpy(g_pf_names[j - 1], t); }
    static ModList p, out;                                       // static: a ModList is 25 KB
    for (int i = 0; i < g_pf_n && g_pf_current < 0; i++) if (pf_read(g_pf_names[i], &p)) { int ext = 0; profile_apply(&g_order_now, &p, &out, NULL, &ext); if (ext < g_order_now.count && modlist_same(&out, &g_order_now)) g_pf_current = i; }   // ext == count: it names no installed mod
    if (g_pf_current >= 0 && _stricmp(g_pf_lastname, g_pf_names[g_pf_current])) { snprintf(g_pf_lastname, sizeof g_pf_lastname, "%s", g_pf_names[g_pf_current]); g_pf_serial++; }
}
static int pf_exists(const char *name) { for (int i = 0; i < g_pf_n; i++) if (!_stricmp(g_pf_names[i], name)) return 1; return 0; }
static int pf_rows_count(void) {
    pf_scan(); int r = 0, ns = 0;
    snprintf(g_pf_rows[r], 128, "Profiles (Mods\\profiles): %d saved - the current set is %s%s%s", g_pf_n, g_pf_current >= 0 ? "'" : "", g_pf_current >= 0 ? g_pf_names[g_pf_current] : "not saved as a profile", g_pf_current >= 0 ? "'" : ""); g_pf_act[r++] = -1;
    static ModList p;
    for (int i = 0; i < g_pf_n && r < 40; i++) {
        int on = -1, cnt = -1; if (pf_read(g_pf_names[i], &p)) { on = modlist_on(&p); cnt = p.count; }
        if (i == g_pf_current) snprintf(g_pf_rows[r], 128, "  '%s' (%d of %d on) = the current set", g_pf_names[i], on, cnt);
        else snprintf(g_pf_rows[r], 128, "  Switch to '%s' (%d of %d on; restart to apply)", g_pf_names[i], on, cnt);
        g_pf_act[r++] = i == g_pf_current ? -1 : i;
    }
    for (int k = 0; k < 3; k++) if (!pf_exists(g_profile_suggest[k])) snprintf(g_pf_save[ns++], PROFILE_NAME_MAX + 1, "%s", g_profile_suggest[k]);
    for (int n = 1; n < 1000; n++) { char t[PROFILE_NAME_MAX + 1]; snprintf(t, sizeof t, "Profile %d", n); if (!pf_exists(t)) { snprintf(g_pf_save[ns++], PROFILE_NAME_MAX + 1, "%s", t); break; } }
    for (int k = 0; k < ns && r < 46; k++) { snprintf(g_pf_rows[r], 128, "  Save the current set as '%s'", g_pf_save[k]); g_pf_act[r++] = -100 - k; }
    if (g_pf_msg[0] && r < 48) { snprintf(g_pf_rows[r], 128, "  %s", g_pf_msg); g_pf_act[r++] = -1; }
    g_pf_rows_n = r; return r;
}
static const char *pf_rows_label(int i) { return i >= 0 && i < g_pf_rows_n ? g_pf_rows[i] : ""; }
static void pf_save(const char *name, int overwrite) {               // overwrite: "Update" in the profile box (the old file is kept as <name>.txt.bak)
    if (g_safe_mode) { snprintf(g_pf_msg, sizeof g_pf_msg, "Safe mode: nothing is written"); return; }
    if (!profile_name_ok(name)) { snprintf(g_pf_msg, sizeof g_pf_msg, "'%.40s' is not a usable name: letters, digits, space, - _ (1-40, no space at either end)", name); logf_("profiles: %s", g_pf_msg); return; }
    wchar_t dir[MAX_PATH], p[MAX_PATH], tmp[MAX_PATH], bak[MAX_PATH]; swprintf(dir, MAX_PATH, L"%ls\\profiles", g_mods_dir); CreateDirectoryW(dir, NULL);
    pf_path(name, p); swprintf(tmp, MAX_PATH, L"%ls.tmp", p); swprintf(bak, MAX_PATH, L"%ls.bak", p);
    int had = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    if (had && !overwrite) { snprintf(g_pf_msg, sizeof g_pf_msg, "'%s' exists already - not overwritten (type its name in the box above to update it)", name); return; }
    static char text[MODS_MAX * (MOD_NAME_MAX + 4) + 512]; int n = profile_render(&g_order_now, name, text, sizeof text);
    FILE *f = n > 0 ? _wfopen(tmp, L"wb") : NULL; int ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n; if (f) ok = fclose(f) == 0 && ok;
    if (had) ok = ok && ReplaceFileW(p, tmp, bak, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);       // update: atomic swap, the old profile becomes .bak
    else ok = ok && MoveFileExW(tmp, p, MOVEFILE_WRITE_THROUGH);                              // new: no REPLACE flag, never overwrites a profile
    if (!ok) { DWORD e = GetLastError(); DeleteFileW(tmp); snprintf(g_pf_msg, sizeof g_pf_msg, "%s '%s' FAILED (error %lu)", had ? "updating" : "saving", name, e); logf_("profiles: %s", g_pf_msg); return; }
    snprintf(g_pf_msg, sizeof g_pf_msg, "%s '%s' (%d of %d mods on)%s", had ? "Updated" : "Saved", name, modlist_on(&g_order_now), g_order_now.count, had ? " - old file: .txt.bak" : "");
    logf_("profiles: %s Mods\\profiles\\%s.txt: %d of %d mods on%s", had ? "updated" : "saved", name, modlist_on(&g_order_now), g_order_now.count, had ? " (the old one kept as .txt.bak)" : "");
    snprintf(g_pf_lastname, sizeof g_pf_lastname, "%s", name); g_pf_serial++;
}
static void pf_switch(int i) {
    if (i < 0 || i >= g_pf_n) return;
    const char *name = g_pf_names[i];
    if (g_safe_mode) { snprintf(g_pf_msg, sizeof g_pf_msg, "Safe mode: nothing is written"); return; }
    static ModList p, out, keep; if (!pf_read(name, &p)) { snprintf(g_pf_msg, sizeof g_pf_msg, "could not read profile '%s'", name); logf_("profiles: %s", g_pf_msg); return; }
    int miss = 0, ext = 0; profile_apply(&g_order_now, &p, &out, &miss, &ext);
    if (modlist_same(&out, &g_order_now)) { snprintf(g_pf_msg, sizeof g_pf_msg, "'%s' is the current set already", name); return; }
    keep = g_order_now; g_order_now = out; char msg[200];
    if (mods_ui_write_order(msg, sizeof msg)) { g_order_now = keep; snprintf(g_pf_msg, sizeof g_pf_msg, "switch to '%s' FAILED: %s", name, msg); logf_("profiles: %s", g_pf_msg); return; }
    snprintf(g_pf_msg, sizeof g_pf_msg, "Switched to '%s': loadorder.txt written (old one: loadorder.txt.bak) - restart to apply", name);
    snprintf(g_pf_lastname, sizeof g_pf_lastname, "%s", name); g_pf_serial++;
    char order[600]; int w = 0; for (int k = 0; k < out.count && w < (int)sizeof order - 100; k++) w += snprintf(order + w, sizeof order - (size_t)w, "%s%s%s", k ? ", " : "", out.mods[k].enabled ? "" : "-", out.mods[k].name);
    logf_("profiles: switched to '%s' -> loadorder.txt: %s%s%s (takes effect at the next start)", name, order, miss ? " - mods in the profile that are not installed were left out" : "", ext ? " - mods the profile does not name keep their state, at the end" : "");
    if (miss || ext) logf_("profiles:   %d profile mod(s) not installed, %d installed mod(s) not in the profile", miss, ext);
    { wchar_t bak[MAX_PATH]; swprintf(bak, MAX_PATH, L"%ls\\loadorder.txt.bak", g_mods_dir); logf_("profiles:   loadorder.txt.bak %s", GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES ? "present (the order before this switch)" : "MISSING"); }
}
static void pf_rows_action(int i) {
    if (i < 0 || i >= g_pf_rows_n) return;
    int a = g_pf_act[i];
    if (a >= 0) pf_switch(a); else if (a <= -100 && -100 - a < 5) pf_save(g_pf_save[-100 - a], 0);
    g_mui_at = 0;
}
#ifdef LN_OVERLAY
static void pf_command(const char *name, int overwrite) {             // OVC_PROFILE_SAVE from the profile box (modsui_command)
    char nm[PROFILE_NAME_MAX + 4]; snprintf(nm, sizeof nm, "%s", name ? name : "");
    logf_("profiles: box: %s '%s'", overwrite ? "update" : "save as", nm);
    pf_scan(); pf_save(nm, overwrite && pf_exists(nm)); g_mui_at = 0;
}
static void pf_snapshot(OvSnap *o) {                                   // the names of the page's last refresh (pf_rows_count)
    { static ULONGLONG next; ULONGLONG now = GetTickCount64(); if (mui_tabs_on() && now >= next) { next = now + 1000; pf_scan(); } }   // tab mode: the profile rows are not listed, the bar needs the names (once a second)
    o->pf_current = g_pf_current; snprintf(o->pf_msg, sizeof o->pf_msg, "%s", g_pf_msg);
    o->pf_n = g_pf_n < 32 ? g_pf_n : 32; for (int i = 0; i < o->pf_n; i++) snprintf(o->pf_names[i], sizeof o->pf_names[i], "%s", g_pf_names[i]);
    snprintf(o->pf_default, sizeof o->pf_default, "%s", g_pf_lastname); o->pf_serial = g_pf_serial;
}
#endif
LN_MODS_ROWS(0250, profiles, pf_rows_count, pf_rows_label, pf_rows_action)

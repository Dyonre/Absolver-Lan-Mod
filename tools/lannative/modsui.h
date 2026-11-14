// Text of the F1 menu's "Mods" page (pure formatting, no engine calls).
// The mod loader (modglue.inc) fills one ModInfo per mod folder while it loads; lannative.c adds the live parts (plugin faults, content conflicts, safe mode,
// the last hot reload) and asks modsui_build for the lines. The first `nactions` lines are actions (Reload content, plugin menu items); the rest only informs.
#ifndef LANNATIVE_MODSUI_H
#define LANNATIVE_MODSUI_H
#include <stdio.h>
#include <string.h>
#include "modconf.h"

enum { MI_ON = 0, MI_OFF, MI_SKIPPED, MI_EMPTY };            // loaded / switched off in loadorder.txt / skipped (reason) / nothing to load
enum { MIP_NONE = 0, MIP_ALLOWED, MIP_BLOCKED };             // plugin.dll: none / allowed in Mods\plugins.txt / present but not allowed
typedef struct {
    char name[MOD_NAME_MAX], id[MOD_NAME_MAX], version[24], reason[180];
    int status, paks_ok, paks_total, rows_ops, content_files, plugin, affects_gameplay;
} ModInfo;

#define MUI_LINE 96
#define MUI_WRAP 88            // characters per wrapped line (the menu draws 95 at most)
#define MUI_CCONF 24
#define MUI_PLUG 16
#define MUI_WARN 16
#define MUI_ACTIONS 80
#define MUI_SECS    32

// Plugin menu items as collapsible sections: one header line per plugin,
// sorted by name; the open section (at most one) lists its items right under its header in registration order.
// key[i] = section key of item i (the plugin index), name[i] = its section title, text[i] = the item label without the "Name: " prefix.
// Fills out[] (labels, no ">> ") and ref[] (what each line does) and returns the number of lines.
enum { MUR_HEAD = 1, MUR_ITEM };
typedef struct { int kind, ref; } MuiRef;                    // MUR_HEAD: ref = section key; MUR_ITEM: ref = item index
static int modsui_sections(int n, const int *key, const char *const *name, const char *const *text, int open, char (*out)[MUI_LINE], MuiRef *ref, int max) {
    int sk[MUI_SECS], first[MUI_SECS], cnt[MUI_SECS], ns = 0, lines = 0;
    for (int i = 0; i < n; i++) {
        int s = 0; while (s < ns && sk[s] != key[i]) s++;
        if (s == ns) { if (ns >= MUI_SECS) continue; sk[ns] = key[i]; first[ns] = i; cnt[ns] = 0; ns++; }
        cnt[s]++;
    }
    for (int a = 1; a < ns; a++)                                                  // sections by name (insertion sort, few of them)
        for (int b = a; b > 0 && _stricmp(name[first[b - 1]], name[first[b]]) > 0; b--) {
            int t = sk[b]; sk[b] = sk[b - 1]; sk[b - 1] = t; t = first[b]; first[b] = first[b - 1]; first[b - 1] = t; t = cnt[b]; cnt[b] = cnt[b - 1]; cnt[b - 1] = t;
        }
    for (int s = 0; s < ns && lines < max; s++) {
        int is_open = sk[s] == open;
        snprintf(out[lines], MUI_LINE, is_open ? "%s  v  (Enter / Left = close)" : "%s  >  (%d entr%s)", name[first[s]], cnt[s], cnt[s] == 1 ? "y" : "ies");
        ref[lines].kind = MUR_HEAD; ref[lines].ref = sk[s]; lines++;
        if (!is_open) continue;
        for (int i = 0; i < n && lines < max; i++) if (key[i] == sk[s]) { snprintf(out[lines], MUI_LINE, "    %.80s", text[i]); ref[lines].kind = MUR_ITEM; ref[lines].ref = i; lines++; }
    }
    return lines;
}
typedef struct {
    const ModInfo *mods; int nmods;
    int safe_mode; const char *safe_why;                      // safe_why: "" if not known
    const char *order;                                        // "A, B, C" (load order of the mods that load)
    int pak_conflicts, pak_conflicts_real, row_conflicts, obj_conflicts, split_assets;
    int nwarn; char warn[MUI_WARN][240];                     // content warnings: pak shadowing, superseded paks, equip conflicts (one line each, shown right under "Load order:" so they are on the first screen)
    int c_files, c_entries, c_invalid, ncconf; char cconf[MUI_CCONF][160];            // content registry: files, entries in use, invalid entries, conflict lines
    int nplug; const char *plug_name[MUI_PLUG]; int plug_faults[MUI_PLUG], plug_loaded[MUI_PLUG];   // loaded plugins and their guard faults (3 = switched off)
    const char *reload_status;                                // result of the last hot reload ("" = none yet)
    int nactions; const char *action[MUI_ACTIONS];            // labels of the action lines (first lines of the page)
} ModsUiIn;

// appends one logical line, wrapped at MUI_WRAP characters (continuation lines are indented by two blanks); returns the new line count
static int modsui_put(char (*lines)[MUI_LINE], int n, int max, const char *prefix, const char *text) {
    char buf[480]; snprintf(buf, sizeof buf, "%s%s", prefix, text); size_t len = strlen(buf), pos = 0; int first = 1;
    while ((pos < len || first) && n < max) {
        size_t room = first ? (size_t)MUI_WRAP : (size_t)MUI_WRAP - 2, take = len - pos < room ? len - pos : room;
        if (take < len - pos) { size_t k = take; while (k > take / 2 && buf[pos + k] != ' ' && buf[pos + k - 1] != ',') k--; if (k > take / 2) take = k; }     // break at a blank / comma when there is one
        snprintf(lines[n], MUI_LINE, "%s%.*s", first ? "" : "  ", (int)take, buf + pos); n++; pos += take; while (pos < len && buf[pos] == ' ') pos++; first = 0;
        if (pos >= len) break;
    }
    return n;
}
static int modsui_build(const ModsUiIn *in, char (*lines)[MUI_LINE], int max) {
    int n = 0, on = 0, off = 0, skipped = 0, empty = 0; char t[480];
    for (int i = 0; i < in->nactions && i < MUI_ACTIONS && n < max; i++) { snprintf(lines[n], MUI_LINE, ">> %.88s", in->action[i]); n++; }
    snprintf(t, sizeof t, "%s%s%s", in->safe_mode ? "ON - no mods, no plugins, no extender hooks" : "off", in->safe_mode && in->safe_why && in->safe_why[0] ? ": " : "", in->safe_mode && in->safe_why ? in->safe_why : "");
    n = modsui_put(lines, n, max, "Safe mode: ", t);
    for (int i = 0; i < in->nmods; i++) { switch (in->mods[i].status) { case MI_ON: on++; break; case MI_OFF: off++; break; case MI_SKIPPED: skipped++; break; default: empty++; } }
    snprintf(t, sizeof t, "%d loaded, %d off, %d skipped%s", on, off, skipped, empty ? ", some empty" : "");
    n = modsui_put(lines, n, max, "Mods: ", t);
    n = modsui_put(lines, n, max, "Load order: ", in->order && in->order[0] ? in->order : "(none)");
    for (int i = 0; i < in->nwarn && i < MUI_WARN; i++) n = modsui_put(lines, n, max, "WARNING: ", in->warn[i]);
    for (int i = 0; i < in->nmods; i++) {
        const ModInfo *m = &in->mods[i];
        switch (m->status) {
        case MI_ON: {
            char d[300]; int w = snprintf(d, sizeof d, "%s", m->name);
            if (m->id[0] && strcmp(m->id, m->name)) w += snprintf(d + w, sizeof d - w, " (%s)", m->id);
            if (m->version[0] && strcmp(m->version, "0")) w += snprintf(d + w, sizeof d - w, " v%s", m->version);
            w += snprintf(d + w, sizeof d - w, ":");
            if (m->paks_total) w += snprintf(d + w, sizeof d - w, " %d/%d pak", m->paks_ok, m->paks_total);
            if (m->rows_ops) w += snprintf(d + w, sizeof d - w, " %d row edit(s)", m->rows_ops);
            if (m->content_files) w += snprintf(d + w, sizeof d - w, " %d content file(s)", m->content_files);
            if (m->plugin == MIP_ALLOWED) w += snprintf(d + w, sizeof d - w, " plugin");
            if (m->plugin == MIP_BLOCKED) w += snprintf(d + w, sizeof d - w, " plugin BLOCKED (list it in Mods\\plugins.txt)");
            if (!m->affects_gameplay) w += snprintf(d + w, sizeof d - w, " [cosmetic]");
            n = modsui_put(lines, n, max, "[on] ", d); break; }
        case MI_OFF: n = modsui_put(lines, n, max, "[off] ", m->name); break;
        case MI_SKIPPED: { char d[320]; snprintf(d, sizeof d, "%s: %s", m->name, m->reason[0] ? m->reason : "skipped"); n = modsui_put(lines, n, max, "[SKIPPED] ", d); break; }
        default: n = modsui_put(lines, n, max, "[empty] ", m->name); break;
        }
    }
    snprintf(t, sizeof t, "files %d (%d differ), row edits %d, data assets %d, split assets %d%s", in->pak_conflicts, in->pak_conflicts_real, in->row_conflicts, in->obj_conflicts, in->split_assets, in->split_assets ? " - LIKELY CRASH" : "");
    n = modsui_put(lines, n, max, "Conflicts: ", t);
    snprintf(t, sizeof t, "%d file(s), %d entr%s in use, %d invalid, %d conflict(s)", in->c_files, in->c_entries, in->c_entries == 1 ? "y" : "ies", in->c_invalid, in->ncconf);
    n = modsui_put(lines, n, max, "Content: ", t);
    for (int i = 0; i < in->ncconf && i < MUI_CCONF; i++) n = modsui_put(lines, n, max, "  ", in->cconf[i]);
    if (!in->nplug) n = modsui_put(lines, n, max, "Plugins: ", "none loaded");
    for (int i = 0; i < in->nplug && i < MUI_PLUG; i++) {
        snprintf(t, sizeof t, "%s", !in->plug_loaded[i] ? "not running (load failed or refused)" : in->plug_faults[i] >= 3 ? "SWITCHED OFF after 3 faults" : in->plug_faults[i] ? "running, faults so far" : "running, no faults");
        if (in->plug_loaded[i] && in->plug_faults[i] > 0 && in->plug_faults[i] < 3) snprintf(t + strlen(t), sizeof t - strlen(t), " (%d of 3)", in->plug_faults[i]);
        char h[160]; snprintf(h, sizeof h, "Plugin %s: ", in->plug_name[i]); n = modsui_put(lines, n, max, h, t);
    }
    n = modsui_put(lines, n, max, "Last reload: ", in->reload_status && in->reload_status[0] ? in->reload_status : "none yet (the Reload line of this page, or a LanNative\\reload.txt file, reloads Mods\\*\\content and rows.json)");
    return n;
}
#endif

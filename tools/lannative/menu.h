// In-game F1 menu: pure state machine + text layout (no engine calls, so it can be driven by fake input).
// lannative.c feeds it key edges, performs the action it returns, and draws the lines it lays out.
// Keys (GetAsyncKeyState edges, only while the game window has focus): F1 open/close, Up/Down move, Enter or Right select, Left or Backspace back.
// The game also sees those keys (it has no idea the menu exists); Esc is deliberately not used because it opens the game's own pause menu.
#ifndef LANNATIVE_MENU_H
#define LANNATIVE_MENU_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum { MK_TOGGLE = 1, MK_UP, MK_DOWN, MK_SELECT, MK_BACK, MK_MODS, MK_ERASE };   // MK_MODS = F2: open the menu straight on the Mods page (or close it when it is already there); MK_ERASE = Backspace: deletes a search character, else Back
enum { MA_NONE = 0, MA_HOST, MA_JOIN, MA_SUMMON, MA_STATE, MA_MARK, MA_AUTOLEAVE, MA_PERTEAM, MA_HOSTIP, MA_OPEN_MAPS, MA_OPEN_ALTARS, MA_TRAVEL, MA_TELEPORT,
       MA_OPEN_NPCS, MA_SPAWN_NPC, MA_REMOVE_NPCS, MA_SOLODUEL, MA_EDIT_IP, MA_MODS_ACT, MA_MODS_BACK, MA_QUIT, MA_OPEN_LAN, MA_LAN_JOIN, MA_OPEN_KICK, MA_KICK };   // MA_MODS_ACT: index = line of the Mods page (the first lines are actions: reload, plugin menu items); MA_MODS_BACK: Left on the Mods page while a section is open (index = line)
enum { MP_MAIN = 0, MP_MAPS, MP_ALTARS, MP_NPCS, MP_NPCGROUPS, MP_MODS, MP_LAN, MP_MAPGROUPS, MP_KICK, MP_COUNT };   // MP_LAN: hosts found by the LAN finder (landisc.inc; MA_LAN_JOIN index = host); MP_MAPGROUPS: map categories

#define MENU_MAIN_COUNT   17
#define MENU_MAIN_LAN     14      // "LAN games..." entry (Back from the LAN page lands here)
#define MENU_MAIN_KICK    15      // "Kick player..." entry (host only; kick.inc; MA_KICK index = joiner row)
#define MENU_LIST_MAX     192     // entries kept per file-backed list (found maps + maps.txt extras)
#define MENU_URL_MAX      400     // longest URL kept (ASCII)
#define MENU_GROUPS_MAX   16      // NPC groups
#define MENU_ROWS         12      // lines drawn for a list page
#define MENU_LINE_MAX     96
#define MENU_LINES_MAX    (MENU_ROWS + 8)
#define MENU_FILTER_MAX   32      // search text on a list / category page (typed letters)
#define MENU_VIEW_MAX     512     // rows a search can show (the largest list: 512 NPC entries)

typedef struct {
    int open, page, sel;
    int count[MP_COUNT];                 // items on each page (MENU_MAIN_COUNT, maps, altars, NPCs in the open group (all of them without groups), NPC groups, Mods page lines)
    int ngroups, group;           // NPC groups: entries of group g are the contiguous range [gstart[g], gstart[g] + gcount[g]) of the NPC list; group = the open one (-1 = search over all)
    int gcount[MENU_GROUPS_MAX], gstart[MENU_GROUPS_MAX];
    int mem_page, mem_sel[MP_COUNT];     // where F1 reopens: the last main / map / altar page and line (kept by menu_init_n)
    int mods_sub;                 // set by the caller: a section of the Mods page is open, so Left closes it (MA_MODS_BACK) instead of leaving the page
    // Map categories (same shape as the NPC groups) and type-to-search on the list / category pages. menu_init_n keeps mem_mgroup only.
    int mgroups, mgroup, mgcount[MENU_GROUPS_MAX], mgstart[MENU_GROUPS_MAX];
    int total[MP_COUNT];          // entries in a whole list (maps, altars, NPCs): what a search covers
    const char *(*label)(int page, int entry);   // caller: label of list entry `entry` (whole-list index) of page MP_MAPS / MP_ALTARS / MP_NPCS; NULL = no search
    char filter[MENU_FILTER_MAX]; // search text; "" = none. Rows of a list page with a filter = view[]
    int nview, view[MENU_VIEW_MAX], vmore;   // matching entries (whole-list indices); vmore = matches that did not fit
    int mem_mgroup;
} Menu;

typedef struct { int action, index; } MenuAct;   // index = list entry for MA_TRAVEL / MA_TELEPORT / MA_SPAWN_NPC

// what the labels show; filled by the caller each frame
typedef struct { int autoleave_on, perteam, is_host_world, maps_n, altars_n; const char *hostip; int npcs_n, npc_live, solo; const char *joinaddr; int lan_n, kick_n; } MenuStatus;   // kick_n: joiners connected (host), -1 = not hosting

static int menu_clamp_list(int n) { return n < 0 ? 0 : (n > MENU_LIST_MAX ? MENU_LIST_MAX : n); }

static void menu_init_n(Menu *m, int maps_n, int altars_n, int npcs_n) {
    int mp = m->mem_page, ms[MP_COUNT], mg = m->mem_mgroup; memcpy(ms, m->mem_sel, sizeof ms);
    memset(m, 0, sizeof *m);                   // the search text and the label callback start empty: the caller sets m->label after every init
    m->mem_page = mp; memcpy(m->mem_sel, ms, sizeof ms); m->mem_mgroup = mg;
    m->count[MP_MAIN] = MENU_MAIN_COUNT;
    m->count[MP_MAPS] = m->total[MP_MAPS] = menu_clamp_list(maps_n);
    m->count[MP_ALTARS] = m->total[MP_ALTARS] = menu_clamp_list(altars_n);
    m->count[MP_NPCS] = menu_clamp_list(npcs_n); m->total[MP_NPCS] = npcs_n < 0 ? 0 : npcs_n > MENU_VIEW_MAX ? MENU_VIEW_MAX : npcs_n;
    m->count[MP_LAN] = 1;                      // the caller keeps it at max(1, hosts found): line 0 says "searching" while the list is empty
    m->count[MP_KICK] = 1;                     // kick.inc keeps it at max(1, joiners): line 0 says "no joiners" / "host only"
}
static void menu_init(Menu *m, int maps_n, int altars_n) { menu_init_n(m, maps_n, altars_n, 0); }

// Groups the NPC list: counts[g] entries in group g, in list order. n = 0 keeps the flat list.
static void menu_set_npc_groups(Menu *m, int n, const int *counts) {
    m->ngroups = 0; m->group = 0;
    if (n <= 0 || n > MENU_GROUPS_MAX) { m->count[MP_NPCGROUPS] = 0; return; }
    int start = 0;
    for (int g = 0; g < n; g++) {
        m->gcount[g] = menu_clamp_list(counts[g]); m->gstart[g] = start; start += counts[g] < 0 ? 0 : counts[g];
    }
    m->ngroups = n; m->count[MP_NPCGROUPS] = n;
}

// Map categories: counts[g] entries in category g, in list order (the caller sorts the map list by category). n = 0 keeps the flat list.
static void menu_set_map_groups(Menu *m, int n, const int *counts) {
    m->mgroups = 0; m->count[MP_MAPGROUPS] = 0;
    if (n <= 0 || n > MENU_GROUPS_MAX) return;
    int start = 0;
    for (int g = 0; g < n; g++) { m->mgcount[g] = menu_clamp_list(counts[g]); m->mgstart[g] = start; start += counts[g] < 0 ? 0 : counts[g]; }
    m->mgroups = n; m->count[MP_MAPGROUPS] = n;
    if (m->mgroup >= n) m->mgroup = n - 1;
    if (m->page == MP_MAPS && !m->filter[0] && m->mgroup >= 0) m->count[MP_MAPS] = m->mgcount[m->mgroup];
}
static int menu_is_list(int page) { return page == MP_MAPS || page == MP_ALTARS || page == MP_NPCS; }
// the part of the whole list a list page covers now: a category / group, or everything (no groups, or a search over all of them)
static void menu_range(const Menu *m, int page, int *start, int *n) {
    *start = 0; *n = m->total[page];
    if (page == MP_MAPS && m->mgroups > 0 && m->mgroup >= 0 && m->mgroup < m->mgroups) { *start = m->mgstart[m->mgroup]; *n = m->mgcount[m->mgroup]; }
    if (page == MP_NPCS && m->ngroups > 0 && m->group >= 0 && m->group < m->ngroups) { *start = m->gstart[m->group]; *n = m->gcount[m->group]; }
}
// whole-list index of row `row` of a list page (-1 = none)
static int menu_entry(const Menu *m, int row) {
    if (row < 0) return -1;
    if (menu_is_list(m->page) && m->filter[0]) return row < m->nview ? m->view[row] : -1;
    if (m->page == MP_NPCS && m->ngroups > 0 && m->group >= 0) return m->gstart[m->group] + row;
    if (m->page == MP_MAPS && m->mgroups > 0 && m->mgroup >= 0) return m->mgstart[m->mgroup] + row;
    return row;
}
// case-insensitive: every space-separated word of the filter appears in the label
static int menu_match(const char *label, const char *filter) {
    if (!label) return 0;
    const char *f = filter;
    while (*f) {
        while (*f == ' ') f++; if (!*f) break;
        int wl = 0; while (f[wl] && f[wl] != ' ') wl++;
        int found = 0;
        for (const char *s = label; *s && !found; s++) {
            int k = 0; while (k < wl && s[k]) { int a = s[k], b = f[k]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) break; k++; }
            if (k == wl) found = 1;
        }
        if (!found) return 0;
        f += wl;
    }
    return 1;
}
// Rebuilds the rows of the current list page from the search text (call after the filter, the page, the group or the list changed).
static void menu_refilter(Menu *m) {
    if (!menu_is_list(m->page)) return;
    int start, n; menu_range(m, m->page, &start, &n);
    if (!m->filter[0] || !m->label) { m->nview = 0; m->vmore = 0; m->count[m->page] = m->page == MP_NPCS ? menu_clamp_list(n) : n; }
    else {
        m->nview = 0; m->vmore = 0;
        for (int i = start; i < start + n; i++) if (menu_match(m->label(m->page, i), m->filter)) { if (m->nview < MENU_VIEW_MAX) m->view[m->nview++] = i; else m->vmore++; }
        m->count[m->page] = m->nview;
    }
    if (m->sel >= m->count[m->page]) m->sel = m->count[m->page] ? m->count[m->page] - 1 : 0;
    if (m->sel < 0) m->sel = 0;
}
// the list a category page opens
static int menu_list_of(int page) { return page == MP_MAPGROUPS ? MP_MAPS : page == MP_NPCGROUPS ? MP_NPCS : page; }
// A typed character on a list or category page: it goes into the search text. On a category page the search covers every category.
static void menu_type(Menu *m, int c) {
    if (!m->open || !m->label || c < 32 || c > 126) return;
    int p = m->page; if (!menu_is_list(p) && p != MP_MAPGROUPS && p != MP_NPCGROUPS) return;
    size_t n = strlen(m->filter); if (n + 1 >= MENU_FILTER_MAX) return;
    if (n == 0 && c == ' ') return;                                                  // a leading space would match everything
    if (p == MP_MAPGROUPS) { m->page = MP_MAPS; m->mgroup = -1; m->sel = 0; }        // search over all maps / all NPCs
    else if (p == MP_NPCGROUPS) { m->page = MP_NPCS; m->group = -1; m->sel = 0; }
    m->filter[n] = (char)c; m->filter[n + 1] = 0; m->sel = 0;
    menu_refilter(m);
}
// the search ended (Esc / Left / Backspace on the last character): a search over all categories goes back to the category page
static void menu_clear_filter(Menu *m) {
    m->filter[0] = 0;
    if (m->page == MP_MAPS && m->mgroups > 0 && m->mgroup < 0) { m->page = MP_MAPGROUPS; m->sel = 0; }
    else if (m->page == MP_NPCS && m->ngroups > 0 && m->group < 0) { m->page = MP_NPCGROUPS; m->sel = 0; }
    else { m->sel = 0; menu_refilter(m); }
}
static int menu_mem_sel(const Menu *m, int page) { int n = m->count[page]; int s = m->mem_sel[page]; return (n <= 0 || s < 0) ? 0 : (s >= n ? n - 1 : s); }
static MenuAct menu_key_raw(Menu *m, int key);
// F1 reopens on the page and line it was left on (main, map list or altar list); the other pages start at the top
static MenuAct menu_key(Menu *m, int key) {
    MenuAct a = menu_key_raw(m, key);
    if (m->open && (m->page == MP_MAIN || m->page == MP_MAPS || m->page == MP_ALTARS || m->page == MP_MAPGROUPS)) { m->mem_page = m->page; m->mem_sel[m->page] = m->sel; if (m->page == MP_MAPS) m->mem_mgroup = m->mgroup; }
    else if (a.action == MA_TRAVEL || a.action == MA_TELEPORT) { m->mem_page = a.action == MA_TRAVEL ? MP_MAPS : MP_ALTARS; m->mem_sel[m->mem_page] = m->sel; if (a.action == MA_TRAVEL) m->mem_mgroup = m->mgroup; }
    return a;
}
static MenuAct menu_key_raw(Menu *m, int key) {
    MenuAct none = { MA_NONE, 0 };
    if (key == MK_TOGGLE) { m->open = !m->open;
        if (m->open) {
            int p = m->mem_page;
            if (p == MP_MAPS && m->mgroups > 0) {                                     // back into the category (or the search) it was left in
                m->mgroup = m->mem_mgroup < m->mgroups ? m->mem_mgroup : 0;
                if (m->mgroup < 0 && !m->filter[0]) p = MP_MAPGROUPS; else { m->page = MP_MAPS; menu_refilter(m); }
            }
            if ((p == MP_MAPS || p == MP_ALTARS || p == MP_LAN || p == MP_MAPGROUPS) && m->count[p] > 0) { m->page = p; m->sel = menu_mem_sel(m, p); }
            else { m->page = MP_MAIN; m->sel = menu_mem_sel(m, MP_MAIN); }
        }
        return none; }
    if (key == MK_ERASE && m->open && m->filter[0] && (menu_is_list(m->page))) {
        size_t n = strlen(m->filter); m->filter[n - 1] = 0;
        if (!m->filter[0]) menu_clear_filter(m); else menu_refilter(m);
        return none;
    }
    if (key == MK_ERASE) key = MK_BACK;
    if (key == MK_BACK && m->open && m->filter[0] && menu_is_list(m->page)) { menu_clear_filter(m); return none; }
    if (key == MK_MODS) {
        if (m->open && m->page == MP_MODS) { m->open = 0; return none; }
        m->open = 1; m->page = MP_MODS; m->sel = 0; return none;
    }
    if (!m->open) return none;
    int n = m->count[m->page];
    if (key == MK_UP)   { if (n > 0) m->sel = (m->sel + n - 1) % n; return none; }
    if (key == MK_DOWN) { if (n > 0) m->sel = (m->sel + 1) % n; return none; }
    if (key == MK_BACK) {
        if (m->page == MP_MODS && m->mods_sub) { MenuAct a = { MA_MODS_BACK, m->sel }; return a; }
        if (m->page == MP_MODS) { m->page = MP_MAIN; m->sel = 0; }
        else if (m->page == MP_NPCS && m->ngroups > 0) { m->page = MP_NPCGROUPS; m->sel = m->group >= 0 ? m->group : 0; }       // back to the group list
        else if (m->page == MP_MAPS && m->mgroups > 0) { m->page = MP_MAPGROUPS; m->sel = m->mgroup >= 0 ? m->mgroup : 0; }      // back to the map categories
        else if (m->page != MP_MAIN) { m->sel = (m->page == MP_MAPS || m->page == MP_MAPGROUPS) ? 8 : (m->page == MP_ALTARS) ? 9 : (m->page == MP_LAN) ? MENU_MAIN_LAN : (m->page == MP_KICK) ? MENU_MAIN_KICK : 10; m->page = MP_MAIN; }   // back to the entry that opened the list
        else m->open = 0;
        return none;
    }
    if (key == MK_SELECT && n > 0) {
        if (m->page == MP_MAIN) {
            static const int act[MENU_MAIN_COUNT] = { MA_HOST, MA_JOIN, MA_SUMMON, MA_STATE, MA_MARK, MA_AUTOLEAVE, MA_PERTEAM, MA_HOSTIP, MA_OPEN_MAPS, MA_OPEN_ALTARS, MA_OPEN_NPCS, MA_REMOVE_NPCS, MA_SOLODUEL, MA_EDIT_IP, MA_OPEN_LAN, MA_OPEN_KICK, MA_QUIT };
            MenuAct a = { act[m->sel], 0 };
            if (a.action == MA_OPEN_MAPS && m->mgroups > 0) { m->filter[0] = 0; m->page = MP_MAPGROUPS; m->sel = menu_mem_sel(m, MP_MAPGROUPS); a.action = MA_NONE; }
            else if (a.action == MA_OPEN_MAPS)   { if (m->count[MP_MAPS]) { m->filter[0] = 0; m->page = MP_MAPS; menu_refilter(m); m->sel = menu_mem_sel(m, MP_MAPS); } a.action = MA_NONE; }
            else if (a.action == MA_OPEN_ALTARS) { if (m->count[MP_ALTARS]) { m->filter[0] = 0; m->page = MP_ALTARS; menu_refilter(m); m->sel = menu_mem_sel(m, MP_ALTARS); } a.action = MA_NONE; }
            else if (a.action == MA_OPEN_KICK) { if (m->count[MP_KICK] < 1) m->count[MP_KICK] = 1; m->page = MP_KICK; m->sel = 0; a.action = MA_NONE; }
            else if (a.action == MA_OPEN_LAN) { if (m->count[MP_LAN] < 1) m->count[MP_LAN] = 1; m->page = MP_LAN; m->sel = 0; a.action = MA_NONE; }
            else if (a.action == MA_OPEN_NPCS) {
                m->filter[0] = 0;
                if (m->ngroups > 0) { m->page = MP_NPCGROUPS; m->sel = 0; }
                else if (m->count[MP_NPCS]) { m->page = MP_NPCS; m->sel = 0; menu_refilter(m); }
                a.action = MA_NONE;
            }
            return a;
        }
        if (m->page == MP_MODS) { MenuAct a = { MA_MODS_ACT, m->sel }; return a; }          // stays open: the page shows the result
        if (m->page == MP_NPCGROUPS) {                                                  // open a group: its entries become the NPC page
            m->group = m->sel; m->count[MP_NPCS] = m->gcount[m->group]; m->page = MP_NPCS; m->sel = 0;
            return none;
        }
        if (m->page == MP_MAPGROUPS) {                                                  // open a category: its maps become the map page
            m->mgroup = m->sel; m->page = MP_MAPS; m->sel = 0; menu_refilter(m);
            return none;
        }
        if (m->page == MP_LAN) { MenuAct a = { MA_LAN_JOIN, m->sel }; return a; }
        if (m->page == MP_KICK) { MenuAct a = { MA_KICK, m->sel }; return a; }              // stays open: first Enter arms, second kicks (kick.h)          // stays open: the caller joins (or searches again on the 'searching' line)
        int e = menu_entry(m, m->sel); if (e < 0) return none;
        if (m->page == MP_NPCS) { MenuAct a = { MA_SPAWN_NPC, e }; return a; }           // stays open: several NPCs can be spawned in a row
        MenuAct a = { m->page == MP_ALTARS ? MA_TELEPORT : MA_TRAVEL, e };
        m->open = 0;                                     // travelling / teleporting: the menu is not needed any more
        return a;
    }
    return none;
}

static int menu_append(char (*lines)[MENU_LINE_MAX], int n, const char *fmt, const char *text, int arg) {
    if (n >= MENU_LINES_MAX) return n;
    if (text) snprintf(lines[n], MENU_LINE_MAX, fmt, text);
    else snprintf(lines[n], MENU_LINE_MAX, fmt, arg);
    return n + 1;
}

// Lays the menu out as text. Returns the number of lines; *sel_line = which line is the selected one (-1 = none). Line 0 is the title.
// list_text(i) must give the label of list entry i for the current page.
static int menu_layout(const Menu *m, const MenuStatus *s, const char *(*list_text)(int page, int i), char (*lines)[MENU_LINE_MAX], int *sel_line) {
    int n = 0; *sel_line = -1;
    if (!m->open) return 0;
    if (m->page == MP_MAIN) {
        n = menu_append(lines, n, "Gleam Loader  (F1 close, F2 mods, Up/Down, Enter)%s", "", 0);
        const char *items[MENU_MAIN_COUNT] = { "Host: %s", "Join: %s", "Bring joiners to me: %s", "State dump to log: %s", "Log marker: %s", "Auto-leave: %s", "3v3 players per team: %s", "Host address: %s", "Map list...%s", "Altar teleport...%s", "Spawn NPC...%s", "Remove all NPCs: %s", "Solo duel (test maps alone): %s", "Join address (Enter to type it): %s", "LAN games...%s", "Kick player...%s", "Quit to main menu: %s" };
        for (int i = 0; i < MENU_MAIN_COUNT; i++) {
            char val[32] = "";
            switch (i) {
                case 0: snprintf(val, sizeof val, "%s", "open my world"); break;
                case 1: snprintf(val, sizeof val, "%s", "join ip.txt"); break;
                case 2: snprintf(val, sizeof val, "%s", s->is_host_world ? "go" : "host only"); break;
                case 3: snprintf(val, sizeof val, "%s", "go"); break;
                case 4: snprintf(val, sizeof val, "%s", "go"); break;
                case 5: snprintf(val, sizeof val, "%s", s->autoleave_on ? "ON" : "off"); break;
                case 6: if (s->perteam >= 1 && s->perteam <= 2) snprintf(val, sizeof val, "%d", s->perteam); else snprintf(val, sizeof val, "%s", "game's 3"); break;
                case 7: snprintf(val, sizeof val, "%s", s->hostip && s->hostip[0] ? s->hostip : "auto"); break;
                case 8: snprintf(val, sizeof val, "(%d)", s->maps_n); break;
                case 9: snprintf(val, sizeof val, "(%d)", s->altars_n); break;
                case 10: if (s->is_host_world) snprintf(val, sizeof val, "(%d)", s->npcs_n); else snprintf(val, sizeof val, "%s", " host only"); break;
                case 11: snprintf(val, sizeof val, "%d spawned", s->npc_live); break;
                case 12: snprintf(val, sizeof val, "%s", s->solo ? "ON" : "off"); break;
                case 13: snprintf(val, sizeof val, "%.28s", s->joinaddr && s->joinaddr[0] ? s->joinaddr : "NOT SET"); break;
                case 14: snprintf(val, sizeof val, " (%d found)", s->lan_n); break;
                case 15: if (s->kick_n >= 0) snprintf(val, sizeof val, " (%d connected)", s->kick_n); else snprintf(val, sizeof val, "%s", " host only"); break;
                case 16: snprintf(val, sizeof val, "%s", "title screen"); break;
            }
            if (i == m->sel) *sel_line = n;
            char row[MENU_LINE_MAX];
            snprintf(row, sizeof row, items[i], val);
            n = menu_append(lines, n, "%s", row, 0);
        }
        return n;
    }
    char title[MENU_LINE_MAX];
    if (menu_is_list(m->page) && m->filter[0]) {
        const char *what = m->page == MP_MAPS ? "Maps" : m->page == MP_NPCS ? "NPCs" : "Altars";
        int all = (m->page == MP_MAPS && m->mgroups > 0 && m->mgroup < 0) || (m->page == MP_NPCS && m->ngroups > 0 && m->group < 0);
        const char *in = !all && m->page == MP_MAPS && m->mgroups > 0 ? list_text(MP_MAPGROUPS, m->mgroup) : !all && m->page == MP_NPCS && m->ngroups > 0 ? list_text(MP_NPCGROUPS, m->group) : "all";
        snprintf(title, sizeof title, "%s, search in %.40s: %s_   (Backspace = delete, Esc / Left = end search)", what, in, m->filter);
        n = menu_append(lines, n, "%s", title, 0);
    } else if (m->page == MP_NPCS && m->ngroups > 0) {
        snprintf(title, sizeof title, "NPCs: %s  (Enter = spawn next to you, Left = back, type to search)", list_text(MP_NPCGROUPS, m->group));
        n = menu_append(lines, n, "%s", title, 0);
    } else if (m->page == MP_MAPS && m->mgroups > 0) {
        snprintf(title, sizeof title, "Maps: %s  (Enter = travel, Left = back, type to search)", list_text(MP_MAPGROUPS, m->mgroup));
        n = menu_append(lines, n, "%s", title, 0);
    } else
        n = menu_append(lines, n, m->page == MP_MAPS ? "Maps  (Enter = travel, Left = back)%s" : m->page == MP_NPCS ? "NPCs  (Enter = spawn next to you, Left = back)%s" :
                                  m->page == MP_NPCGROUPS ? "NPC groups  (Enter = open, Left = back, type to search all NPCs)%s" : m->page == MP_MODS ? "Mods  (F2 close, Up/Down scroll, Enter on a >> line runs it, Left = menu)%s" :
                                  m->page == MP_LAN ? "LAN games  (Enter = join, Left = back)%s" : m->page == MP_KICK ? "Kick player  (Enter twice = kick, Left = back)%s" : m->page == MP_MAPGROUPS ? "Map categories  (Enter = open, Left = back, type to search all maps)%s" :
                                  "Altars  (Enter = teleport, Left = back)%s", "", 0);
    int count = m->count[m->page];
    int first = m->sel - MENU_ROWS / 2; if (first > count - MENU_ROWS) first = count - MENU_ROWS; if (first < 0) first = 0;
    for (int i = first; i < count && i < first + MENU_ROWS; i++) {
        if (i == m->sel) *sel_line = n;
        n = menu_append(lines, n, "%s", list_text(m->page, menu_is_list(m->page) ? menu_entry(m, i) : i), 0);
    }
    if (n < MENU_LINES_MAX) {
        if (!count && m->filter[0]) snprintf(lines[n], MENU_LINE_MAX, "no match for '%s'", m->filter);
        else snprintf(lines[n], MENU_LINE_MAX, "%d of %d%s", count ? m->sel + 1 : 0, count, m->vmore ? " (more matches: type more)" : "");
        n++;
    }
    return n;
}

// Label order of the F1 lists: case-insensitive, digit runs compared as numbers ("Lv2" before "Lv10"). <0 / 0 / >0 like strcmp.
static int menu_label_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++; while (*b == '0') b++;
            int la = 0, lb = 0; while (a[la] >= '0' && a[la] <= '9') la++; while (b[lb] >= '0' && b[lb] <= '9') lb++;
            if (la != lb) return la - lb;
            int c = strncmp(a, b, (size_t)la); if (c) return c;
            a += la; b += lb; continue;
        }
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
// Stable sort of entries [start, start + n) by label(i); swap(i, j) exchanges two entries. Insertion sort: a group holds at most MENU_LIST_MAX entries.
static void menu_sort_range(int start, int n, const char *(*label)(int), void (*swap)(int, int)) {
    for (int a = start + 1; a < start + n; a++)
        for (int b = a; b > start && menu_label_cmp(label(b - 1), label(b)) > 0; b--) swap(b - 1, b);
}

// One line of npcs.txt, TAB separated: [group,] label, archetype asset path (/Game/DB/AI/NPCs/Trickster/Trickster.Trickster) [, character class path
// (/Game/Blueprints/AI/BP_AICharacter_MiniBoss.BP_AICharacter_MiniBoss_C)]. Without a group the entry goes to "Other". Returns 1 and fills the outputs
// (cls = "" when none), 0 for blank lines, '#' comments or lines without a label or without a /Game/ archetype path; 0 when anything does not fit.
static int menu_parse_npc_line(const char *line, char *group, int gcap, char *label, int lcap, char *path, int pcap, char *cls, int ccap) {
    if (!line || gcap < 4 || lcap < 4 || pcap < 12 || ccap < 12) return 0;
    while (*line == ' ') line++;
    if (*line == '#' || *line == '\r' || *line == '\n' || !*line) return 0;
    char f[5][256]; int nf = 0, n = 0;
    for (const char *p = line; nf < 5; p++) {
        if (*p == '\t' || *p == '\r' || *p == '\n' || !*p) {
            while (n > 0 && f[nf][n - 1] == ' ') n--;
            f[nf][n] = 0; nf++; n = 0;
            if (*p != '\t') break;
            while (p[1] == ' ') p++;
        } else if (n < 255) f[nf][n++] = *p;
        else return 0;                                                                // a field longer than any buffer
    }
    int pi = (nf >= 2 && strncmp(f[1], "/Game/", 6) == 0) ? 1 : (nf >= 3 && strncmp(f[2], "/Game/", 6) == 0) ? 2 : -1;
    if (pi < 0 || !f[pi - 1][0] || strchr(f[pi], ' ')) return 0;
    const char *g = pi == 2 ? f[0] : "Other", *l = f[pi - 1], *c = (nf > pi + 1 && strncmp(f[pi + 1], "/Game/", 6) == 0) ? f[pi + 1] : "";
    if (!g[0] || (int)strlen(g) >= gcap || (int)strlen(l) >= lcap || (int)strlen(f[pi]) >= pcap || (int)strlen(c) >= ccap || strchr(c, ' ')) return 0;
    strcpy(group, g); strcpy(label, l); strcpy(path, f[pi]); strcpy(cls, c);
    return 1;
}

// Pulls a travel URL out of a line of maps.txt / altars.txt: the text after "Open " when the line is (optionally indented, optionally
// prefixed "Host console :") a console command "Open /Game/...". Returns 1 and the URL (ASCII, no trailing blanks) or 0.
static int menu_parse_open_line(const char *line, char *url, int cap) {
    if (!line || cap < 8) return 0;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "Host console :", 14) == 0) { line += 14; while (*line == ' ' || *line == '\t') line++; }
    if (strncmp(line, "Open /Game/", 11) != 0) return 0;
    line += 5;
    int n = 0;
    while (line[n] && line[n] != '\r' && line[n] != '\n' && n < cap - 1) { url[n] = line[n]; n++; }
    int clipped = line[n] && line[n] != '\r' && line[n] != '\n';
    while (n > 0 && (url[n - 1] == ' ' || url[n - 1] == '\t')) n--;
    url[n] = 0;
    return !clipped && n > 6;
}

// One line of altars.tsv (tab separated: label, class, zone, subzone, x, y, z, root). Returns 1 and fills label ("name (zone N)") and xyz,
// 0 for the header, blank lines or anything whose coordinates are not numbers.
static int menu_parse_altar_line(const char *line, char *label, int cap, float xyz[3]) {
    if (!line || cap < 8) return 0;
    char f[8][96]; int nf = 0, n = 0;
    for (const char *p = line; nf < 8; p++) {
        if (*p == '\t' || *p == '\r' || *p == '\n' || *p == 0) {
            f[nf][n] = 0; nf++; n = 0;
            if (*p != '\t') break;
        } else if (n < 95) f[nf][n++] = *p;
    }
    if (nf < 7) return 0;
    char *end;
    for (int i = 0; i < 3; i++) {
        xyz[i] = (float)strtod(f[4 + i], &end);
        if (end == f[4 + i] || *end) return 0;
    }
    if (!f[0][0]) return 0;
    if (f[2][0]) snprintf(label, (size_t)cap, "%s (zone %s)", f[0], f[2]);
    else snprintf(label, (size_t)cap, "%s", f[0]);
    return 1;
}

#endif

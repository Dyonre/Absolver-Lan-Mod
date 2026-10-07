// LanNative M4.0 in-game menu: pure state machine + text layout (no engine calls; tools/lannative/test/logic_test.c tests it against fake input).
// lannative.c feeds it key edges, performs the action it returns, and draws the lines it lays out.
// Keys (GetAsyncKeyState edges, only while the game window has focus): F1 open/close, Up/Down move, Enter or Right select, Left or Backspace back.
// The game also sees those keys (it has no idea the menu exists); Esc is deliberately not used because it opens the game's own pause menu.
#ifndef LANNATIVE_MENU_H
#define LANNATIVE_MENU_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum { MK_TOGGLE = 1, MK_UP, MK_DOWN, MK_SELECT, MK_BACK };
enum { MA_NONE = 0, MA_HOST, MA_JOIN, MA_SUMMON, MA_STATE, MA_MARK, MA_AUTOLEAVE, MA_PERTEAM, MA_HOSTIP, MA_OPEN_MAPS, MA_OPEN_ALTARS, MA_TRAVEL, MA_TELEPORT,
       MA_OPEN_NPCS, MA_SPAWN_NPC, MA_REMOVE_NPCS };
enum { MP_MAIN = 0, MP_MAPS, MP_ALTARS, MP_NPCS, MP_NPCGROUPS };

#define MENU_MAIN_COUNT   12
#define MENU_LIST_MAX     96      // entries kept per file-backed list
#define MENU_URL_MAX      400     // longest URL kept (ASCII)
#define MENU_GROUPS_MAX   16      // NPC groups
#define MENU_ROWS         12      // lines drawn for a list page
#define MENU_LINE_MAX     96
#define MENU_LINES_MAX    (MENU_ROWS + 4)

typedef struct {
    int open, page, sel;
    int count[5];                 // items on each page (MENU_MAIN_COUNT, maps, altars, NPCs in the open group (all of them without groups), NPC groups)
    int ngroups, group;           // NPC groups: entries of group g are the contiguous range [gstart[g], gstart[g] + gcount[g]) of the NPC list; group = the open one
    int gcount[MENU_GROUPS_MAX], gstart[MENU_GROUPS_MAX];
} Menu;

typedef struct { int action, index; } MenuAct;   // index = list entry for MA_TRAVEL / MA_TELEPORT / MA_SPAWN_NPC

// what the labels show; filled by the caller each frame
typedef struct { int autoleave_on, perteam, is_host_world, maps_n, altars_n; const char *hostip; int npcs_n, npc_live; } MenuStatus;

static int menu_clamp_list(int n) { return n < 0 ? 0 : (n > MENU_LIST_MAX ? MENU_LIST_MAX : n); }

static void menu_init_n(Menu *m, int maps_n, int altars_n, int npcs_n) {
    memset(m, 0, sizeof *m);
    m->count[MP_MAIN] = MENU_MAIN_COUNT;
    m->count[MP_MAPS] = menu_clamp_list(maps_n);
    m->count[MP_ALTARS] = menu_clamp_list(altars_n);
    m->count[MP_NPCS] = menu_clamp_list(npcs_n);
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

static MenuAct menu_key(Menu *m, int key) {
    MenuAct none = { MA_NONE, 0 };
    if (key == MK_TOGGLE) { m->open = !m->open; if (m->open) { m->page = MP_MAIN; m->sel = 0; } return none; }
    if (!m->open) return none;
    int n = m->count[m->page];
    if (key == MK_UP)   { if (n > 0) m->sel = (m->sel + n - 1) % n; return none; }
    if (key == MK_DOWN) { if (n > 0) m->sel = (m->sel + 1) % n; return none; }
    if (key == MK_BACK) {
        if (m->page == MP_NPCS && m->ngroups > 0) { m->page = MP_NPCGROUPS; m->sel = m->group; }                                // back to the group list
        else if (m->page != MP_MAIN) { m->sel = (m->page == MP_MAPS) ? 8 : (m->page == MP_ALTARS) ? 9 : 10; m->page = MP_MAIN; }   // back to the entry that opened the list
        else m->open = 0;
        return none;
    }
    if (key == MK_SELECT && n > 0) {
        if (m->page == MP_MAIN) {
            static const int act[MENU_MAIN_COUNT] = { MA_HOST, MA_JOIN, MA_SUMMON, MA_STATE, MA_MARK, MA_AUTOLEAVE, MA_PERTEAM, MA_HOSTIP, MA_OPEN_MAPS, MA_OPEN_ALTARS, MA_OPEN_NPCS, MA_REMOVE_NPCS };
            MenuAct a = { act[m->sel], 0 };
            if (a.action == MA_OPEN_MAPS)   { if (m->count[MP_MAPS]) { m->page = MP_MAPS; m->sel = 0; } a.action = MA_NONE; }
            else if (a.action == MA_OPEN_ALTARS) { if (m->count[MP_ALTARS]) { m->page = MP_ALTARS; m->sel = 0; } a.action = MA_NONE; }
            else if (a.action == MA_OPEN_NPCS) {
                if (m->ngroups > 0) { m->page = MP_NPCGROUPS; m->sel = 0; }
                else if (m->count[MP_NPCS]) { m->page = MP_NPCS; m->sel = 0; }
                a.action = MA_NONE;
            }
            return a;
        }
        if (m->page == MP_NPCGROUPS) {                                                  // open a group: its entries become the NPC page
            m->group = m->sel; m->count[MP_NPCS] = m->gcount[m->group]; m->page = MP_NPCS; m->sel = 0;
            return none;
        }
        if (m->page == MP_NPCS) { MenuAct a = { MA_SPAWN_NPC, (m->ngroups > 0 ? m->gstart[m->group] : 0) + m->sel }; return a; }   // stays open: several NPCs can be spawned in a row
        MenuAct a = { m->page == MP_ALTARS ? MA_TELEPORT : MA_TRAVEL, m->sel };
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
        n = menu_append(lines, n, "LanNative menu  (F1 close, Up/Down, Enter)%s", "", 0);
        const char *items[MENU_MAIN_COUNT] = { "Host (F6): %s", "Join (F7): %s", "Bring joiners to me (F3): %s", "State dump to log (F5): %s", "Log marker (F8): %s", "Auto-leave: %s", "3v3 players per team: %s", "Host address: %s", "Map list...%s", "Altar teleport...%s", "Spawn NPC...%s", "Remove all NPCs: %s" };
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
            }
            if (i == m->sel) *sel_line = n;
            char row[MENU_LINE_MAX];
            snprintf(row, sizeof row, items[i], val);
            n = menu_append(lines, n, "%s", row, 0);
        }
        return n;
    }
    if (m->page == MP_NPCS && m->ngroups > 0) {
        char title[MENU_LINE_MAX];
        snprintf(title, sizeof title, "NPCs: %s  (Enter = spawn next to you, Left = back)", list_text(MP_NPCGROUPS, m->group));
        n = menu_append(lines, n, "%s", title, 0);
    } else
        n = menu_append(lines, n, m->page == MP_MAPS ? "Maps  (Enter = travel, Left = back)%s" : m->page == MP_NPCS ? "NPCs  (Enter = spawn next to you, Left = back)%s" :
                                  m->page == MP_NPCGROUPS ? "NPC groups  (Enter = open, Left = back)%s" : "Altars  (Enter = teleport, Left = back)%s", "", 0);
    int count = m->count[m->page];
    int first = m->sel - MENU_ROWS / 2; if (first > count - MENU_ROWS) first = count - MENU_ROWS; if (first < 0) first = 0;
    for (int i = first; i < count && i < first + MENU_ROWS; i++) {
        if (i == m->sel) *sel_line = n;
        n = menu_append(lines, n, "%s", list_text(m->page, i), 0);
    }
    if (n < MENU_LINES_MAX) { snprintf(lines[n], MENU_LINE_MAX, "%d of %d", m->sel + 1, count); n++; }
    return n;
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

// keybinds.h - keybind registry: key names, a user remap file, conflict detection. Pure code, no Windows calls.
// Used by LuaHost for abs.bind.
//   Binding:  owner (mod / plugin), action id, label, default key.
//   Remap file (one per line, '#' comments):   <owner>.<action> = <key> | off       e.g.  LuaDemo.toggle = F7
//   Keys: F1..F24, A..Z, 0..9, NUMPAD0..NUMPAD9, and 0x01..0xFE. Names are case-insensitive.
// Reserved keys = the loader's own hotkeys (kb_reserved): a binding on one of them is a conflict, it stays bound (both fire) but is reported.
#ifndef LUAHOST_KEYBINDS_H
#define LUAHOST_KEYBINDS_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define KB_MAX 128
#define KB_NAME 64
typedef struct { char owner[KB_NAME], action[KB_NAME], label[96]; int def_vk, vk; int remapped; } KbBind;   // vk = effective key (0 = off)
typedef struct { KbBind b[KB_MAX]; int n; } KbTable;

static const struct { int vk; const char *what; } KB_RESERVED[] = {
    { 0x70, "F1 LanNative menu" }, { 0x71, "F2 Mods page" }, { 0x72, "F3 LAN summon" }, { 0x73, "F4 hot reload" }, { 0x74, "F5 LAN state" }, { 0x75, "F6 LAN travel" },
    { 0x76, "F7 LAN join" }, { 0x77, "F8 telemetry marker" }, { 0x78, "F9 free camera file" }, { 0x79, "F10 goto file" }, { 0x7A, "F11 loader self-test" }, { 0, NULL } };
static const char *kb_reserved(int vk) { for (int i = 0; KB_RESERVED[i].vk; i++) if (KB_RESERVED[i].vk == vk) return KB_RESERVED[i].what; return NULL; }

// key name -> virtual key, 0 = unknown
static int kb_parse_key(const char *s) {
    if (!s) return 0; while (*s == ' ') s++;
    char k[24]; size_t n = 0; while (s[n] && s[n] != ' ' && s[n] != '\r' && s[n] != '\n' && s[n] != '\t' && n < sizeof k - 1) { k[n] = (char)toupper((unsigned char)s[n]); n++; } k[n] = 0;
    if (!n) return 0;
    if (n >= 3 && k[0] == '0' && k[1] == 'X') { char *e; long v = strtol(k, &e, 16); return !*e && v >= 1 && v <= 0xFE ? (int)v : 0; }
    if (n == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
    if (k[0] == 'F' && n <= 3) { int f = atoi(k + 1); if (f >= 1 && f <= 24 && (n == 2 ? isdigit((unsigned char)k[1]) : isdigit((unsigned char)k[1]) && isdigit((unsigned char)k[2]))) return 0x6F + f; return 0; }
    if (!strncmp(k, "NUMPAD", 6) && n == 7 && isdigit((unsigned char)k[6])) return 0x60 + (k[6] - '0');
    return 0;
}
static void kb_key_name(int vk, char *out, size_t cap) {
    if (vk == 0) snprintf(out, cap, "off");
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(out, cap, "%c", vk);
    else if (vk >= 0x70 && vk <= 0x87) snprintf(out, cap, "F%d", vk - 0x6F);
    else if (vk >= 0x60 && vk <= 0x69) snprintf(out, cap, "NUMPAD%d", vk - 0x60);
    else snprintf(out, cap, "0x%02X", vk);
}
static int kb_id_ok(const char *s) { size_t n = s ? strlen(s) : 0; if (!n || n >= KB_NAME) return 0; for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return 0; return 1; }

// register; returns the index, -1 = bad arguments / full / duplicate (owner, action)
static int kb_add(KbTable *t, const char *owner, const char *action, const char *label, int def_vk) {
    if (!kb_id_ok(owner) || !kb_id_ok(action) || def_vk < 0 || def_vk > 0xFE || t->n >= KB_MAX) return -1;
    for (int i = 0; i < t->n; i++) if (!strcmp(t->b[i].owner, owner) && !strcmp(t->b[i].action, action)) return -1;
    KbBind *b = &t->b[t->n]; memset(b, 0, sizeof *b); snprintf(b->owner, sizeof b->owner, "%s", owner); snprintf(b->action, sizeof b->action, "%s", action);
    snprintf(b->label, sizeof b->label, "%s", label ? label : action); b->def_vk = b->vk = def_vk; return t->n++;
}
// apply the remap text to every binding (call again after adding bindings). Returns the number of lines that did not apply; why[0] = the first problem
static int kb_apply_remap(KbTable *t, const char *text, char *why, size_t cap) {
    int bad = 0, line = 0; if (why && cap) why[0] = 0;
    for (int i = 0; i < t->n; i++) { t->b[i].vk = t->b[i].def_vk; t->b[i].remapped = 0; }
    while (text && *text) {
        const char *e = text; while (*e && *e != '\n') e++; line++;
        char l[200]; size_t n = (size_t)(e - text); if (n >= sizeof l) n = sizeof l - 1; memcpy(l, text, n); l[n] = 0; text = *e ? e + 1 : e;
        char *c = strchr(l, '#'); if (c) *c = 0; char *p = l; while (*p == ' ' || *p == '\t') p++; size_t L = strlen(p); while (L && (p[L - 1] == ' ' || p[L - 1] == '\r' || p[L - 1] == '\t')) p[--L] = 0;
        if (!*p) continue;
        char *eq = strchr(p, '='), *dot = strchr(p, '.');
        if (!eq || !dot || dot > eq) { if (!bad++ && why) snprintf(why, cap, "line %d: expected <owner>.<action> = <key>", line); continue; }
        *eq = 0; *dot = 0; char *owner = p, *action = dot + 1, *key = eq + 1; for (char *q = action + strlen(action); q > action && q[-1] == ' '; ) *--q = 0;
        while (*key == ' ') key++;
        int vk = !_stricmp(key, "off") ? 0 : kb_parse_key(key);
        if (vk == 0 && _stricmp(key, "off")) { if (!bad++ && why) snprintf(why, cap, "line %d: unknown key '%.20s'", line, key); continue; }
        int hit = 0; for (int i = 0; i < t->n; i++) if (!_stricmp(t->b[i].owner, owner) && !_stricmp(t->b[i].action, action)) { t->b[i].vk = vk; t->b[i].remapped = 1; hit = 1; }
        if (!hit) { if (!bad++ && why) snprintf(why, cap, "line %d: no binding %s.%s (yet)", line, owner, action); }
    }
    return bad;
}
// conflicts: two live bindings on one key, or a binding on a loader key. Writes a report, returns the number of conflicts.
static int kb_conflicts(const KbTable *t, char *out, size_t cap) {
    size_t w = 0; int n = 0; if (cap) out[0] = 0; char k[16];
#define KW(...) do { if (w < cap) { int r_ = snprintf(out + w, cap - w, __VA_ARGS__); if (r_ > 0) w += (size_t)r_; if (w >= cap) w = cap - 1; } } while (0)
    for (int i = 0; i < t->n; i++) {
        if (!t->b[i].vk) continue; kb_key_name(t->b[i].vk, k, sizeof k);
        const char *r = kb_reserved(t->b[i].vk); if (r) { n++; KW("%s: %s.%s and the loader (%s)\r\n", k, t->b[i].owner, t->b[i].action, r); }
        for (int j = i + 1; j < t->n; j++) if (t->b[j].vk == t->b[i].vk) { n++; KW("%s: %s.%s and %s.%s\r\n", k, t->b[i].owner, t->b[i].action, t->b[j].owner, t->b[j].action); }
    }
#undef KW
    return n;
}
#endif

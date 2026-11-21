// keyreg.h - who uses which key Pure C, no engine calls.
// The loader notes its own global keys (F1 menu, F2 Mods page, the test keys when devkeys.txt is there), every plugin on_key registration and every
// AbsApi::declare_key (plugins that poll keys themselves), plus keys Windows / Steam take. A key with two different owners is a clash: logged once and shown
// on the F2 Mods page ("Keys" rows). Keys only used while a window or menu is open (Up / Down / Enter inside a menu) are not global and are not noted here.
// The table grows as needed (no fixed limit); a failed allocation keeps what is there and counts the rest as dropped.
#ifndef LANNATIVE_KEYREG_H
#define LANNATIVE_KEYREG_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum { KR_LOADER = 0, KR_ON_KEY, KR_DECLARED, KR_RESERVED };   // how the use was learned
typedef struct { int vk, kind; char who[48], what[64]; } KeyUse;
typedef struct { KeyUse *u; int n, cap, dropped; } KeyReg;

// readable name of a virtual key: "F10", "K", "Enter", "Insert", "0x6A"
static const char *keyreg_vk_name(int vk, char *buf, size_t cap) {
    static const struct { int vk; const char *n; } T[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Enter" }, { 0x10, "Shift" }, { 0x11, "Ctrl" }, { 0x12, "Alt" }, { 0x13, "Pause" }, { 0x14, "Caps Lock" },
        { 0x1B, "Esc" }, { 0x20, "Space" }, { 0x21, "Page Up" }, { 0x22, "Page Down" }, { 0x23, "End" }, { 0x24, "Home" }, { 0x25, "Left" }, { 0x26, "Up" },
        { 0x27, "Right" }, { 0x28, "Down" }, { 0x2C, "Print Screen" }, { 0x2D, "Insert" }, { 0x2E, "Delete" }, { 0x5B, "Left Win" }, { 0x5C, "Right Win" }, { 0x90, "Num Lock" },
        { 0x91, "Scroll Lock" }, { 0xBA, ";" }, { 0xBB, "=" }, { 0xBC, "," }, { 0xBD, "-" }, { 0xBE, "." }, { 0xBF, "/" }, { 0xC0, "`" }, { 0xDB, "[" }, { 0xDC, "\\" },
        { 0xDD, "]" }, { 0xDE, "'" } };
    if (vk >= 0x70 && vk <= 0x87) { snprintf(buf, cap, "F%d", vk - 0x70 + 1); return buf; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { snprintf(buf, cap, "%c", vk); return buf; }
    if (vk >= 0x60 && vk <= 0x69) { snprintf(buf, cap, "NumPad %d", vk - 0x60); return buf; }
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].vk == vk) { snprintf(buf, cap, "%s", T[i].n); return buf; }
    snprintf(buf, cap, "0x%02X", vk & 0xFF); return buf;
}
// "F10", "f10", "K", "Insert", "0x79" -> virtual key, 0 = not understood
static int keyreg_vk_parse(const char *s) {
    if (!s) return 0; while (*s == ' ') s++;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 24) return 0x70 + n - 1; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { long v = strtol(s + 2, NULL, 16); return v > 0 && v < 255 ? (int)v : 0; }
    if (s[0] && (s[1] == 0 || s[1] == ' ')) { char c = s[0] >= 'a' && s[0] <= 'z' ? (char)(s[0] - 32) : s[0]; if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c; }
    for (int vk = 1; vk < 255; vk++) { char b[24]; keyreg_vk_name(vk, b, sizeof b); if (b[0] != '0' && !_stricmp(b, s)) return vk; }
    return 0;
}
// other owners of vk besides `who` (by name): their count; the first one's index in *first (-1 none)
static int keyreg_others(const KeyReg *r, int vk, const char *who, int *first) {
    int n = 0; if (first) *first = -1;
    for (int i = 0; i < r->n; i++) if (r->u[i].vk == vk && _stricmp(r->u[i].who, who) != 0) { if (first && *first < 0) *first = i; n++; }
    return n;
}
// Notes one use. Returns -1 = bad arguments / no memory, 0 = noted (no clash), 1 = noted and it clashes (another owner already has vk),
// 2 = the same owner already noted this key (not added again).
static int keyreg_add(KeyReg *r, int vk, int kind, const char *who, const char *what) {
    if (!r || vk <= 0 || vk > 254 || !who || !who[0]) return -1;
    for (int i = 0; i < r->n; i++) if (r->u[i].vk == vk && !_stricmp(r->u[i].who, who)) return 2;
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 32; KeyUse *nu = (KeyUse *)realloc(r->u, (size_t)cap * sizeof *nu);
        if (!nu) { r->dropped++; return -1; }
        r->u = nu; r->cap = cap;
    }
    KeyUse *k = &r->u[r->n++]; k->vk = vk; k->kind = kind;
    snprintf(k->who, sizeof k->who, "%s", who); snprintf(k->what, sizeof k->what, "%s", what && what[0] ? what : "(no description)");
    return keyreg_others(r, vk, who, NULL) > 0 ? 1 : 0;
}
static int keyreg_clash_count(const KeyReg *r) {                     // keys with more than one owner
    int n = 0;
    for (int i = 0; i < r->n; i++) {
        int first_of_vk = 1; for (int j = 0; j < i; j++) if (r->u[j].vk == r->u[i].vk) { first_of_vk = 0; break; }
        if (first_of_vk && keyreg_others(r, r->u[i].vk, r->u[i].who, NULL) > 0) n++;
    }
    return n;
}
// Display rows, one per key (sorted by key code), clashes marked: "F10  MyMod: open its window" / "F7  CLASH: A (open), B (debug)". Returns rows written.
static int keyreg_lines(const KeyReg *r, char (*out)[128], int cap) {
    int rows = 0;
    for (int vk = 1; vk < 255 && rows < cap; vk++) {
        int idx[8], n = 0; for (int i = 0; i < r->n; i++) if (r->u[i].vk == vk) { if (n < 8) idx[n] = i; n++; }
        if (!n) continue;
        char kn[24]; keyreg_vk_name(vk, kn, sizeof kn); int w;
        if (n == 1) { snprintf(out[rows++], 128, "%-6s %s: %s", kn, r->u[idx[0]].who, r->u[idx[0]].what); continue; }
        w = snprintf(out[rows], 128, "%-6s CLASH:", kn);
        for (int k = 0; k < n && k < 8 && w > 0 && w < 127; k++) w += snprintf(out[rows] + w, (size_t)(128 - w), "%s %s (%s)", k ? "," : "", r->u[idx[k]].who, r->u[idx[k]].what);
        rows++;
    }
    return rows;
}
static void keyreg_free(KeyReg *r) { free(r->u); memset(r, 0, sizeof *r); }
#endif

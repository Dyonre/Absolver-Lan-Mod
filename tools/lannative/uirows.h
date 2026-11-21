// uirows.h - loader-drawn plugin rows (API 2.27 ui_toggle / ui_number / ui_button / ui_hint / ui_set; docs/API-REFERENCE.md):
// the row table entry and its pure rules (key check, step / wrap, clamp, stored text, row text). No engine calls.
// The unit uirows.inc keeps the table, stores values in the plugin's kv ("ui:<key>") and draws the rows in the plugin's section of the loader window's Mods tab.
#ifndef LANNATIVE_UIROWS_H
#define LANNATIVE_UIROWS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { UIR_TOGGLE = 1, UIR_NUMBER, UIR_BUTTON, UIR_HINT };
#define UIR_MAX 64             // rows of all plugins together
#define UIR_KEY 40             // key: 1..39 characters A-Z a-z 0-9 _ . - (the bind id is "ui_<key>", at most 42 of bindreg's 47)
#define UIR_LABEL 64
#define UIR_TEXT 96
typedef struct {
    int plugin, kind; char key[UIR_KEY], label[UIR_LABEL], text[UIR_TEXT];   // text: hint rows only
    double v, min, max, step;                                                 // toggle: v 0 / 1; number: min < max, step > 0
    void (*cb_toggle)(int on, void *user); void (*cb_number)(double v, void *user); void (*cb_button)(void *user); void *user;
    int bind;                                                                 // Keybinds-tab action handle (toggle / button), -1 = none
    int restore;                                                              // 1 = the first cb call (stored or default value) waits for the next game tick
} UiRow;

static int uir_key_ok(const char *k) {
    if (!k || !*k || strlen(k) >= UIR_KEY) return 0;
    for (const char *c = k; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-')) return 0;
    return 1;
}
// a number on the row's grid (min + n * step), inside min..max; NaN -> min
static double uir_clamp(double v, double mn, double mx, double step) {
    if (v != v) return mn;
    if (step > 0) v = mn + floor((v - mn) / step + 0.5) * step;
    if (v < mn) v = mn; if (v > mx) v = mx;
    if (fabs(v) < 1e-12) v = 0;                                               // -0 / rounding dust prints as 0
    return v;
}
// what one select (Enter, click, pad A, a bound key) makes of the value: a toggle flips, a number goes one step up and wraps to min after max
static double uir_next(const UiRow *r) {
    if (r->kind == UIR_TOGGLE) return r->v ? 0 : 1;
    if (r->kind != UIR_NUMBER) return r->v;
    if (r->v >= r->max - r->step * 1e-6) return r->min;
    return uir_clamp(r->v + r->step, r->min, r->max, r->step);
}
// a stored / typed value: the whole text must be a number (the kv may hold anything); 1 = ok
static int uir_parse(const char *s, double *out) {
    if (!s || !*s) return 0; char *e; double v = strtod(s, &e);
    while (*e == ' ') e++;
    if (*e || v != v) return 0; *out = v; return 1;
}
static void uir_fmt(double v, char *out, size_t cap) { snprintf(out, cap, "%.6g", v); }
// the row as the Mods page shows it (both the text menu and the ImGui page)
static void uir_line(const UiRow *r, char *out, size_t cap) {
    char a[32], b[32], c[32];
    switch (r->kind) {
    case UIR_TOGGLE: snprintf(out, cap, "%s: %s", r->label, r->v ? "ON" : "off"); break;
    case UIR_NUMBER: uir_fmt(r->v, a, sizeof a); uir_fmt(r->min, b, sizeof b); uir_fmt(r->max, c, sizeof c); snprintf(out, cap, "%s: %s  (%s..%s)", r->label, a, b, c); break;
    case UIR_BUTTON: snprintf(out, cap, "%s", r->label); break;
    default: snprintf(out, cap, "%s", r->text); break;
    }
}
#endif

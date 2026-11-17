// bindreg.h - bindable actions (key binding). Pure C, no engine calls. Used by keys.inc.
// An action = (mod, id) with a label, a default input and a context. The player rebinds it on the F2 Keys rows; what differs from the default is
// stored as "Mod/id = Input" lines (LanNative\binds.txt, loader-owned; lines of mods that are not loaded stay in the file).
// Inputs are one int: 1..254 a Windows virtual key (keyboard, and the mouse buttons 1 2 4 5 6), BIND_PAD + XInput bit (0..15) a pad button, BIND_LT / BIND_RT
// the triggers, 0 = unbound. Conflicts are WARNED, never blocked: two actions conflict when they have the same input and overlapping contexts.
// Contexts: "always" overlaps every context; any other name (on foot = "foot", "riding", "menu", or a mod's own "mymod:air") overlaps only itself.
#ifndef LANNATIVE_BINDREG_H
#define LANNATIVE_BINDREG_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "keyreg.h"

#define BIND_PAD 0x100
#define BIND_LT  0x110
#define BIND_RT  0x111
#define BIND_MAX_CODE 0x111
enum { BIND_FIXED = 1 };                                             // flags: a key the loader or a mod reads itself; listed for conflicts, not rebindable here
typedef struct { char mod[96], id[48], label[64], ctx[48]; int def, code, flags; } BindAct;   // mod: a folder name (MOD_NAME_MAX 96); longer ids / contexts are refused
typedef struct { char key[160]; int code; } BindOverride;            // "Mod/id" -> input (0 = unbound by the player)
typedef struct { BindAct *a; int n, cap; BindOverride *o; int no, ocap; } BindReg;

static const char *const g_bind_pad_names[18] = { "Pad Up", "Pad Down", "Pad Left", "Pad Right", "Pad Start", "Pad Back", "Pad LS", "Pad RS",
    "Pad LB", "Pad RB", "Pad 0x400", "Pad 0x800", "Pad A", "Pad B", "Pad X", "Pad Y", "Pad LT", "Pad RT" };   // XInput bit order, then LT RT
// readable name: "K", "F8", "Mouse 4", "Pad A", "Pad LT", "(none)"
static const char *bind_code_name(int code, char *buf, size_t cap) {
    if (code <= 0) { snprintf(buf, cap, "(none)"); return buf; }
    if (code >= BIND_PAD && code <= BIND_MAX_CODE) { snprintf(buf, cap, "%s", g_bind_pad_names[code - BIND_PAD]); return buf; }
    static const char *const mouse[7] = { "", "Mouse Left", "Mouse Right", "", "Mouse Middle", "Mouse 4", "Mouse 5" };
    if (code < 7 && mouse[code][0]) { snprintf(buf, cap, "%s", mouse[code]); return buf; }
    return keyreg_vk_name(code, buf, cap);
}
// "K", "f8", "Mouse 4", "pad a", "none" / "" -> code; -1 = not understood (0 = unbound)
static int bind_code_parse(const char *s) {
    if (!s) return -1; while (*s == ' ' || *s == '\t') s++;
    char t[40]; size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    if (n >= sizeof t) return -1; memcpy(t, s, n); t[n] = 0;
    if (!t[0] || !_stricmp(t, "none") || !_stricmp(t, "(none)")) return 0;
    char b[24];
    for (int c = BIND_PAD; c <= BIND_MAX_CODE; c++) if (!_stricmp(t, bind_code_name(c, b, sizeof b))) return c;
    for (int c = 1; c < 7; c++) if (c != 3 && !_stricmp(t, bind_code_name(c, b, sizeof b))) return c;
    int vk = keyreg_vk_parse(t); return vk > 0 ? vk : -1;
}
static int bind_ctx_overlap(const char *a, const char *b) {
    if (!a || !a[0] || !b || !b[0]) return 1;
    return !_stricmp(a, "always") || !_stricmp(b, "always") || !_stricmp(a, b);
}
static int bindreg_find(const BindReg *r, const char *mod, const char *id) {
    for (int i = 0; i < r->n; i++) if (!_stricmp(r->a[i].mod, mod) && !_stricmp(r->a[i].id, id)) return i;
    return -1;
}
static int bindreg_override_of(const BindReg *r, const char *mod, const char *id) {   // index in r->o, -1 none
    char k[160]; snprintf(k, sizeof k, "%s/%s", mod, id);
    for (int i = 0; i < r->no; i++) if (!_stricmp(r->o[i].key, k)) return i;
    return -1;
}
// Declares an action (or updates label / default / context of one already declared). Returns its index, -1 = bad arguments / no memory.
static int bindreg_add(BindReg *r, const char *mod, const char *id, const char *label, int def, const char *ctx, int flags) {
    if (!r || !mod || !mod[0] || !id || !id[0] || def < 0 || def > BIND_MAX_CODE || strchr(id, '=') || strchr(id, '/') || strchr(mod, '/') || strchr(mod, '=')
        || strlen(mod) >= sizeof r->a[0].mod || strlen(id) >= sizeof r->a[0].id || (ctx && strlen(ctx) >= sizeof r->a[0].ctx)) return -1;   // a cut name would never match again
    int i = bindreg_find(r, mod, id);
    if (i < 0) {
        if (r->n == r->cap) { int cap = r->cap ? r->cap * 2 : 32; BindAct *na = (BindAct *)realloc(r->a, (size_t)cap * sizeof *na); if (!na) return -1; r->a = na; r->cap = cap; }
        i = r->n++; memset(&r->a[i], 0, sizeof r->a[i]);
        snprintf(r->a[i].mod, sizeof r->a[i].mod, "%s", mod); snprintf(r->a[i].id, sizeof r->a[i].id, "%s", id);
    }
    BindAct *a = &r->a[i];
    snprintf(a->label, sizeof a->label, "%s", label && label[0] ? label : id); snprintf(a->ctx, sizeof a->ctx, "%s", ctx && ctx[0] ? ctx : "always");
    a->def = def; a->flags = flags;
    int o = (flags & BIND_FIXED) ? -1 : bindreg_override_of(r, mod, id);
    a->code = o >= 0 ? r->o[o].code : def;
    return i;
}
// The player's choice for action i: code (0 = unbound). Returns 1 = changed, 0 = same as before, -1 = bad index / fixed / no memory.
// Choosing the default again removes the stored line.
static int bindreg_set(BindReg *r, int i, int code) {
    if (!r || i < 0 || i >= r->n || (r->a[i].flags & BIND_FIXED) || code < 0 || code > BIND_MAX_CODE) return -1;
    BindAct *a = &r->a[i]; int was = a->code, o = bindreg_override_of(r, a->mod, a->id);
    if (code == a->def) { if (o >= 0) { r->o[o] = r->o[r->no - 1]; r->no--; } }
    else {
        if (o < 0) {
            if (r->no == r->ocap) { int cap = r->ocap ? r->ocap * 2 : 32; BindOverride *nb = (BindOverride *)realloc(r->o, (size_t)cap * sizeof *nb); if (!nb) return -1; r->o = nb; r->ocap = cap; }
            o = r->no++; snprintf(r->o[o].key, sizeof r->o[o].key, "%s/%s", a->mod, a->id);
        }
        r->o[o].code = code;
    }
    a->code = code;
    return was != code;
}
// Reads the stored lines ("Mod/id = Input", '#' comments). Returns lines used; *bad = lines not understood (left out). Applies them to actions already declared.
static int bindreg_load(BindReg *r, const char *text, int *bad) {
    int used = 0, nbad = 0; if (bad) *bad = 0; if (!text) return 0;
    for (const char *p = text; *p; ) {
        const char *e = strpbrk(p, "\r\n"); size_t ln = e ? (size_t)(e - p) : strlen(p); char line[200];
        if (ln < sizeof line) {
            memcpy(line, p, ln); line[ln] = 0; char *s = line; while (*s == ' ' || *s == '\t') s++;
            if (*s && *s != '#') {
                char *eq = strchr(s, '='), *sl = strchr(s, '/'); int code = eq ? bind_code_parse(eq + 1) : -1;
                if (eq && sl && sl < eq && code >= 0) {
                    *eq = 0; size_t kl = strlen(s); while (kl && (s[kl - 1] == ' ' || s[kl - 1] == '\t')) s[--kl] = 0;
                    *sl = 0; const char *mod = s, *id = sl + 1;
                    if (mod[0] && id[0]) {
                        int o = bindreg_override_of(r, mod, id);
                        if (o < 0) {
                            if (r->no == r->ocap) { int cap = r->ocap ? r->ocap * 2 : 32; BindOverride *nb = (BindOverride *)realloc(r->o, (size_t)cap * sizeof *nb); if (!nb) break; r->o = nb; r->ocap = cap; }
                            o = r->no++; snprintf(r->o[o].key, sizeof r->o[o].key, "%s/%s", mod, id);
                        }
                        r->o[o].code = code; used++;
                        int a = bindreg_find(r, mod, id); if (a >= 0 && !(r->a[a].flags & BIND_FIXED)) r->a[a].code = code;
                    } else nbad++;
                } else nbad++;
            }
        } else nbad++;
        p += ln; while (*p == '\r' || *p == '\n') p++;
    }
    if (bad) *bad = nbad;
    return used;
}
// The file text for the stored lines. Returns the length, -1 = cap too small.
static int bindreg_save(const BindReg *r, char *out, size_t cap) {
    int w = snprintf(out, cap, "# Gleam Loader key binds: Mod/action = input (keys like K or F8, Mouse 4, Pad A, none). Written by the F2 Keys rows; defaults are not listed.\r\n");
    for (int i = 0; i < r->no && w >= 0 && (size_t)w < cap; i++) { char b[24]; w += snprintf(out + w, cap - (size_t)w, "%s = %s\r\n", r->o[i].key, r->o[i].code ? bind_code_name(r->o[i].code, b, sizeof b) : "none"); }
    return w >= 0 && (size_t)w < cap ? w : -1;
}
// Other actions with the same input in an overlapping context: their indexes (up to cap) in out; returns how many there are.
static int bindreg_conflicts(const BindReg *r, int i, int *out, int cap) {
    if (i < 0 || i >= r->n || !r->a[i].code) return 0;
    int n = 0;
    for (int j = 0; j < r->n; j++) if (j != i && r->a[j].code == r->a[i].code && bind_ctx_overlap(r->a[i].ctx, r->a[j].ctx)) { if (n < cap && out) out[n] = j; n++; }
    return n;
}
static int bindreg_conflict_count(const BindReg *r) { int n = 0; for (int i = 0; i < r->n; i++) if (!(r->a[i].flags & BIND_FIXED) && bindreg_conflicts(r, i, NULL, 0)) n++; return n; }
static void bindreg_free(BindReg *r) { free(r->a); free(r->o); memset(r, 0, sizeof *r); }
#endif

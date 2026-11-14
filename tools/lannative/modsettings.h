#ifndef LANNATIVE_MODSETTINGS_H
#define LANNATIVE_MODSETTINGS_H
// Mod settings: the optional "settings" list of mod.json, validation of the values a player types, and the text of Mods\<mod>\plugin.cfg
// (pure code, no Windows calls). The Mods page draws one form per mod from this list.
//   "settings": [ { "key": "speed", "type": "float", "default": 1.0, "min": 0.5, "max": 3, "label": "Speed", "help": "Playback speed" },
//                 { "key": "mode", "type": "choice", "choices": ["easy", "hard"], "default": "easy" }, { "key": "on", "type": "bool", "default": true },
//                 { "key": "count", "type": "int", "default": 3, "min": 1, "max": 9 }, { "key": "name", "type": "text", "default": "" } ]
// plugin.cfg stores "key = value" lines (a bool is 1 or 0). Nothing is written unless every changed value is valid.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rowsjson.h"

#define MSET_MAX 24                  // settings per mod
#define MSET_CHOICES 16
#define MSET_TEXT 128                // longest value (with the terminator)
enum { MSETT_BOOL = 0, MSETT_INT, MSETT_FLOAT, MSETT_CHOICE, MSETT_TEXT };
typedef struct {
    char key[48], label[64], help[160], def[MSET_TEXT];
    int type, has_min, has_max, nchoice; double min, max; char choice[MSET_CHOICES][48];
} ModSetting;
typedef struct { int n; ModSetting s[MSET_MAX]; } ModSettings;

static int mset_key_ok(const char *s) {
    size_t n = strlen(s); if (!n || n >= 48) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_' || *s == '.' || *s == '-')) return 0;
    return 1;
}

// Checks one value against the setting. 0 = valid: canon receives the text to store (a bool as 1 / 0, numbers normalised). Otherwise -1 and err says why.
static int mset_validate(const ModSetting *s, const char *text, char *canon, size_t cap, char *err, size_t ecap) {
    char v[MSET_TEXT * 2]; snprintf(v, sizeof v, "%s", text ? text : "");
    char *b = v; while (*b == ' ' || *b == '\t') b++;
    size_t n = strlen(b); while (n && (b[n - 1] == ' ' || b[n - 1] == '\t')) b[--n] = 0;
    if (strpbrk(b, "\r\n")) { snprintf(err, ecap, "%s: a value cannot contain a line break", s->key); return -1; }
    if (strlen(b) >= MSET_TEXT) { snprintf(err, ecap, "%s: the value is longer than %d characters", s->key, MSET_TEXT - 1); return -1; }
    switch (s->type) {
    case MSETT_BOOL: {
        static const char *yes[] = { "1", "true", "on", "yes" }, *no[] = { "0", "false", "off", "no" };
        for (int i = 0; i < 4; i++) { if (!_stricmp(b, yes[i])) { snprintf(canon, cap, "1"); return 0; } if (!_stricmp(b, no[i])) { snprintf(canon, cap, "0"); return 0; } }
        snprintf(err, ecap, "%s: '%.40s' is not a yes/no value (use 1 or 0)", s->key, b); return -1; }
    case MSETT_INT: {
        char *e = NULL; if (!*b) { snprintf(err, ecap, "%s: a whole number is needed", s->key); return -1; }
        long long x = strtoll(b, &e, 10);
        if (*e || (strlen(b) > 18)) { snprintf(err, ecap, "%s: '%.40s' is not a whole number", s->key, b); return -1; }
        if ((s->has_min && (double)x < s->min) || (s->has_max && (double)x > s->max)) { snprintf(err, ecap, "%s: %lld is outside %.0f..%.0f", s->key, x, s->has_min ? s->min : -1e18, s->has_max ? s->max : 1e18); return -1; }
        snprintf(canon, cap, "%lld", x); return 0; }
    case MSETT_FLOAT: {
        char *e = NULL; if (!*b) { snprintf(err, ecap, "%s: a number is needed", s->key); return -1; }
        double x = strtod(b, &e);
        if (*e || !isfinite(x)) { snprintf(err, ecap, "%s: '%.40s' is not a number", s->key, b); return -1; }
        if ((s->has_min && x < s->min) || (s->has_max && x > s->max)) { snprintf(err, ecap, "%s: %g is outside %g..%g", s->key, x, s->has_min ? s->min : -1e300, s->has_max ? s->max : 1e300); return -1; }
        snprintf(canon, cap, "%.9g", x); return 0; }
    case MSETT_CHOICE:
        for (int i = 0; i < s->nchoice; i++) if (!strcmp(b, s->choice[i])) { snprintf(canon, cap, "%s", b); return 0; }
        snprintf(err, ecap, "%s: '%.40s' is not one of the choices", s->key, b); return -1;
    default: snprintf(canon, cap, "%s", b); return 0;
    }
}

// JSON scalar -> setting text (a JSON true/false -> 1/0)
static int mset_scalar(const JDoc *d, int node, char *out, size_t cap) {
    if (node < 0) return 0;
    if (d->n[node].t == J_BOOL) { snprintf(out, cap, "%s", d->n[node].str && !strcmp(d->n[node].str, "True") ? "1" : "0"); return 1; }
    if ((d->n[node].t == J_NUM || d->n[node].t == J_STR) && d->n[node].str) { snprintf(out, cap, "%s", d->n[node].str); return 1; }
    return 0;
}
// Reads the "settings" list of a mod.json. Returns 0 if the list is fine or absent, -1 if the file is not JSON or an entry had to be dropped (err = the first problem);
// the valid entries are kept either way, so one typo does not hide the whole form.
static int mset_parse(const char *json, ModSettings *out, char *err, size_t cap) {
    out->n = 0; if (err && cap) err[0] = 0; if (!json) return 0;
    JDoc d; if (jdoc_parse(&d, json) != 0) { if (err) snprintf(err, cap, "mod.json: %s", d.err); jdoc_free(&d); return -1; }
    int rc = 0, list = d.n[0].t == J_OBJ ? jfind(&d, 0, "settings") : -1;
    if (list < 0) { jdoc_free(&d); return 0; }
    if (d.n[list].t != J_ARR) { if (err) snprintf(err, cap, "mod.json: \"settings\" must be a list"); jdoc_free(&d); return -1; }
#define MSET_BAD(...) do { if (err && !err[0]) snprintf(err, cap, __VA_ARGS__); rc = -1; goto next; } while (0)
    int idx = 0;
    for (int e = d.n[list].first; e >= 0; e = d.n[e].next, idx++) {
        ModSetting s; memset(&s, 0, sizeof s); int k;
        if (d.n[e].t != J_OBJ) MSET_BAD("mod.json: settings entry %d is not an object", idx);
        k = jfind(&d, e, "key"); if (k < 0 || d.n[k].t != J_STR || !mset_key_ok(d.n[k].str)) MSET_BAD("mod.json: settings entry %d needs a \"key\" of letters, digits, _ . -", idx);
        snprintf(s.key, sizeof s.key, "%s", d.n[k].str);
        for (int i = 0; i < out->n; i++) if (!strcmp(out->s[i].key, s.key)) MSET_BAD("mod.json: setting '%s' is listed twice", s.key);
        k = jfind(&d, e, "type"); const char *tn = k >= 0 && d.n[k].t == J_STR ? d.n[k].str : "";
        if (!strcmp(tn, "bool")) s.type = MSETT_BOOL; else if (!strcmp(tn, "int")) s.type = MSETT_INT; else if (!strcmp(tn, "float")) s.type = MSETT_FLOAT;
        else if (!strcmp(tn, "choice")) s.type = MSETT_CHOICE; else if (!strcmp(tn, "text")) s.type = MSETT_TEXT;
        else MSET_BAD("mod.json: setting '%s' has type '%.20s' (bool, int, float, choice or text)", s.key, tn);
        k = jfind(&d, e, "label"); if (k >= 0 && d.n[k].t == J_STR) snprintf(s.label, sizeof s.label, "%s", d.n[k].str); else snprintf(s.label, sizeof s.label, "%s", s.key);
        k = jfind(&d, e, "help"); if (k >= 0 && d.n[k].t == J_STR) snprintf(s.help, sizeof s.help, "%s", d.n[k].str);
        char tmp[64];
        k = jfind(&d, e, "min"); if (k >= 0) { if (!mset_scalar(&d, k, tmp, sizeof tmp) || d.n[k].t != J_NUM) MSET_BAD("mod.json: setting '%s': min must be a number", s.key); s.min = atof(tmp); s.has_min = 1; }
        k = jfind(&d, e, "max"); if (k >= 0) { if (!mset_scalar(&d, k, tmp, sizeof tmp) || d.n[k].t != J_NUM) MSET_BAD("mod.json: setting '%s': max must be a number", s.key); s.max = atof(tmp); s.has_max = 1; }
        if (s.has_min && s.has_max && s.min > s.max) MSET_BAD("mod.json: setting '%s': min is larger than max", s.key);
        if (s.type == MSETT_CHOICE) {
            k = jfind(&d, e, "choices"); if (k < 0 || d.n[k].t != J_ARR) MSET_BAD("mod.json: setting '%s' (choice) needs a \"choices\" list", s.key);
            for (int c = d.n[k].first; c >= 0; c = d.n[c].next) {
                if (d.n[c].t != J_STR || !d.n[c].str[0] || strlen(d.n[c].str) >= 48 || s.nchoice >= MSET_CHOICES) MSET_BAD("mod.json: setting '%s': choices must be 1-47 character texts (at most %d)", s.key, MSET_CHOICES);
                snprintf(s.choice[s.nchoice++], 48, "%s", d.n[c].str);
            }
            if (!s.nchoice) MSET_BAD("mod.json: setting '%s': the choices list is empty", s.key);
        }
        { char raw[MSET_TEXT] = ""; int have = mset_scalar(&d, jfind(&d, e, "default"), raw, sizeof raw);
          if (!have) { if (s.type == MSETT_BOOL) strcpy(raw, "0"); else if (s.type == MSETT_INT || s.type == MSETT_FLOAT) snprintf(raw, sizeof raw, "%g", s.has_min ? s.min : 0.0); else if (s.type == MSETT_CHOICE) snprintf(raw, sizeof raw, "%s", s.choice[0]); }
          char canon[MSET_TEXT * 2], why[200];
          if (mset_validate(&s, raw, canon, sizeof canon, why, sizeof why)) MSET_BAD("mod.json: the default of '%s' is not valid: %s", s.key, why);
          snprintf(s.def, sizeof s.def, "%s", canon); }
        if (out->n >= MSET_MAX) MSET_BAD("mod.json: more than %d settings; the rest are ignored", MSET_MAX);
        out->s[out->n++] = s;
        next:;
    }
#undef MSET_BAD
    jdoc_free(&d); return rc;
}

// ----- plugin.cfg text
static const char *mset_line_value(const char *line, size_t len, const char *key, size_t *vlen) {   // value part of "key = value" if the line sets key
    const char *p = line, *end = line + len; while (p < end && (*p == ' ' || *p == '\t')) p++;
    size_t kn = strlen(key); if (p >= end || *p == '#' || *p == ';' || (size_t)(end - p) < kn || strncmp(p, key, kn)) return NULL;
    p += kn; while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '=') return NULL;
    p++; while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *e = end; while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *vlen = (size_t)(e - p); return p;
}
// value of key in cfg text (the first line that sets it, the one config_get reads; the loader writes a key only once); 1 = found
static int mset_cfg_get(const char *cfg, const char *key, char *out, size_t cap) {
    int found = 0; const char *p = cfg ? cfg : "";
    while (*p) {
        const char *e = strpbrk(p, "\r\n"); size_t n = e ? (size_t)(e - p) : strlen(p), vl;
        const char *v = mset_line_value(p, n, key, &vl);
        if (v && !found) { snprintf(out, cap, "%.*s", (int)vl, v); found = 1; }
        if (!e) break; p = e + 1;
    }
    return found;
}
// Sets key in the cfg text: the first line that sets it is replaced, other lines (comments, other keys) stay as they are; a missing key is appended. Returns the length or -1 if cap is too small.
static int mset_cfg_set(const char *cfg, const char *key, const char *value, char *out, size_t cap) {
    const char *p = cfg ? cfg : ""; size_t w = 0; int done = 0; int crlf = strstr(p, "\r\n") != NULL || !*p;
#define MSET_PUT(s, n) do { if (w + (n) + 1 > cap) return -1; memcpy(out + w, (s), (n)); w += (n); } while (0)
    while (*p) {
        const char *e = strpbrk(p, "\r\n"); size_t n = e ? (size_t)(e - p) : strlen(p), vl; const char *v = mset_line_value(p, n, key, &vl);
        if (v && !done) { char ln[400]; int m = snprintf(ln, sizeof ln, "%s = %s", key, value); MSET_PUT(ln, (size_t)m); done = 1; }
        else MSET_PUT(p, n);
        if (!e) break;
        size_t nl = (e[0] == '\r' && e[1] == '\n') ? 2 : 1; MSET_PUT(e, nl); p = e + nl;
    }
    if (!done) {
        if (w && out[w - 1] != '\n') { if (crlf) MSET_PUT("\r\n", 2); else MSET_PUT("\n", 1); }
        char ln[400]; int m = snprintf(ln, sizeof ln, "%s = %s", key, value); MSET_PUT(ln, (size_t)m); if (crlf) MSET_PUT("\r\n", 2); else MSET_PUT("\n", 1);
    }
    out[w] = 0; return (int)w;
#undef MSET_PUT
}
// Applies edits to the cfg text all-or-nothing: every (key, value) must name a known setting and be valid, otherwise -1, err says which, and out is not touched.
// Keys that already have the default and no line in the file are still written (an explicit value is what the player chose).
static int mset_apply(const ModSettings *set, const char *cfg, int n, const char *const *keys, const char *const *values, char *out, size_t cap, char *err, size_t ecap) {
    char canon[MSET_MAX][MSET_TEXT * 2]; const ModSetting *st[MSET_MAX]; if (n < 0 || n > MSET_MAX) { snprintf(err, ecap, "too many values at once"); return -1; }
    for (int i = 0; i < n; i++) {
        st[i] = NULL; for (int j = 0; j < set->n; j++) if (!strcmp(set->s[j].key, keys[i])) st[i] = &set->s[j];
        if (!st[i]) { snprintf(err, ecap, "'%.40s' is not a setting of this mod", keys[i]); return -1; }
        if (mset_validate(st[i], values[i], canon[i], sizeof canon[i], err, ecap)) return -1;
    }
    static char a[65536], b[65536]; const char *cur = cfg ? cfg : "";
    if (strlen(cur) >= sizeof a - 1024) { snprintf(err, ecap, "plugin.cfg is too large"); return -1; }
    snprintf(a, sizeof a, "%s", cur);
    for (int i = 0; i < n; i++) { if (mset_cfg_set(a, st[i]->key, canon[i], b, sizeof b) < 0) { snprintf(err, ecap, "plugin.cfg would become too large"); return -1; } memcpy(a, b, strlen(b) + 1); }
    if (strlen(a) + 1 > cap) { snprintf(err, ecap, "plugin.cfg would become too large"); return -1; }
    memcpy(out, a, strlen(a) + 1); return (int)strlen(a);
}
#endif

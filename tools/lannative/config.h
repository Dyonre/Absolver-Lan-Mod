// Single settings file: pure text helpers (no I/O).
// LanNative\config.txt holds one "key = value" per line ('#' starts a comment line, keys are case-insensitive, a key may repeat: hostip).
// Without config.txt the game-side code reads one file per setting (parryfix.txt, pvpfix.txt, ...), see setting_get in lannative.c.
#ifndef LANNATIVE_CONFIG_H
#define LANNATIVE_CONFIG_H

#include <stddef.h>
#include <string.h>
#include <ctype.h>

// Next line of text starting at *pp: sets [*b, *e) to the line without CR/LF and moves *pp past it. 0 = no more lines.
static int cfg_next_line(const char **pp, const char **b, const char **e) {
    const char *p = *pp;
    if (!*p) return 0;
    *b = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    *e = p;
    while (*p == '\n' || *p == '\r') p++;
    *pp = p;
    return 1;
}

static void cfg_trim(const char **b, const char **e) {
    while (*b < *e && (**b == ' ' || **b == '\t')) (*b)++;
    while (*e > *b && ((*e)[-1] == ' ' || (*e)[-1] == '\t')) (*e)--;
}

static int cfg_copy(const char *b, const char *e, char *out, size_t cap) {
    size_t n = (size_t)(e - b);
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
    return 1;
}

static const char *cfg_skip_bom(const char *t) {
    return ((unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) ? t + 3 : t;
}

// The nth (0-based) "key = value" line with this key; value trimmed into out. 1 = found, 0 = absent.
static int cfg_get(const char *text, const char *key, int nth, char *out, size_t cap) {
    const char *p = cfg_skip_bom(text), *b, *e;
    size_t klen = strlen(key);
    while (cfg_next_line(&p, &b, &e)) {
        cfg_trim(&b, &e);
        if (b == e || *b == '#') continue;
        const char *eq = b;
        while (eq < e && *eq != '=') eq++;
        if (eq == e) continue;                                   // not a "key = value" line
        const char *kb = b, *ke = eq;
        cfg_trim(&kb, &ke);
        if ((size_t)(ke - kb) != klen) continue;
        size_t i = 0;
        while (i < klen && tolower((unsigned char)kb[i]) == tolower((unsigned char)key[i])) i++;
        if (i != klen) continue;
        if (nth-- > 0) continue;
        const char *vb = eq + 1, *ve = e;
        cfg_trim(&vb, &ve);
        return cfg_copy(vb, ve, out, cap);
    }
    return 0;
}

// One-file-per-setting layout: the nth non-blank, non-'#' line of a single-setting file, trimmed. 1 = found.
static int cfg_nth_line(const char *text, int nth, char *out, size_t cap) {
    const char *p = cfg_skip_bom(text), *b, *e;
    while (cfg_next_line(&p, &b, &e)) {
        cfg_trim(&b, &e);
        if (b == e || *b == '#') continue;
        if (nth-- > 0) continue;
        return cfg_copy(b, e, out, cap);
    }
    return 0;
}


// Returns text with the (first) "key = value" line replaced by "key = value", or the line appended when the key is absent. Comments and other lines are kept as they are.
// out receives the new text (CRLF if the text uses it). 1 = ok, 0 = out too small.
static int cfg_set(const char *text, const char *key, const char *value, char *out, size_t cap) {
    const char *p = cfg_skip_bom(text), *b, *e; size_t w = 0, klen = strlen(key); int done = 0, crlf = strstr(text, "\r\n") != NULL;
    const char *nlz = crlf ? "\r\n" : "\n";
    while (cfg_next_line(&p, &b, &e)) {
        const char *tb = b, *te = e; cfg_trim(&tb, &te); int hit = 0;
        if (!done && tb < te && *tb != '#') {
            const char *eq = tb; while (eq < te && *eq != '=') eq++;
            if (eq < te) { const char *kb = tb, *ke = eq; cfg_trim(&kb, &ke); size_t i = 0; if ((size_t)(ke - kb) == klen) { while (i < klen && tolower((unsigned char)kb[i]) == tolower((unsigned char)key[i])) i++; hit = i == klen; } }
        }
        char line[512]; int n;
        if (hit) { n = snprintf(line, sizeof line, "%s = %s", key, value); done = 1; } else { n = (int)(e - b); if (n >= (int)sizeof line) n = (int)sizeof line - 1; memcpy(line, b, (size_t)n); line[n] = 0; }
        if (n < 0 || w + (size_t)n + 2 >= cap) return 0;
        memcpy(out + w, line, (size_t)n); w += (size_t)n; memcpy(out + w, nlz, strlen(nlz)); w += strlen(nlz);
    }
    if (!done) { char line[512]; int n = snprintf(line, sizeof line, "%s = %s", key, value); if (n < 0 || w + (size_t)n + 2 >= cap) return 0; memcpy(out + w, line, (size_t)n); w += (size_t)n; memcpy(out + w, nlz, strlen(nlz)); w += strlen(nlz); }
    out[w] = 0; return 1;
}

#endif

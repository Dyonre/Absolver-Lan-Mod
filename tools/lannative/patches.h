// Generic runtime patch table: pure text/byte helpers, no Win32.
// LanNative\patches.txt, one patch per line (blank lines and '#' lines ignored):
//     name | target | expected bytes | new bytes | enabled
//   target   = RVA (0x33F46F) or symbol+offset (UComboManager::Foo+0x12; the symbol comes from symbols.bin, offset optional)
//   bytes    = hex, spaces allowed ("48 81 C1 FC 01 00 00"); expected and new have the same length (1..PATCH_MAX bytes)
//   enabled  = on | off
// A patch is applied only when the memory holds exactly the expected bytes (already-new bytes are reported as such, anything else is refused),
// and can be reverted only while it holds exactly the new bytes. The loader applies the table only with config "patches = on" (default OFF).
#ifndef LANNATIVE_PATCHES_H
#define LANNATIVE_PATCHES_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define PATCH_MAX 32
#define PATCH_ROWS 64

typedef struct {
    char name[48];
    char sym[96];          // empty = plain RVA
    uint32_t off;          // the RVA (sym empty) or the offset added to the symbol's address
    uint8_t expect[PATCH_MAX], repl[PATCH_MAX];
    int n;                 // byte count
    int enabled;
    uint32_t rva;          // filled in by the loader once the symbol is resolved
    int state;             // 0 not applied, 1 applied (loader bookkeeping)
} PatchRow;

static const char *patch_trim_b(const char *b, const char *e) { while (b < e && (*b == ' ' || *b == '\t')) b++; return b; }
static const char *patch_trim_e(const char *b, const char *e) { while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--; return e; }

// hex text -> bytes. Returns the byte count, -1 on a bad digit / odd count / overflow.
static int patch_parse_hex(const char *b, const char *e, uint8_t *out, int cap) {
    int n = 0, hi = -1;
    for (; b < e; b++) {
        if (*b == ' ' || *b == '\t') { if (hi >= 0) return -1; continue; }     // a space may not split a byte
        int v = isdigit((unsigned char)*b) ? *b - '0' : (*b >= 'a' && *b <= 'f') ? *b - 'a' + 10 : (*b >= 'A' && *b <= 'F') ? *b - 'A' + 10 : -1;
        if (v < 0) return -1;
        if (hi < 0) hi = v; else { if (n >= cap) return -1; out[n++] = (uint8_t)(hi * 16 + v); hi = -1; }
    }
    return hi >= 0 ? -1 : n;
}

// target text -> sym/off. "0x33F46F" = RVA; "Name::Fn+0x12" = symbol + offset ("+off" optional; offset hex with 0x or decimal).
static int patch_parse_target(const char *b, const char *e, PatchRow *r) {
    char t[128]; size_t len = (size_t)(e - b);
    if (len == 0 || len >= sizeof t) return 0;
    memcpy(t, b, len); t[len] = 0;
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        char *end; unsigned long v = strtoul(t, &end, 16);
        if (*end || v == 0) return 0;
        r->sym[0] = 0; r->off = (uint32_t)v; return 1;
    }
    char *plus = strrchr(t, '+'); r->off = 0;
    if (plus) { char *end; unsigned long v = strtoul(plus + 1, &end, 0); if (*end || plus[1] == 0) return 0; r->off = (uint32_t)v; *plus = 0; }
    size_t sl = strlen(t); while (sl && t[sl - 1] == ' ') t[--sl] = 0;
    if (sl == 0 || sl >= sizeof r->sym || isdigit((unsigned char)t[0])) return 0;   // a bare number is a bad RVA, not a symbol
    for (size_t i = 0; i < sl; i++) if (!isalnum((unsigned char)t[i]) && t[i] != '_' && t[i] != ':' && t[i] != '<' && t[i] != '>' && t[i] != '~') return 0;
    memcpy(r->sym, t, sl + 1); return 1;
}

// One line. 1 = row parsed, 0 = blank/comment, -1 = error (message in err).
static int patch_parse_line(const char *b, const char *e, PatchRow *r, char *err, size_t ecap) {
    b = patch_trim_b(b, e); e = patch_trim_e(b, e);
    if (b >= e || *b == '#') return 0;
    { const char *h = memchr(b, '#', (size_t)(e - b)); if (h) e = patch_trim_e(b, h); }          // trailing comment
    const char *f[5], *fe[5]; int nf = 0;
    const char *s = b;
    for (const char *p = b; p <= e && nf < 5; p++) if (p == e || *p == '|') { f[nf] = s; fe[nf] = p; nf++; s = p + 1; }
    int bars = 0; for (const char *p = b; p < e; p++) if (*p == '|') bars++;
    if (bars != 4 || nf != 5) { snprintf(err, ecap, "expected 5 '|' separated fields (found %d)", bars + 1); return -1; }
    memset(r, 0, sizeof *r);
    for (int i = 0; i < 5; i++) { f[i] = patch_trim_b(f[i], fe[i]); fe[i] = patch_trim_e(f[i], fe[i]); }
    size_t nl = (size_t)(fe[0] - f[0]);
    if (nl == 0 || nl >= sizeof r->name) { snprintf(err, ecap, "bad name"); return -1; }
    memcpy(r->name, f[0], nl);
    if (!patch_parse_target(f[1], fe[1], r)) { snprintf(err, ecap, "bad target '%.*s'", (int)(fe[1] - f[1]), f[1]); return -1; }
    int a = patch_parse_hex(f[2], fe[2], r->expect, PATCH_MAX), c = patch_parse_hex(f[3], fe[3], r->repl, PATCH_MAX);
    if (a <= 0) { snprintf(err, ecap, "bad or empty expected bytes"); return -1; }
    if (c <= 0) { snprintf(err, ecap, "bad or empty new bytes"); return -1; }
    if (a != c) { snprintf(err, ecap, "expected (%d) and new (%d) byte counts differ", a, c); return -1; }
    if (memcmp(r->expect, r->repl, (size_t)a) == 0) { snprintf(err, ecap, "new bytes equal the expected bytes (nothing to patch)"); return -1; }
    r->n = a;
    size_t el = (size_t)(fe[4] - f[4]);
    if (el == 2 && _strnicmp(f[4], "on", 2) == 0) r->enabled = 1;
    else if (el == 3 && _strnicmp(f[4], "off", 3) == 0) r->enabled = 0;
    else { snprintf(err, ecap, "enabled must be on or off"); return -1; }
    return 1;
}

// Whole file -> rows. Bad lines are skipped and counted in *bad (first message in err1). Returns the row count. Duplicate names are bad.
static int patch_parse_file(const char *text, PatchRow *rows, int cap, int *bad, char *err1, size_t ecap) {
    int n = 0; *bad = 0; if (err1 && ecap) err1[0] = 0;
    int lineno = 0;
    while (*text) {
        const char *b = text, *e = text;
        while (*e && *e != '\n') e++;
        text = *e ? e + 1 : e; lineno++;
        char msg[96]; PatchRow r; int rc = patch_parse_line(b, e, &r, msg, sizeof msg);
        if (rc == 0) continue;
        if (rc > 0) { for (int i = 0; i < n; i++) if (_stricmp(rows[i].name, r.name) == 0) { snprintf(msg, sizeof msg, "duplicate name '%s'", r.name); rc = -1; break; } }
        if (rc > 0 && n >= cap) { snprintf(msg, sizeof msg, "more than %d rows", cap); rc = -1; }
        if (rc < 0) { if (*bad == 0 && err1 && ecap) snprintf(err1, ecap, "line %d: %s", lineno, msg); (*bad)++; continue; }
        rows[n++] = r;
    }
    return n;
}

// Two rows overlap when both patch the same RVA range (only meaningful once rva is resolved).
static int patch_overlap(const PatchRow *a, const PatchRow *b) { return a->rva < b->rva + (uint32_t)b->n && b->rva < a->rva + (uint32_t)a->n; }

#define PATCH_APPLIED 1
#define PATCH_ALREADY 2     // memory already holds the new bytes
#define PATCH_MISMATCH 0

static int patch_apply_bytes(uint8_t *mem, const PatchRow *r) {
    if (!mem || r->n <= 0) return PATCH_MISMATCH;
    if (memcmp(mem, r->expect, (size_t)r->n) == 0) { memcpy(mem, r->repl, (size_t)r->n); return PATCH_APPLIED; }
    if (memcmp(mem, r->repl, (size_t)r->n) == 0) return PATCH_ALREADY;
    return PATCH_MISMATCH;
}

// 1 = restored the expected bytes, 0 = memory does not hold the new bytes (left untouched).
static int patch_revert_bytes(uint8_t *mem, const PatchRow *r) {
    if (!mem || r->n <= 0 || memcmp(mem, r->repl, (size_t)r->n) != 0) return 0;
    memcpy(mem, r->expect, (size_t)r->n); return 1;
}

static void patch_hex(const uint8_t *p, int n, char *out, size_t cap) {
    size_t w = 0; out[0] = 0;
    for (int i = 0; i < n && w + 4 < cap; i++) w += (size_t)snprintf(out + w, cap - w, "%s%02X", i ? " " : "", p[i]);
}

#endif

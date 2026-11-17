#ifndef LANNATIVE_JOINGUARD_H
#define LANNATIVE_JOINGUARD_H
// Optional address allow list for a hosting PC (pure logic).
//   allow = a.b.c.d[/bits]   (repeatable; none = anybody who can reach the host may join, the default)
// The host checks it in PreLogin (before the engine creates a player) and the LAN finder (landisc.inc) answers only allowed addresses.
// There is no join key, joiner limit or failed-login lock-out: old config lines with those keys are ignored with a log line (jg_obsolete_keys).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

#define JG_ALLOW_MAX 64
// Fail closed: an allow line that is not valid (bad address, bad /bits, junk, more than JG_ALLOW_MAX lines)
// used to be dropped with a log line - if it was the only one, the host was open to everybody. Now any invalid line refuses EVERY join
// (and every LAN-list answer) until it is fixed. A '#' starts a comment anywhere on the line.

typedef struct { uint32_t net, mask; } JgRule;
typedef struct { JgRule allow[JG_ALLOW_MAX]; int nallow, invalid, bad_line; char bad[80]; } JoinGuard;   // invalid: refuse everybody; bad = the first bad line's text, bad_line its number (1-based)

// "a.b.c.d" or "a.b.c.d/bits" -> rule. 1 = ok
static int jg_parse_rule(const char *s, JgRule *r) {
    unsigned p[4] = { 0, 0, 0, 0 }; int part = 0, digits = 0; const char *q = s; unsigned bits = 32;
    for (;; q++) {
        if (*q >= '0' && *q <= '9') { p[part] = p[part] * 10 + (unsigned)(*q - '0'); if (++digits > 3 || p[part] > 255) return 0; }
        else if ((*q == '.' || *q == '/' || *q == 0) && digits) { digits = 0; if (*q == '.') { if (++part > 3) return 0; } else break; }
        else return 0;
    }
    if (part != 3) return 0;
    if (*q == '/') {                                                  // 1-2 digits, 0..32: no sign, no spaces, no junk ("/-0" used to allow every address)
        const char *d = q + 1; int nd = 0; unsigned b = 0;
        for (; *d >= '0' && *d <= '9' && nd < 3; d++, nd++) b = b * 10 + (unsigned)(*d - '0');
        if (nd == 0 || nd > 2 || *d || b > 32) return 0; bits = b;
    }
    r->mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
    r->net = ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) & r->mask; return 1;
}
// peer address text as the engine gives it: "192.168.1.5:7777", maybe with junk after it; non-IPv4 (IPv6, names) = not parsed
static int jg_parse_addr(const char *s, uint32_t *ip) {
    unsigned p[4] = { 0, 0, 0, 0 }; int part = 0, digits = 0;
    for (const char *q = s;; q++) {
        if (*q >= '0' && *q <= '9') { p[part] = p[part] * 10 + (unsigned)(*q - '0'); if (++digits > 3 || p[part] > 255) return 0; }
        else if (digits && (*q == '.' || *q == ':' || *q == 0 || *q == ' ')) { digits = 0; if (*q == '.') { if (++part > 3) return 0; } else break; }
        else return 0;
    }
    if (part != 3) return 0;
    *ip = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; return 1;
}
// last octet hidden: logs get pasted into forums
static void jg_mask_addr(const char *addr, char *out, size_t cap) {
    uint32_t ip; if (!jg_parse_addr(addr, &ip)) { snprintf(out, cap, "(unparsed)"); return; }
    snprintf(out, cap, "%u.%u.%u.x", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255);
}
// "26.0.0.0/8   # radmin" -> "26.0.0.0/8": the text before a '#', without the blanks around it. 0 = nothing left (an empty allow line)
static int jg_rule_text(const char *in, char *out, size_t cap) {
    size_t n = 0; if (!cap) return 0; out[0] = 0; if (!in) return 0;
    while (*in == ' ' || *in == '\t') in++;
    for (; *in && *in != '#' && n + 1 < cap; in++) out[n++] = *in;
    if (*in && *in != '#') { out[0] = 0; return 0; }                 // longer than the buffer: never a valid rule
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r')) n--;
    out[n] = 0; return n > 0;
}
// adds the value of one "allow =" line (line_no counts the allow lines, 1-based). 0 = the line is not valid: the guard now refuses everybody
static int jg_add_line(JoinGuard *g, const char *value, int line_no) {
    char txt[64]; JgRule r;
    int ok = jg_rule_text(value, txt, sizeof txt) && jg_parse_rule(txt, &r);
    if (ok && g->nallow >= JG_ALLOW_MAX) ok = 0;                      // past the limit: fail closed, never a silent drop
    if (ok) { g->allow[g->nallow++] = r; return 1; }
    if (!g->invalid) { g->invalid = 1; g->bad_line = line_no; snprintf(g->bad, sizeof g->bad, "%.70s%s", value ? value : "", g->nallow >= JG_ALLOW_MAX ? " (more than 64 allow lines)" : ""); }
    return 0;
}
// 1 = joins are checked at all (some allow line exists, valid or not)
static int jg_active(const JoinGuard *g) { return g->nallow > 0 || g->invalid; }
// 1 = this IPv4 address (host order) may join / be answered. No rules = everybody; an invalid allow line = nobody.
static int jg_allowed_ip(const JoinGuard *g, uint32_t ip) {
    if (g->invalid) return 0;
    if (!g->nallow) return 1;
    for (int i = 0; i < g->nallow; i++) if ((ip & g->allow[i].mask) == g->allow[i].net) return 1;
    return 0;
}
// the engine's peer address text; an address that is not IPv4 cannot match a rule (refused only when there are rules)
static int jg_allowed(const JoinGuard *g, const char *addr) {
    uint32_t ip; if (g->invalid) return 0; if (!g->nallow) return 1;
    return addr && jg_parse_addr(addr, &ip) && jg_allowed_ip(g, ip);
}
// Settings that no longer exist. Fills out with "joinkey, maxpeers" (the ones present in config text) and returns how many were found.
static int jg_obsolete_keys(const char *cfg_text, char *out, size_t cap) {
    static const char *const keys[] = { "joinkey", "joinprotect", "maxpeers" };
    int n = 0; if (cap) out[0] = 0;
    for (int i = 0; cfg_text && i < 3; i++) {
        char v[8]; if (!cfg_get(cfg_text, keys[i], 0, v, sizeof v)) continue;
        size_t l = strlen(out); snprintf(out + l, cap > l ? cap - l : 0, "%s%s", n ? ", " : "", keys[i]); n++;
    }
    return n;
}
#endif

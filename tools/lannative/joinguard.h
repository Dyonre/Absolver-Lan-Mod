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

#define JG_ALLOW_MAX 16

typedef struct { uint32_t net, mask; } JgRule;
typedef struct { JgRule allow[JG_ALLOW_MAX]; int nallow; } JoinGuard;

// "a.b.c.d" or "a.b.c.d/bits" -> rule. 1 = ok
static int jg_parse_rule(const char *s, JgRule *r) {
    unsigned p[4] = { 0, 0, 0, 0 }; int part = 0, digits = 0; const char *q = s; unsigned bits = 32;
    for (;; q++) {
        if (*q >= '0' && *q <= '9') { p[part] = p[part] * 10 + (unsigned)(*q - '0'); if (++digits > 3 || p[part] > 255) return 0; }
        else if ((*q == '.' || *q == '/' || *q == 0) && digits) { digits = 0; if (*q == '.') { if (++part > 3) return 0; } else break; }
        else return 0;
    }
    if (part != 3) return 0;
    if (*q == '/') { char *end; long b = strtol(q + 1, &end, 10); if (end == q + 1 || *end || b < 0 || b > 32) return 0; bits = (unsigned)b; }
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
// 1 = this IPv4 address (host order) may join / be answered. No rules = everybody.
static int jg_allowed_ip(const JoinGuard *g, uint32_t ip) {
    if (!g->nallow) return 1;
    for (int i = 0; i < g->nallow; i++) if ((ip & g->allow[i].mask) == g->allow[i].net) return 1;
    return 0;
}
// the engine's peer address text; an address that is not IPv4 cannot match a rule (refused only when there are rules)
static int jg_allowed(const JoinGuard *g, const char *addr) {
    uint32_t ip; if (!g->nallow) return 1;
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

#ifndef LANNATIVE_LANDISC_H
#define LANNATIVE_LANDISC_H
// LAN finder: pure protocol + host list (no sockets, no engine). landisc.inc does the I/O.
//
// A player who looks for games (altar menu open, or F1 > LAN games) sends a QUERY every 2 s to UDP LD_PORT on every adapter's broadcast address
// (so VPN adapters such as Radmin / Hamachi / ZeroTier are reached too), on 255.255.255.255 and, unicast, to the address in ip.txt.
// A PC that hosts (listen server) answers each query with one REPLY sent back to the asking address. Nothing is sent while nobody looks.
//   query: "LNQ1\n" + id=<finder id> + padding to LD_QUERY_LEN bytes. A reply is always shorter than a query, so the finder cannot be used
//          to amplify traffic toward a third party.
//   reply: "LNH1\n" + "key=value\n" lines: id, name (character), map, port, addr (the host's bound address when hostip is set), duel (1 = duel map,
//          joiners use a bare URL), players, ver (loader version), fp + mods (gameplay-mod fingerprint and names). Unknown keys are ignored.
// Every value is cleaned to printable ASCII and length-limited before it is used or shown.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LD_PORT       7778          // game port 7777 + 1 (a firewall rule "UDP 7777-7778" covers both)
#define LD_QUERY_LEN  512
#define LD_REPLY_MAX  480           // < LD_QUERY_LEN
#define LD_HOSTS_MAX  16
#define LD_TTL_MS     7000          // a host that stops answering disappears after this (3 missed queries)
#define LD_QUERY_MS   2000

typedef struct {
    char id[12], name[32], map[48], addr[24], ver[16], mods[160];
    unsigned port; int duel, players, has_fp; uint64_t fp;
} LdInfo;
typedef struct { LdInfo info; uint32_t from; char join[40]; uint64_t seen; } LdHost;
typedef struct { LdHost h[LD_HOSTS_MAX]; int n; } LdList;

static void ld_clean(const char *in, char *out, size_t cap) {         // printable ASCII only, others become '?'; always terminated
    size_t n = 0; if (!cap) return;
    for (; in && *in && n + 1 < cap; in++) { unsigned char c = (unsigned char)*in; out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?'; }
    out[n] = 0;
}
static void ld_ip_text(uint32_t ip, char *out, size_t cap) { snprintf(out, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }
static int ld_ip_parse(const char *s, uint32_t *ip) {                // strict "a.b.c.d" (nothing after it)
    unsigned p[4]; int n = 0; char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%n%c", &p[0], &p[1], &p[2], &p[3], &n, &tail) != 4 || !n || s[n]) return 0;
    for (int i = 0; i < 4; i++) if (p[i] > 255) return 0;
    for (const char *q = s; *q; q++) if (!((*q >= '0' && *q <= '9') || *q == '.')) return 0;
    *ip = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; return 1;
}
// broadcast address of an adapter (host order); 0 = none (loopback, /32 point-to-point links, no mask)
static uint32_t ld_bcast(uint32_t addr, uint32_t mask) {
    if (!addr || !mask || mask == 0xFFFFFFFFu || (addr >> 24) == 127) return 0;
    return (addr & mask) | ~mask;
}

static int ld_build_query(const char *id, char *out, size_t cap) {
    if (cap < LD_QUERY_LEN) return 0;
    memset(out, ' ', LD_QUERY_LEN);
    char head[40]; int n = snprintf(head, sizeof head, "LNQ1\nid=%.10s\n", id ? id : "");
    memcpy(out, head, (size_t)n); out[LD_QUERY_LEN - 1] = '\n';
    return LD_QUERY_LEN;
}
// 1 = a query; id (optional, 12 bytes) receives the finder's id
static int ld_is_query(const char *buf, int n, char *id) {
    if (!buf || n < LD_QUERY_LEN || n > 2048 || memcmp(buf, "LNQ1\n", 5)) return 0;
    if (id) { id[0] = 0; if (!memcmp(buf + 5, "id=", 3)) { char t[12]; int k = 0; for (const char *p = buf + 8; k < 10 && p < buf + n && *p != '\n'; p++) t[k++] = *p; t[k] = 0; ld_clean(t, id, 12); } }
    return 1;
}
static int ld_build_reply(const LdInfo *i, char *out, size_t cap) {
    char name[32], map[48], ver[16], mods[160], addr[24], id[12];
    ld_clean(i->name, name, sizeof name); ld_clean(i->map, map, sizeof map); ld_clean(i->ver, ver, sizeof ver); ld_clean(i->mods, mods, sizeof mods);
    ld_clean(i->addr, addr, sizeof addr); ld_clean(i->id, id, sizeof id);
    char fp[24] = ""; if (i->has_fp) snprintf(fp, sizeof fp, "%016llX", (unsigned long long)i->fp);
    int n = snprintf(out, cap, "LNH1\nid=%s\nname=%s\nmap=%s\nport=%u\naddr=%s\nduel=%d\nplayers=%d\nver=%s\nfp=%s\nmods=%s\n",
                     id, name, map, i->port ? i->port : 7777, addr, i->duel ? 1 : 0, i->players, ver, fp, mods);
    return (n > 0 && (size_t)n < cap && n <= LD_REPLY_MAX) ? n : 0;
}
static int ld_parse_reply(const char *buf, int n, LdInfo *o) {
    memset(o, 0, sizeof *o); o->port = 7777;
    if (!buf || n < 5 || n > 1024 || memcmp(buf, "LNH1\n", 5)) return 0;
    char t[1025]; memcpy(t, buf, (size_t)n); t[n] = 0;
    for (char *line = t + 5; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0; const char *k = line, *v = eq + 1;
            if (!strcmp(k, "id")) ld_clean(v, o->id, sizeof o->id);
            else if (!strcmp(k, "name")) ld_clean(v, o->name, sizeof o->name);
            else if (!strcmp(k, "map")) ld_clean(v, o->map, sizeof o->map);
            else if (!strcmp(k, "ver")) ld_clean(v, o->ver, sizeof o->ver);
            else if (!strcmp(k, "mods")) ld_clean(v, o->mods, sizeof o->mods);
            else if (!strcmp(k, "addr")) { uint32_t ip; if (ld_ip_parse(v, &ip)) ld_ip_text(ip, o->addr, sizeof o->addr); }
            else if (!strcmp(k, "port")) { long p = strtol(v, NULL, 10); o->port = (p > 0 && p < 65536) ? (unsigned)p : 7777; }
            else if (!strcmp(k, "duel")) o->duel = v[0] == '1';
            else if (!strcmp(k, "players")) { long p = strtol(v, NULL, 10); o->players = (p >= 0 && p <= 64) ? (int)p : 0; }
            else if (!strcmp(k, "fp")) { char *end; unsigned long long f = strtoull(v, &end, 16); if (strlen(v) == 16 && !*end) { o->fp = f; o->has_fp = 1; } }
        }
        line = nl ? nl + 1 : NULL;
    }
    return 1;
}
// "address:port" a joiner connects to: the address the host announced (hostip set) or the one the reply came from
static void ld_join_addr(const LdInfo *i, uint32_t from, char *out, size_t cap) {
    char a[24]; if (i->addr[0]) snprintf(a, sizeof a, "%s", i->addr); else ld_ip_text(from, a, sizeof a);
    snprintf(out, cap, "%s:%u", a, i->port ? i->port : 7777);
}
// which reply address to join when one host answers over several networks (LAN + Radmin + ...): lower = better
static int ld_addr_rank(uint32_t ip) {
    if ((ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8) return 0;   // private LAN 10/8, 172.16/12, 192.168/16
    if ((ip >> 16) == 0xA9FE) return 2;                                                // link-local 169.254/16
    if ((ip >> 24) == 127) return 3;                                                   // this PC
    return 1;                                                                          // VPN ranges (Radmin 26/8, Hamachi 25/8, CGNAT 100.64/10, ...)
}
// adds or refreshes a host: same host id (one game answering over several networks) or same join address = same host; the best-ranked
// address is kept. 1 = new in the list, 0 = refreshed
static int ld_list_note(LdList *l, const LdInfo *i, uint32_t from, uint64_t now) {
    char j[40]; ld_join_addr(i, from, j, sizeof j);
    for (int k = 0; k < l->n; k++)
        if ((i->id[0] && !strcmp(l->h[k].info.id, i->id)) || !strcmp(l->h[k].join, j)) {
            if (!strcmp(l->h[k].join, j) || ld_addr_rank(from) < ld_addr_rank(l->h[k].from)) { l->h[k].from = from; snprintf(l->h[k].join, sizeof l->h[k].join, "%s", j); }
            l->h[k].info = *i; l->h[k].seen = now; return 0;
        }
    int slot = l->n;
    if (l->n == LD_HOSTS_MAX) { slot = 0; for (int k = 1; k < l->n; k++) if (l->h[k].seen < l->h[slot].seen) slot = k; }
    else l->n++;
    l->h[slot].info = *i; l->h[slot].from = from; l->h[slot].seen = now; snprintf(l->h[slot].join, sizeof l->h[slot].join, "%s", j);
    return 1;
}
static int ld_list_expire(LdList *l, uint64_t now) {                 // keeps the order of the rest (the menu selection stays on its host)
    int w = 0, gone = 0;
    for (int k = 0; k < l->n; k++) { if (now - l->h[k].seen > LD_TTL_MS) { gone++; continue; } if (w != k) l->h[w] = l->h[k]; w++; }
    l->n = w; return gone;
}
// menu line: "Prospect - MainMap_Main (co-op, 2 players) - 192.168.1.5:7777 - mods match"
static void ld_label(const LdHost *h, int me_has_fp, uint64_t me_fp, char *out, size_t cap) {
    const LdInfo *i = &h->info;
    const char *mods = (!i->has_fp || !me_has_fp) ? "" : i->fp == me_fp ? " - mods match" : " - MODS DIFFER";
    snprintf(out, cap, "%s - %s (%s, %d player%s) - %s%s", i->name[0] ? i->name : "Host", i->map[0] ? i->map : "?", i->duel ? "duel" : "co-op",
             i->players, i->players == 1 ? "" : "s", h->join, mods);
}
// A listen travel ("<map>?listen...") without ?bIsLanMatch gets it appended. The game's net driver is SteamNetDriver: on a PC where Steam is online
// it listens on Steam sockets only unless the URL has bIsLanMatch (UE 4.18 SteamNetDriver::InitListen), and every join by IP (LAN list, ip.txt, VPN)
// times out. 1 = out holds the changed URL, 0 = keep the URL as it is (not a listen travel, already set, or no room).
static int ld_tok_is(const wchar_t *b, const wchar_t *e, const wchar_t *name) {          // option token [b, e) is "name" or "name=..." (case-insensitive)
    size_t n = wcslen(name); if ((size_t)(e - b) < n || _wcsnicmp(b, name, n)) return 0;
    return b + n == e || b[n] == L'=';
}
static int ld_lan_listen_url(const wchar_t *in, wchar_t *out, size_t cap) {
    if (!in || !out || !cap) return 0;
    int listen = 0, lan = 0;
    for (const wchar_t *q = wcschr(in, L'?'); q; q = wcschr(q + 1, L'?')) {
        const wchar_t *b = q + 1, *e = b; while (*e && *e != L'?' && *e != L' ') e++;
        if (ld_tok_is(b, e, L"listen")) listen = 1;
        if (ld_tok_is(b, e, L"bIsLanMatch")) lan = 1;
    }
    if (!listen || lan) return 0;
    size_t n = wcslen(in); if (n + 13 >= cap) return 0;
    memcpy(out, in, n * sizeof(wchar_t)); memcpy(out + n, L"?bIsLanMatch", 13 * sizeof(wchar_t)); return 1;
}
// at most `per_sec` answers per second (a flood of queries costs the host nothing more)
typedef struct { uint64_t window; int n; } LdRate;
static int ld_rate_ok(LdRate *r, uint64_t now, int per_sec) {
    if (now - r->window >= 1000) { r->window = now; r->n = 0; }
    return r->n++ < per_sec;
}
#endif

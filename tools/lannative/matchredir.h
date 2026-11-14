// Matchmaking redirect: pure helpers (no I/O, no Windows calls).
// With "matchmaking = <ip>|auto" in LanNative\config.txt the game's lookups of the two Sloclap service names (the redirector and the mentoring server) are answered with a
// PRIVATE LAN address instead, so a LAN matchmaking backend (a separate program on the host PC) can stand in for Sloclap's servers. Nothing else is changed; without the
// setting (or "off", the default) the game talks to Sloclap's servers unchanged. Only RFC 1918 / loopback / link-local addresses are ever accepted.
#ifndef LANNATIVE_MATCHREDIR_H
#define LANNATIVE_MATCHREDIR_H

#include <stddef.h>
#include <string.h>
#include <ctype.h>

static const char *const mr_names[] = { "pc.redir.live.eu-west-2.absolver.com", "pc.mentoring.b.london.absolver.com" };

// 1 when node is one of the service names (case-insensitive, one trailing dot allowed).
static int mr_name_match(const char *node) {
    if (!node) return 0;
    size_t n = strlen(node);
    if (n && node[n - 1] == '.') n--;
    for (size_t k = 0; k < sizeof mr_names / sizeof mr_names[0]; k++) {
        const char *m = mr_names[k]; size_t i = 0;
        if (strlen(m) != n) continue;
        while (i < n && tolower((unsigned char)node[i]) == m[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

// Parses dotted-quad IPv4 text (optionally followed by ":port" / whitespace, which is ignored) into o[4]. 1 = ok.
static int mr_parse_ip(const char *s, int o[4]) {
    if (!s) return 0;
    int parts = 0, digits = 0, val = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); if (++digits > 3 || val > 255) return 0; }
        else if (*p == '.' || *p == 0 || *p == ':' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '?') {
            if (!digits || parts > 3) return 0;
            o[parts++] = val; digits = 0; val = 0;
            if (*p != '.') break;
        } else return 0;
    }
    return parts == 4;
}

// 1 for 10/8, 172.16/12, 192.168/16, 127/8 and 169.254/16 only.
static int mr_ip_private(const int o[4]) {
    return o[0] == 10 || o[0] == 127 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168) || (o[0] == 169 && o[1] == 254);
}

// 1 when s starts with the word w (case-insensitive) followed by the end of the text, whitespace or '#'.
static int mr_word(const char *s, const char *w) {
    size_t i = 0;
    while (w[i] && tolower((unsigned char)s[i]) == w[i]) i++;
    return !w[i] && (s[i] == 0 || s[i] == ' ' || s[i] == 9 || s[i] == 13 || s[i] == 10 || s[i] == '#');
}

// Decides the redirect address. setting = the config value ("", "off", "auto" or an IPv4), joinline = first line of ip.txt (host or host:port, used by "auto").
// Writes the dotted-quad text into out and returns 1; returns 0 when the redirect is off or the address is not acceptable (why = static reason text).
static int mr_target(const char *setting, const char *joinline, char *out, size_t cap, const char **why) {
    int o[4]; const char *src = setting;
    while (src && (*src == ' ' || *src == '\t')) src++;
    if (!src || !*src || mr_word(src, "off")) { *why = "off"; return 0; }
    if (mr_word(src, "auto")) {
        src = joinline; while (src && (*src == ' ' || *src == '\t')) src++;
        if (!src || !*src) { *why = "'auto' but ip.txt has no address yet"; return 0; }
    }
    if (!mr_parse_ip(src, o)) { *why = "not an IPv4 address"; return 0; }
    if (!mr_ip_private(o)) { *why = "refused: only private LAN addresses are accepted"; return 0; }
    if (cap < 16) { *why = "buffer too small"; return 0; }
    out[0] = 0;
    { char t[20]; int n = 0; for (int i = 0; i < 4; i++) { int v = o[i]; if (i) t[n++] = '.'; if (v >= 100) t[n++] = (char)('0' + v / 100); if (v >= 10) t[n++] = (char)('0' + (v / 10) % 10); t[n++] = (char)('0' + v % 10); } t[n] = 0; memcpy(out, t, (size_t)n + 1); }
    *why = "ok";
    return 1;
}

// Fail-closed guard while the redirect is active.
// The game must not reach Sloclap's real servers while it runs against the LAN backend: every other Sloclap / absolver.com name fails to resolve, and the game's
// socket connect / sendto refuse destinations that are not LAN-like. LAN-like = private ranges, loopback, link-local, carrier-grade NAT (Tailscale), 25/8 and 26/8
// (Hamachi / Radmin VPN), multicast and the limited broadcast address.

// 1 for names that belong to Sloclap's services but are NOT one of the two redirected names (these are refused while the redirect is active).
static int mr_name_block(const char *node) {
    if (!node || mr_name_match(node)) return 0;
    char t[256]; size_t i = 0;
    for (; node[i] && i < sizeof t - 1; i++) t[i] = (char)tolower((unsigned char)node[i]);
    t[i] = 0;
    return strstr(t, "absolver") != NULL || strstr(t, "sloclap") != NULL;
}

static int mr_ip4_lan_ok(const unsigned char a[4]) {
    if (a[0] == 10 || a[0] == 127 || a[0] == 25 || a[0] == 26) return 1;
    if (a[0] == 172 && a[1] >= 16 && a[1] <= 31) return 1;
    if (a[0] == 192 && a[1] == 168) return 1;
    if (a[0] == 169 && a[1] == 254) return 1;
    if (a[0] == 100 && a[1] >= 64 && a[1] <= 127) return 1;      // 100.64/10: Tailscale
    if (a[0] >= 224 && a[0] <= 239) return 1;                    // multicast
    if (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255) return 1;
    if (a[0] == 0) return 1;                                     // 0.0.0.0 (bind / unspecified)
    return 0;
}

static int mr_ip6_lan_ok(const unsigned char a[16]) {
    static const unsigned char zero10[10] = { 0 };
    if (memcmp(a, zero10, 10) == 0 && a[10] == 0xFF && a[11] == 0xFF) return mr_ip4_lan_ok(a + 12);     // ::ffff:a.b.c.d
    { int allzero = 1; for (int i = 0; i < 15; i++) if (a[i]) { allzero = 0; break; } if (allzero && (a[15] == 0 || a[15] == 1)) return 1; }   // :: and ::1
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return 1;         // fe80::/10 link-local
    if ((a[0] & 0xFE) == 0xFC) return 1;                         // fc00::/7 unique local
    if (a[0] == 0xFF) return 1;                                  // multicast
    return 0;
}

// sockaddr (AF_INET = 2: port u16, addr 4 bytes at +4; AF_INET6 = 23: addr 16 bytes at +8). Other families and short buffers are allowed (not an internet address).
static int mr_sockaddr_ok(const unsigned char *sa, int len) {
    if (!sa || len < 2) return 1;
    unsigned fam = (unsigned)sa[0] | ((unsigned)sa[1] << 8);
    if (fam == 2) return len < 8 ? 1 : mr_ip4_lan_ok(sa + 4);
    if (fam == 23) return len < 24 ? 1 : mr_ip6_lan_ok(sa + 8);
    return 1;
}

#endif

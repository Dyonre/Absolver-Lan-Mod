#ifndef LANNATIVE_JOINGUARD_H
#define LANNATIVE_JOINGUARD_H
// Join protection for a hosting PC (pure logic). Without it anybody who can reach UDP 7777 can connect to the game's listen server.
// The host decides in PreLogin, before the engine creates a player, using only the join URL's options and the peer address string the engine passes ("ip:port").
//   joinprotect = key (default) | open      key: a joiner must send ?LNKey=<key>;  open: no key (the other checks below still apply)
//   joinkey     = <4-32 chars A-Z a-z 0-9 - _>   (absent: LanNative creates a random one and keeps it in LanNative\joinkey.txt)
//   allow       = a.b.c.d[/bits]   (repeatable; none = any address)
//   maxpeers    = 4                (joiners at once; 0 = unlimited)
// A peer that fails 5 times within a minute is locked out for 5 minutes (also when it then sends the right key), so the key cannot be guessed by trying.
// The key travels in the clear (UE4 URL options): it keeps strangers out, it does not stop someone who can read the traffic. Never reuse a password here.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define JG_KEY_MIN 4
#define JG_KEY_MAX 32
#define JG_ALLOW_MAX 16
#define JG_LOCK_SLOTS 32
#define JG_FAILS_MAX 5
#define JG_FAIL_WINDOW_MS 60000ull
#define JG_LOCK_MS 300000ull

typedef enum { JG_OK = 0, JG_BAD_KEY, JG_NOT_ALLOWED, JG_FULL, JG_LOCKED } JgVerdict;
typedef struct { uint32_t net, mask; } JgRule;
typedef struct { uint32_t ip; uint64_t first_fail, locked_until; int fails; } JgSlot;
typedef struct {
    int need_key; char key[JG_KEY_MAX + 1];
    JgRule allow[JG_ALLOW_MAX]; int nallow;
    int maxpeers;
    JgSlot slots[JG_LOCK_SLOTS];
} JoinGuard;

static int jg_key_char(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }
static int jg_key_valid(const char *k) { size_t n = k ? strlen(k) : 0; if (n < JG_KEY_MIN || n > JG_KEY_MAX) return 0; for (size_t i = 0; i < n; i++) if (!jg_key_char((unsigned char)k[i])) return 0; return 1; }

// random key, 12 characters from a 32-symbol alphabet (60 bits). rnd = a source of random 32-bit values (rand_s in the game, fixed numbers in tests)
static void jg_make_key(char out[13], int (*rnd)(unsigned *)) {
    static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";           // no 0 O 1 I: easy to read out over voice chat
    for (int i = 0; i < 12; i++) { unsigned v = 0; if (rnd(&v) != 0) v = (unsigned)i * 2654435761u; out[i] = alpha[v & 31]; }
    out[12] = 0;
}

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

static int jg_ct_equal(const char *a, const char *b) {                  // no early exit on the first difference
    size_t la = strlen(a), lb = strlen(b); unsigned char d = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb; for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}
static JgSlot *jg_slot(JoinGuard *g, uint32_t ip, uint64_t now, int create) {
    JgSlot *free_s = NULL, *oldest = &g->slots[0];
    for (int i = 0; i < JG_LOCK_SLOTS; i++) {
        JgSlot *s = &g->slots[i];
        if (s->fails && s->ip == ip) return s;
        if (!s->fails && !free_s) free_s = s;
        if (s->first_fail < oldest->first_fail) oldest = s;
    }
    if (!create) return NULL;
    JgSlot *s = free_s ? free_s : oldest; memset(s, 0, sizeof *s); s->ip = ip; s->first_fail = now; return s;
}
static void jg_note_fail(JoinGuard *g, uint32_t ip, uint64_t now) {
    JgSlot *s = jg_slot(g, ip, now, 1);
    if (!s->fails || now - s->first_fail > JG_FAIL_WINDOW_MS) { s->first_fail = now; s->fails = 0; }
    s->fails++; s->ip = ip;
    if (s->fails >= JG_FAILS_MAX) s->locked_until = now + JG_LOCK_MS;
}
// options: the join URL option string (wide); peers = connections now (the joining one included); now = a millisecond clock.
// A peer address that cannot be read as IPv4 is treated like an unknown peer: it cannot match an allow rule, and it shares one lock-out slot.
static JgVerdict jg_check(JoinGuard *g, const wchar_t *options, int optlen, const char *addr, int peers, uint64_t now) {
    uint32_t ip = 0; int have_ip = jg_parse_addr(addr, &ip);
    JgSlot *s = jg_slot(g, ip, now, 0);
    if (s && s->locked_until && now < s->locked_until) return JG_LOCKED;
    if (s && s->locked_until && now >= s->locked_until) memset(s, 0, sizeof *s);
    if (g->nallow) { int ok = 0; for (int i = 0; have_ip && i < g->nallow; i++) if ((ip & g->allow[i].mask) == g->allow[i].net) ok = 1; if (!ok) { jg_note_fail(g, ip, now); return JG_NOT_ALLOWED; } }
    if (g->maxpeers > 0 && peers > g->maxpeers) return JG_FULL;
    if (g->need_key) {
        char got[JG_KEY_MAX + 2] = ""; int found = 0;
        for (int i = 0; i + 6 <= optlen && !found; i++)
            if ((i == 0 || options[i - 1] == L'?' || options[i - 1] == L'&') && !_wcsnicmp(options + i, L"LNKey=", 6)) {
                int n = 0; for (int k = i + 6; k < optlen && options[k] && options[k] != L'?' && options[k] != L'&' && options[k] != L' ' && n < JG_KEY_MAX + 1; k++) got[n++] = options[k] < 128 ? (char)options[k] : '?';
                got[n] = 0; found = 1;
            }
        if (!found || !jg_ct_equal(got, g->key)) { jg_note_fail(g, ip, now); return JG_BAD_KEY; }
    }
    return JG_OK;
}
static const char *jg_refusal_text(JgVerdict v) {
    switch (v) {
    case JG_BAD_KEY: return "Join refused: the host needs a join key. Set joinkey = <key> in LanNative\\config.txt (ask the host).";
    case JG_NOT_ALLOWED: return "Join refused: your address is not on the host's list.";
    case JG_FULL: return "Join refused: the host is full.";
    case JG_LOCKED: return "Join refused: too many failed attempts. Try again later.";
    default: return "";
    }
}
static const char *jg_verdict_name(JgVerdict v) { return v == JG_OK ? "ok" : v == JG_BAD_KEY ? "wrong or missing key" : v == JG_NOT_ALLOWED ? "address not allowed" : v == JG_FULL ? "host full" : "locked out"; }
// URL suffix the joiner appends: ?LNKey=<key>  (0 if the key is not usable in a URL)
static int jg_join_suffix(const char *key, wchar_t *out, size_t cap) {
    if (!jg_key_valid(key)) return 0;
    wchar_t w[JG_KEY_MAX + 1]; size_t i = 0; for (; key[i]; i++) w[i] = (wchar_t)(unsigned char)key[i]; w[i] = 0;
    return swprintf(out, cap, L"?LNKey=%ls", w) >= 0;
}
#endif

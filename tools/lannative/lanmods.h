#ifndef LANNATIVE_LANMODS_H
#define LANNATIVE_LANMODS_H
// LAN mod check (pure text logic): the joiner sends its gameplay-mod fingerprint as URL options, the host compares
// it in PreLogin and sends its own list back, and both sides show the same warning. "Gameplay" mods = every mod except those whose mod.json says
// "affects_gameplay": false (texture packs and similar); cosmetic mods never cause a warning.
//   join URL suffix : ?LNMods=<16 hex>?LNModL=<name+name+...>?LNVer=<major>.<minor>   (LNVer since 5.4: the host refuses another MAJOR version)
//   host -> joiner  : LNMODS|<16 hex>|<name+name+...>   (sent with the engine's ClientMessage)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "gleam_version.h"

#define LM_NAMES_MAX 240
#define LM_LINE_MAX 100
typedef struct { int present; uint64_t fp; char names[LM_NAMES_MAX]; int ver_major, ver_minor; } LmInfo;   // names: '+'-separated mod folder names; ver_*: LNVer (-1 = not sent)
typedef enum { LM_SAME = 0, LM_DIFFERENT = 1, LM_UNKNOWN = 2 } LmStatus;

// keeps [A-Za-z0-9._-]; everything else becomes '_' (names travel inside URL options, where ? = & space are separators)
static void lm_clean_name(const char *in, char *out, size_t cap) {
    size_t w = 0;
    for (; *in && w + 1 < cap; in++) { unsigned char c = (unsigned char)*in; out[w++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') ? (char)c : '_'; }
    out[w] = 0;
}
// "A", "B" -> "A+B". Names that do not fit are dropped and the list ends with "+..." (the fingerprint still covers everything).
static void lm_names_join(const char *const *names, int n, char *out, size_t cap) {
    size_t w = 0; out[0] = 0;
    for (int i = 0; i < n; i++) {
        char clean[LM_NAMES_MAX]; lm_clean_name(names[i], clean, sizeof clean);
        size_t need = strlen(clean) + (w ? 1 : 0);
        if (w + need + 5 >= cap) { if (w + 4 < cap) { memcpy(out + w, "+...", 5); } return; }
        if (w) out[w++] = '+';
        memcpy(out + w, clean, strlen(clean)); w += strlen(clean); out[w] = 0;
    }
}

static int lm_join_suffix(const LmInfo *me, wchar_t *out, size_t cap) {
    wchar_t names[LM_NAMES_MAX + 1]; size_t i = 0;
    for (; me->names[i] && i < LM_NAMES_MAX; i++) names[i] = (wchar_t)(unsigned char)me->names[i];
    names[i] = 0;
    return swprintf(out, cap, L"?LNMods=%016llX?LNModL=%ls?LNVer=%d.%d", (unsigned long long)me->fp, names, GLEAM_VER_MAJOR, GLEAM_VER_MINOR) >= 0;
}

// finds "Key=" in an options string and copies the value (up to ? & space or the end) into out
static int lm_option(const wchar_t *opt, int len, const wchar_t *key, wchar_t *out, int cap) {
    int klen = (int)wcslen(key);
    for (int i = 0; i + klen + 1 <= len; i++) {
        if ((i == 0 || opt[i - 1] == L'?' || opt[i - 1] == L'&') && _wcsnicmp(opt + i, key, klen) == 0 && opt[i + klen] == L'=') {
            int s = i + klen + 1, e = s;
            while (e < len && opt[e] && opt[e] != L'?' && opt[e] != L'&' && opt[e] != L' ') e++;
            int n = e - s; if (n >= cap) n = cap - 1;
            for (int k = 0; k < n; k++) out[k] = opt[s + k];
            out[n] = 0; return 1;
        }
    }
    return 0;
}
static void lm_wide_to_names(const wchar_t *w, char *out, size_t cap) {
    size_t i = 0; for (; w[i] && i + 1 < cap; i++) out[i] = (w[i] < 128 && w[i] > 32) ? (char)w[i] : '_';
    out[i] = 0;
}
// LNVer value "<major>[.<minor>]": 1-4 digits per part, nothing else. Returns 1 and the numbers, or 0 (= treated as not sent).
static int lm_parse_ver(const wchar_t *v, int *major, int *minor) {
    int a = 0, b = 0, i = 0, d = 0;
    if (!v) return 0;
    for (; v[i] >= L'0' && v[i] <= L'9'; i++) { if (++d > 4) return 0; a = a * 10 + (v[i] - L'0'); }
    if (!d) return 0;
    if (v[i] == L'.') { i++; d = 0; for (; v[i] >= L'0' && v[i] <= L'9'; i++) { if (++d > 4) return 0; b = b * 10 + (v[i] - L'0'); } if (!d) return 0; }
    if (v[i]) return 0;
    *major = a; *minor = b; return 1;
}
// parses the PreLogin options string; present = 1 only when a valid 16-digit fingerprint was found
static int lm_parse_options(const wchar_t *opt, int len, LmInfo *out) {
    memset(out, 0, sizeof *out); out->ver_major = out->ver_minor = -1;
    if (!opt || len <= 0) return 0;
    wchar_t v[LM_NAMES_MAX + 8];
    if (lm_option(opt, len, L"LNVer", v, 16)) { int a, b; if (lm_parse_ver(v, &a, &b)) { out->ver_major = a; out->ver_minor = b; } }
    if (!lm_option(opt, len, L"LNMods", v, 40) || wcslen(v) != 16) return 0;
    uint64_t fp = 0;
    for (int i = 0; i < 16; i++) { wchar_t c = v[i]; int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : -1; if (d < 0) return 0; fp = (fp << 4) | (uint64_t)d; }
    out->present = 1; out->fp = fp;
    if (lm_option(opt, len, L"LNModL", v, LM_NAMES_MAX)) lm_wide_to_names(v, out->names, sizeof out->names);
    return 1;
}

// Loader version rule: joins need the same MAJOR version. A joiner that sends LNVer (5.4+) is compared directly; one that sends LNMods but no LNVer is a
// Gleam Loader 5.0-5.3 (major 5); one that sends neither is a vanilla game or LanNative 4.x and earlier - they cannot be told apart at login, so it is
// allowed and the mod labels decide as before. Returns 1 when the join is refused (text filled); *their_major = the joiner's major (-1 = unknown).
static int lm_ver_refuse(int my_major, const LmInfo *c, char *text, size_t cap, int *their_major) {
    int m = c->ver_major >= 0 ? c->ver_major : c->present ? 5 : -1;
    if (their_major) *their_major = m;
    if (m < 0 || m == my_major) return 0;
    snprintf(text, cap, "Join refused: host loader %d.x, your %s %d.x - %s.", my_major, m <= 4 ? "LanNative" : "Gleam Loader", m,
             m < my_major ? "update your loader" : "the host needs a newer loader");
    return 1;
}
// Joiner side of a refused join: UEngine::HandleNetworkFailure gets the host's PreLogin text (NMT_Failure). type = ENetworkFailure (5 FailureReceived,
// 8 PendingConnectionFailure; 3 / 4 lost / timeout are ordinary disconnects and say nothing). Returns 1 and the line to show, or 0.
static int lm_join_fail_text(int type, const char *err, char *out, size_t cap) {
    if (type != 5 && type != 8) return 0;
    if (!err) err = "";
    if (!strncmp(err, "Join refused", 12)) snprintf(out, cap, "%s", err);
    else if (err[0]) snprintf(out, cap, "Join failed: %s", err);
    else snprintf(out, cap, "Join failed (the host gave no reason).");
    return 1;
}
// Both sides run the same comparison. Nothing to say when neither side has gameplay mods.
static LmStatus lm_compare(const LmInfo *me, const LmInfo *other) {
    if (!other->present) return me->fp == 0 ? LM_SAME : LM_UNKNOWN;       // other side sent no mod info (vanilla game or a loader without the mod check)
    return me->fp == other->fp ? LM_SAME : LM_DIFFERENT;
}

static int lm_has_name(const char *list, const char *name, size_t n) {
    for (const char *p = list; *p;) {
        const char *e = strchr(p, '+'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && _strnicmp(p, name, n) == 0) return 1;
        p += l; if (*p == '+') p++;
    }
    return 0;
}
// names in a that are not in b, ','-joined into out ("" if none)
static void lm_only_in(const char *a, const char *b, char *out, size_t cap) {
    size_t w = 0; out[0] = 0;
    for (const char *p = a; *p;) {
        const char *e = strchr(p, '+'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && !lm_has_name(b, p, l) && w + l + 2 < cap) { if (w) out[w++] = ','; memcpy(out + w, p, l); w += l; out[w] = 0; }
        p += l; if (*p == '+') p++;
    }
}

// Screen text. a_label/b_label: who is who ("Host (you)", "Joiner"). Returns the number of lines (0 = nothing to show).
static int lm_lines(LmStatus st, const LmInfo *a, const char *a_label, const LmInfo *b, const char *b_label, char lines[][LM_LINE_MAX], int max) {
    if (st == LM_SAME || max < 4) return 0;
    int n = 0; char only_a[LM_NAMES_MAX], only_b[LM_NAMES_MAX];
    snprintf(lines[n++], LM_LINE_MAX, st == LM_UNKNOWN ? "MOD CHECK: %s has no mod information (vanilla game or an older LanNative)" : "MOD MISMATCH: you and the %s have different mods", b_label);
    snprintf(lines[n++], LM_LINE_MAX, "%s: %s", a_label, a->names[0] ? a->names : "no gameplay mods");
    if (st == LM_DIFFERENT) snprintf(lines[n++], LM_LINE_MAX, "%s: %s", b_label, b->names[0] ? b->names : "no gameplay mods");
    if (st == LM_DIFFERENT) {
        lm_only_in(a->names, b->names, only_a, sizeof only_a); lm_only_in(b->names, a->names, only_b, sizeof only_b);
        if (only_a[0] && n < max) snprintf(lines[n++], LM_LINE_MAX, "Only you have: %s", only_a);
        if (only_b[0] && n < max) { char who[24]; size_t k = 0; for (; b_label[k] && k < 23; k++) who[k] = (b_label[k] >= 'A' && b_label[k] <= 'Z') ? (char)(b_label[k] + 32) : b_label[k]; who[k] = 0;
                                      snprintf(lines[n++], LM_LINE_MAX, "Only the %s has: %s", who, only_b); }
        if (!only_a[0] && !only_b[0] && n < max) snprintf(lines[n++], LM_LINE_MAX, "Same mods, but a different version or load order");
    }
    if (n < max) snprintf(lines[n++], LM_LINE_MAX, "Fights may not match or the game may misbehave. Cosmetic-only mods do not count.");
    return n;
}

// Host notices (NOTE verb): appends "<name>\t<text>\n" at out + w when the whole line fits (name cut to 95, text to 99 characters), else
// leaves out unchanged. Needs w < cap; returns the new w (always < cap, out stays terminated).
static int lm_note_append(char *out, size_t cap, int w, const char *name, const char *text) {
    if (!out || w < 0 || (size_t)w >= cap) return w;
    int r = snprintf(out + w, cap - (size_t)w, "%.95s\t%.99s\n", name ? name : "?", text ? text : "");
    if (r < 0 || (size_t)r >= cap - (size_t)w) { out[w] = 0; return w; }
    return w + r;
}
static int lm_pack_msg(const LmInfo *host, char *out, size_t cap) { return snprintf(out, cap, "LNMODS|%016llX|%s", (unsigned long long)host->fp, host->names) > 0; }
#define LM_MSG_MIN 24                                                       // "LNMODS|<16 hex>|" with an empty name list
static int lm_unpack_msg(const wchar_t *w, int len, LmInfo *out) {
    memset(out, 0, sizeof *out);
    if (!w || len < LM_MSG_MIN || wcsncmp(w, L"LNMODS|", 7) != 0) return 0;
    uint64_t fp = 0;
    for (int i = 0; i < 16; i++) { wchar_t c = w[7 + i]; int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : -1; if (d < 0) return 0; fp = (fp << 4) | (uint64_t)d; }
    if (w[7 + 16] != L'|') return 0;
    out->present = 1; out->fp = fp;
    wchar_t names[LM_NAMES_MAX]; int k = 0; for (int i = 7 + 17; i < len && w[i] && k < LM_NAMES_MAX - 1; i++) names[k++] = w[i]; names[k] = 0;
    lm_wide_to_names(names, out->names, sizeof out->names);
    return 1;
}
#endif

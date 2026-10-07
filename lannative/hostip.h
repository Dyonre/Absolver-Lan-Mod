// LanNative M4.2 host address choice: pure helpers (no engine calls; tested in test/logic_test.c).
// The game's socket code reads "-MULTIHOME=<ip>" from the process command line when it picks the address a listen server binds to
// (ISocketSubsystem::GetLocalHostAddr, RVA 0xAD7E60+: FParse::Value(FCommandLine::Get(), "MULTIHOME=")). With nothing set it binds every adapter.
// LanNative\hostip.txt lists the addresses a host may bind (one IPv4 per line, "auto" = every adapter); the menu cycles through them, and
// lannative.c writes "-MULTIHOME=<ip>" into the command line buffer only while a "?listen" travel starts.
#ifndef LANNATIVE_HOSTIP_H
#define LANNATIVE_HOSTIP_H

#include <stddef.h>
#include <string.h>
#include <wchar.h>

#define HOSTIP_MAX   8      // candidates kept (plus "auto")
#define HOSTIP_LEN   20     // "255.255.255.255" + NUL, with margin

// dotted-quad IPv4, each part 0-255, no leading junk. 1 = ok.
static int hostip_valid(const char *s) {
    if (!s || !*s) return 0;
    int parts = 0, digits = 0, val = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); if (++digits > 3 || val > 255) return 0; }
        else if (*p == '.' || *p == 0) {
            if (!digits) return 0;
            parts++; digits = 0; val = 0;
            if (*p == 0) break;
        } else return 0;
    }
    return parts == 4;
}

// "auto" (any case) or a valid address; leading/trailing blanks and CR/LF are stripped. 1 = accepted into out (out = "auto" or the address).
static int hostip_clean(const char *line, char *out) {
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;   // UTF-8 BOM (Notepad)
    while (*line == ' ' || *line == '\t') line++;
    char tmp[64]; size_t n = 0;
    while (line[n] && line[n] != '\r' && line[n] != '\n' && n < sizeof tmp - 1) { tmp[n] = line[n]; n++; }
    while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t')) n--;
    tmp[n] = 0;
    if (n == 4 && (tmp[0] | 32) == 'a' && (tmp[1] | 32) == 'u' && (tmp[2] | 32) == 't' && (tmp[3] | 32) == 'o') { strcpy(out, "auto"); return 1; }
    if (!hostip_valid(tmp)) return 0;
    strcpy(out, tmp);
    return 1;
}

// Builds "<orig> -MULTIHOME=<ip>" (ip NULL or "auto" = just orig). 1 = fits in cap wide chars including the NUL, 0 = left untouched.
static int hostip_compose(wchar_t *out, size_t cap, const wchar_t *orig, const char *ip) {
    size_t n = wcslen(orig);
    int add = ip && strcmp(ip, "auto") != 0;
    size_t extra = add ? 12 + strlen(ip) : 0;           // " -MULTIHOME=" is 12 characters
    if (n + extra + 1 > cap) return 0;
    wmemcpy(out, orig, n);
    if (add) {
        static const wchar_t key[] = L" -MULTIHOME=";
        wmemcpy(out + n, key, 12);
        for (size_t i = 0; i < strlen(ip); i++) out[n + 12 + i] = (wchar_t)(unsigned char)ip[i];
    }
    out[n + extra] = 0;
    return 1;
}

// A client travel that starts a listen server ("<map>?listen" anywhere in the URL, case-insensitive).
static int hostip_url_is_listen(const wchar_t *url) {
    if (!url) return 0;
    for (; *url; url++) {
        static const wchar_t k[] = L"?listen";
        size_t i = 0;
        while (i < 7 && url[i] && (wchar_t)(url[i] | 32) == k[i]) i++;   // '|32' lower-cases letters and leaves '?' (0x3F) unchanged
        if (i == 7) return 1;
    }
    return 0;
}

#endif

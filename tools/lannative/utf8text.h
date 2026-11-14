// utf8text.h - UTF-8 <-> UTF-16 for the text that crosses the loader's boundary (rows.json values, property text dumps, FText). Pure C, tested offline (test/utf8text_test.c).
#ifndef LANNATIVE_UTF8TEXT_H
#define LANNATIVE_UTF8TEXT_H
#include <stddef.h>

// Decodes UTF-8 into wide characters (UTF-16 units: code points above U+FFFF become surrogate pairs). Invalid or overlong bytes become U+FFFD, one per bad byte.
// Writes at most cap-1 units plus a NUL; returns the number of units written; *truncated (may be NULL) is set to 1 when the input did not fit.
static int ut8_to_w(const char *s, wchar_t *w, int cap, int *truncated) {
    int n = 0; if (truncated) *truncated = 0; if (cap < 1) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp; int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if (*p >= 0xC2 && *p <= 0xDF) { cp = *p & 0x1F; len = 2; }
        else if (*p >= 0xE0 && *p <= 0xEF) { cp = *p & 0x0F; len = 3; }
        else if (*p >= 0xF0 && *p <= 0xF4) { cp = *p & 0x07; len = 4; }
        else { cp = 0xFFFD; len = 1; }
        if (len > 1) {
            int ok = 1; for (int i = 1; i < len; i++) { if ((p[i] & 0xC0) != 0x80) { ok = 0; break; } cp = (cp << 6) | (p[i] & 0x3F); }
            if (ok && ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000))) ok = 0;      // overlong, out of range, or a lone surrogate
            if (!ok) { cp = 0xFFFD; len = 1; }
        }
        int need = cp >= 0x10000 ? 2 : 1;
        if (n + need > cap - 1) { if (truncated) *truncated = 1; break; }
        if (cp >= 0x10000) { cp -= 0x10000; w[n++] = (wchar_t)(0xD800 + (cp >> 10)); w[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF)); }
        else w[n++] = (wchar_t)cp;
        p += len;
    }
    w[n] = 0; return n;
}

// Encodes up to len wide characters (stops at a NUL) as UTF-8 into s (cap bytes including the NUL; never cuts a character in half). Control characters (< 32) become '?'
// so the result is safe in a log line or a JSON string. Returns the number of bytes written (without the NUL).
static int utw_to_8(const wchar_t *w, int len, char *s, int cap) {
    int n = 0; if (cap < 1) return 0;
    for (int i = 0; i < len && w[i]; i++) {
        unsigned cp = (unsigned)w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)w[i + 1] - 0xDC00); i++; }
        else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;                                  // lone surrogate
        if (cp < 32) cp = '?';
        char b[4]; int k;
        if (cp < 0x80) { b[0] = (char)cp; k = 1; }
        else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
        else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
        else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); k = 4; }
        if (n + k > cap - 1) break;
        for (int j = 0; j < k; j++) s[n++] = b[j];
    }
    s[n] = 0; return n;
}
#endif

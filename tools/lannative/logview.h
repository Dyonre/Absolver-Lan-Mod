// logview.h - the log window's data: the last LV_MAX lines of lannative.log, each with its mod tag and an error flag, and the
// filter (text + "errors only"). Pure C (also compiled as C++ by overlay.cpp); offline test test/logview_test.c.
#ifndef LANNATIVE_LOGVIEW_H
#define LANNATIVE_LOGVIEW_H
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LV_MAX 3000
#define LV_LINE 320
typedef struct { char text[LV_LINE]; char tag[48]; unsigned char err; } LvLine;
typedef struct { LvLine *l; int head, n; char part[LV_LINE]; int partn; unsigned long total; } LogView;   // ring: line i (0 = oldest) = l[(head + i) % LV_MAX]

static int lv_has(const char *s, const char *word) {            // case-insensitive substring
    size_t m = strlen(word);
    for (; *s; s++) { size_t k = 0; while (k < m && s[k] && tolower((unsigned char)s[k]) == word[k]) k++; if (k == m) return 1; }
    return 0;
}
// an error / warning line: what the loader and plugins write when something went wrong ("no faults" and "0 failed" summaries excluded)
static int lv_is_error(const char *line) {
    static const char *const w[] = { "error", "failed", "exception", "warning", "clash", "refused", "faulted", "missing" };
    if (lv_has(line, " 0 failed") || lv_has(line, ", 0 failed") || lv_has(line, "0 error")) return 0;
    for (size_t i = 0; i < sizeof w / sizeof w[0]; i++) if (lv_has(line, w[i])) return 1;
    return 0;
}
// the mod a line names: "plugin <Mod>: ...", "plugin hook <Mod>:<fn>: ...", "binds: <Mod>/...", "keys: <Mod>'s ..."; "" = the loader's own line
static void lv_tag(const char *line, char *tag, size_t cap) {
    tag[0] = 0; const char *s = line;
    while (*s == '[') { const char *e = strchr(s, ']'); if (!e) break; s = e + 1; while (*s == ' ') s++; }   // "[hh:mm:ss.mmm] [pid N] "
    const char *name = NULL; size_t n = 0;
    if (!strncmp(s, "plugin hook ", 12)) { name = s + 12; n = strcspn(name, ": "); }
    else if (!strncmp(s, "plugin ", 7)) { name = s + 7; n = strcspn(name, ": "); }
    else if (!strncmp(s, "binds: ", 7)) { name = s + 7; n = strcspn(name, "/: "); if (name[n] != '/' && name[n] != ':') n = 0; }
    else if (!strncmp(s, "keys: ", 6)) { name = s + 6; const char *ap = strstr(name, "'s "); n = ap && (size_t)(ap - name) < 47 && !memchr(name, ' ', (size_t)(ap - name)) ? (size_t)(ap - name) : 0; }
    if (name && n && n < cap) { memcpy(tag, name, n); tag[n] = 0; }
}
static int lv_init(LogView *v) { memset(v, 0, sizeof *v); v->l = (LvLine *)calloc(LV_MAX, sizeof(LvLine)); return v->l != NULL; }
static void lv_free(LogView *v) { free(v->l); memset(v, 0, sizeof *v); }
static void lv_clear(LogView *v) { v->head = v->n = v->partn = 0; v->total = 0; }
static void lv_push(LogView *v, const char *text, size_t n) {
    while (n && (text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
    if (!n || !v->l) return;
    int at = v->n < LV_MAX ? (v->head + v->n++) % LV_MAX : (v->head++) % LV_MAX; if (v->head >= LV_MAX) v->head -= LV_MAX;
    LvLine *d = &v->l[at]; if (n >= LV_LINE) n = LV_LINE - 1; memcpy(d->text, text, n); d->text[n] = 0;
    for (char *c = d->text; *c; c++) if ((unsigned char)*c < 32) *c = ' ';
    lv_tag(d->text, d->tag, sizeof d->tag); d->err = (unsigned char)lv_is_error(d->text); v->total++;
}
// new bytes of the file (any split): whole lines go in, a partial last line waits for the rest
static void lv_feed(LogView *v, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') { lv_push(v, v->part, (size_t)v->partn); v->partn = 0; continue; }
        if (v->partn < LV_LINE - 1) v->part[v->partn++] = buf[i];
    }
}
static const LvLine *lv_at(const LogView *v, int i) { return i >= 0 && i < v->n ? &v->l[(v->head + i) % LV_MAX] : NULL; }
static int lv_match(const LvLine *l, const char *filter, int errors_only) {
    if (errors_only && !l->err) return 0;
    if (!filter || !filter[0]) return 1;
    char f[64]; size_t k = 0; for (; filter[k] && k < sizeof f - 1; k++) f[k] = (char)tolower((unsigned char)filter[k]); f[k] = 0;
    return lv_has(l->text, f) || lv_has(l->tag, f);
}
#endif

// modes.h - pure logic of the modes API (modes.inc): banner queue, round-end detection from team scores, travel URL building. Offline tests: test\modes_test.c
#ifndef LANNATIVE_MODES_H
#define LANNATIVE_MODES_H
#include <string.h>
#include <stdio.h>
#define MD_BANNERS 8
#define MD_TEAMS 4
typedef struct { char text[160]; float secs; } MdBannerItem;
typedef struct { MdBannerItem q[MD_BANNERS]; int n; int showing; float left; char cur[160]; } MdBanners;
// 1 = queued (or shown at the next tick), 0 = refused (empty text or queue full)
static int mdb_push(MdBanners *b, const char *text, float secs) {
    if (!text || !*text || b->n >= MD_BANNERS) return 0;
    MdBannerItem *it = &b->q[b->n++]; snprintf(it->text, sizeof it->text, "%s", text); it->secs = secs > 0.2f ? (secs > 60 ? 60 : secs) : 0.2f; return 1;
}
// advance time; returns 1 when the shown banner changed: *text = new text (empty string = hide the widget)
static int mdb_tick(MdBanners *b, float dt, const char **text) {
    static const char *const none = "";
    if (b->showing) { b->left -= dt; if (b->left > 0) return 0; b->showing = 0; if (!b->n) { b->cur[0] = 0; *text = none; return 1; } }
    if (b->n) {
        snprintf(b->cur, sizeof b->cur, "%s", b->q[0].text); b->left = b->q[0].secs; memmove(b->q, b->q + 1, sizeof b->q[0] * (size_t)(b->n - 1)); b->n--; b->showing = 1; *text = b->cur; return 1;
    }
    return 0;
}
// round end: a team score went up. Scores that go down (a new match / map) only re-arm the detector. Returns the team that scored (the lowest index if several), or -1
typedef struct { int inited, last[MD_TEAMS], rounds; } MdRounds;
static int mdr_update(MdRounds *r, const int score[MD_TEAMS], int teams) {
    int winner = -1; if (teams > MD_TEAMS) teams = MD_TEAMS;
    if (!r->inited) { memcpy(r->last, score, sizeof r->last); r->inited = 1; r->rounds = 0; for (int i = 0; i < teams; i++) r->rounds += score[i]; return -1; }
    int reset = 0; for (int i = 0; i < teams; i++) if (score[i] < r->last[i]) reset = 1;
    if (!reset) for (int i = 0; i < teams; i++) if (score[i] > r->last[i]) { winner = i; break; }
    memcpy(r->last, score, sizeof r->last); r->rounds = 0; for (int i = 0; i < teams; i++) r->rounds += score[i];
    return winner;
}
// "open <map>?listen": a map object path (/Game/.../Name.Name or without .Name) gets "?listen" unless it has options already; "open ..." / other console text is used as is
static int md_travel_cmd(const char *in, char *out, int cap) {
    if (!in || !*in || cap < 16) return 0; while (*in == ' ') in++;
    if (!_strnicmp(in, "open ", 5) || !_strnicmp(in, "servertravel ", 13)) return snprintf(out, (size_t)cap, "%s", in) < cap;
    char url[400]; snprintf(url, sizeof url, "%s", in); char *q = strchr(url, '?'); char opts[200] = ""; if (q) { snprintf(opts, sizeof opts, "%s", q); *q = 0; }
    if (url[0] == '/') { char *dot = strrchr(url, '.'), *sl = strrchr(url, '/'); if (dot && sl && dot > sl && strlen(dot + 1) == (size_t)(dot - sl - 1) && !strncmp(dot + 1, sl + 1, (size_t)(dot - sl - 1))) *dot = 0; }   // Name.Name -> Name
    return snprintf(out, (size_t)cap, "open %s%s", url, opts[0] ? opts : "?listen") < cap;
}
#endif

#ifndef LANNATIVE_NETINSPECT_H
#define LANNATIVE_NETINSPECT_H
// Net Inspector, loader edition (pure logic; engine / socket / menu glue in netinspect.inc).
// What a joiner got from a modded host: every Character in the world with its body mesh, running anim class and outfit pieces (FightingCharacter
// equipment slots 0-9), flagged NO MESH / NO ANIM / EMPTY OUTFIT, and - with the host's own list - which host assets this PC lacks, which mod
// provides them on the host, and whether the host offers that mod for download (hostmods.inc).
//   joiner -> host on the host-mods side channel (hostmods.inc, same peer binding), one request "NI":
//       "LNNI1 <nchars> <nassets> <role>\n"
//       + nchars lines "C\t<actor>\t<player name>\t<class path>\t<x>\t<y>\t<z>\t<role>\t<fighting 0|1>\t<mesh>\t<anim class>\t<piece0>|...|<piece9>\n"
//       + nassets lines "A\t<object path>\t<provider>\n"
// Providers (where an asset comes from on the machine that has it): "exe" (/Script), "base" (a stock pakchunk), "mod:<Mod>" (a loader mod's pak,
// Mods\<Mod>\ or Mods\_FromHost\<host>\<Mod>\), "pak:<file>" (another pak in Content\Paks), "runtime:<Mod|->" (in memory, in no pak: made at run
// time, e.g. a loader content mod's /Game/Mods/<Mod>/ copy), "" (unknown).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NI_PATH   256
#define NI_SLOTS  10                // EEquipmentSlot 0..9: Mask Elbow Shoulder UnderTop OverTop Gloves Belt Trouser Shoes Hair
#define NI_CHARS  64
#define NI_ASSETS 384
#define NI_PROV   96
#define NI_LINE   96                // one F2 page row (the overlay draws 95 characters; the Mods window shows ~75 at 1280 px: keep headers short)
#define NI_ROWS   40                // rows the inspector adds to the Mods page at most
#define NI_WIRE   4096              // longest wire line
#define NI_MATCH_DIST 1500.0f       // a local character of the host character's class within 15 m = the same character
#define NI_SHOW_CHARS 6             // characters with gaps listed on the page (the report file lists all)
#define NI_SHOW_GAPS  3             // gap rows per character on the page
#define NI_MODS   8                 // distinct providers summarised on the page
enum { NI_ROLE_UNKNOWN = -1, NI_ROLE_NONE = 0, NI_ROLE_SIM = 1, NI_ROLE_AUTO = 2, NI_ROLE_AUTH = 3 };
enum { NIF_NO_MESH = 1, NIF_NO_ANIM = 2, NIF_EMPTY_OUTFIT = 4 };
enum { NIO_UNKNOWN = 0, NIO_OFFERED, NIO_DOWNLOADED, NIO_NOSHARE, NIO_OWN, NIO_LISTING, NIO_DOWNLOADING };   // a host mod as the host-mods offer sees it
enum { NIA_INFO = 0, NIA_REFRESH, NIA_ALL, NIA_INSTALL };                                    // what clicking a page row does
enum { NIV_SOLO = 0, NIV_HOST, NIV_CLIENT };

typedef struct {
    char name[64], player[48], cls[NI_PATH];        // actor object name, PlayerState name ("" = NPC / no player state), class path
    float pos[3]; int role, fighting;               // world position; net role on this machine; 1 = FightingCharacter (has outfit slots)
    char mesh[NI_PATH], anim[NI_PATH], piece[NI_SLOTS][NI_PATH];   // object paths, "" = None
} NiChar;
typedef struct { char path[NI_PATH], prov[NI_PROV]; } NiAsset;
typedef struct { char text[NI_LINE]; int act; char arg[64]; } NiRow;

static const char *const ni_slot_name[NI_SLOTS] = { "mask", "elbow", "shoulder", "undertop", "overtop", "gloves", "belt", "trouser", "shoes", "hair" };

// ----- small text helpers
static void ni_copy(char *out, size_t cap, const char *in) { if (!cap) return; size_t n = in ? strlen(in) : 0; if (n >= cap) n = cap - 1; if (n) memcpy(out, in, n); out[n] = 0; }
// wire field: tabs / newlines / '|' become '_' (they separate fields), non-ASCII control bytes dropped
static void ni_clean(const char *in, char *out, size_t cap) {
    size_t w = 0; for (; in && *in && w + 1 < cap; in++) { unsigned char c = (unsigned char)*in; if (c < 32 && c != '\t' && c != '\r' && c != '\n') continue; out[w++] = (c == '\t' || c == '\r' || c == '\n' || c == '|') ? '_' : (char)c; }
    out[w] = 0;
}
// engine export text of an object / class property -> plain object path ("" for None / empty). "SkeletalMesh'/Game/A/B.B'", "/Game/A/B.B", "Class'\"/Game/X.X_C\"'"
static void ni_objpath(const char *text, char *out, size_t cap) {
    out[0] = 0; if (!text) return;
    const char *p = strchr(text, '/'); if (!p) return;
    size_t n = 0; while (p[n] && p[n] != '\'' && p[n] != '"' && p[n] != ' ' && n + 1 < cap) n++;
    memcpy(out, p, n); out[n] = 0;
}
// "/Game/A/B/SK_Foo.SK_Foo" -> "SK_Foo"; "/Script/Engine.Character" -> "Character"; "/Game/X.Y_C" -> "Y_C"
static const char *ni_short(const char *path) {
    const char *d = strrchr(path, '.'), *s = strrchr(path, '/'), *c = strrchr(path, ':');
    const char *b = d ? d + 1 : s ? s + 1 : path; if (c && c + 1 > b) b = c + 1; return b;
}
// "/Game/Mods/<Mod>/..." -> Mod. 1 = found
static int ni_mod_of_path(const char *path, char *out, size_t cap) {
    if (_strnicmp(path, "/Game/Mods/", 11)) return 0;
    const char *a = path + 11, *e = strchr(a, '/'); if (!e || e == a || (size_t)(e - a) >= cap) return 0;
    memcpy(out, a, (size_t)(e - a)); out[e - a] = 0; return 1;
}
static int ni_role_parse(const char *t) {
    if (!t || !*t) return NI_ROLE_UNKNOWN;
    if (strstr(t, "Authority")) return NI_ROLE_AUTH; if (strstr(t, "AutonomousProxy")) return NI_ROLE_AUTO; if (strstr(t, "SimulatedProxy")) return NI_ROLE_SIM;
    if (strstr(t, "None")) return NI_ROLE_NONE; return NI_ROLE_UNKNOWN;
}
// arrived from the server: on a client a replicated actor is a proxy (map actors that do not replicate have ROLE_None, own spawns ROLE_Authority)
static int ni_from_server(int role) { return role == NI_ROLE_SIM || role == NI_ROLE_AUTO; }

// ----- provider of a pak file (the FPakFile's file name as the engine mounted it): see the header comment
static void ni_prov_from_pak(const char *pakfile, char *out, size_t cap) {
    char seg[8][96]; int ns = 0; const char *p = pakfile ? pakfile : "";
    // split into segments (either slash); keep the last 8
    while (*p) {
        while (*p == '/' || *p == '\\') p++; if (!*p) break;
        size_t n = 0; char tmp[96]; while (p[n] && p[n] != '/' && p[n] != '\\') { if (n + 1 < sizeof tmp) tmp[n] = p[n]; n++; }
        tmp[n < sizeof tmp ? n : sizeof tmp - 1] = 0; p += n;
        if (ns == 8) { memmove(seg[0], seg[1], sizeof seg[0] * 7); ns = 7; }
        ni_copy(seg[ns++], sizeof seg[0], tmp);
    }
    if (!ns) { ni_copy(out, cap, ""); return; }
    const char *file = seg[ns - 1];
    if (ns >= 5 && !_stricmp(seg[ns - 4], "_FromHost") && !_stricmp(seg[ns - 5], "Mods")) { snprintf(out, cap, "mod:%s", seg[ns - 2]); return; }
    if (ns >= 3 && !_stricmp(seg[ns - 3], "Mods") && _stricmp(seg[ns - 2], "_FromHost")) { snprintf(out, cap, "mod:%s", seg[ns - 2]); return; }
    if (!_strnicmp(file, "pakchunk", 8)) { snprintf(out, cap, "base"); return; }
    snprintf(out, cap, "pak:%s", file);
}
// "mod:X" / "runtime:X" -> X (the mod that provides it); 1 = a mod
static int ni_prov_mod(const char *prov, char *mod, size_t cap) {
    if (!strncmp(prov, "mod:", 4)) { ni_copy(mod, cap, prov + 4); return mod[0] != 0; }
    if (!strncmp(prov, "runtime:", 8) && strcmp(prov + 8, "-")) { ni_copy(mod, cap, prov + 8); return mod[0] != 0; }
    return 0;
}
static void ni_prov_text(const char *prov, char *out, size_t cap) {
    if (!prov || !*prov) snprintf(out, cap, "unknown source");
    else if (!strcmp(prov, "exe")) snprintf(out, cap, "game code");
    else if (!strcmp(prov, "base")) snprintf(out, cap, "base game");
    else if (!strncmp(prov, "mod:", 4)) snprintf(out, cap, "mod %s", prov + 4);
    else if (!strncmp(prov, "pak:", 4)) snprintf(out, cap, "pak %s", prov + 4);
    else if (!strncmp(prov, "runtime:", 8)) snprintf(out, cap, strcmp(prov + 8, "-") ? "mod %s (made at run time)" : "made at run time", prov + 8);
    else snprintf(out, cap, "%s", prov);
}

// ----- flags
static int ni_pieces(const NiChar *c) { int n = 0; for (int i = 0; i < NI_SLOTS; i++) n += c->piece[i][0] != 0; return n; }
static int ni_flags(const NiChar *c) {
    int f = 0; if (!c->mesh[0]) f |= NIF_NO_MESH; if (!c->anim[0]) f |= NIF_NO_ANIM; if (c->fighting && !ni_pieces(c)) f |= NIF_EMPTY_OUTFIT;
    return f;
}
static void ni_flags_text(int f, char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s%s", f & NIF_NO_MESH ? "NO MESH " : "", f & NIF_NO_ANIM ? "NO ANIM " : "", f & NIF_EMPTY_OUTFIT ? "EMPTY OUTFIT " : "", f ? "" : "ok");
    size_t n = strlen(out); if (n && out[n - 1] == ' ') out[n - 1] = 0;
}

// ----- wire format
static int ni_format_header(int nc, int na, const char *role, char *out, size_t cap) { char r[32]; ni_clean(role, r, sizeof r); for (char *q = r; *q; q++) if (*q == ' ') *q = '_'; return snprintf(out, cap, "LNNI1 %d %d %s\n", nc, na, r[0] ? r : "-") > 0; }
// -> nchars (0..NI_CHARS), *na (0..NI_ASSETS), role text; -1 = not a header
static int ni_parse_header(const char *line, int *na, char *role, size_t rcap) {
    int nc = -1, a = -1; char r[32] = ""; if (strncmp(line, "LNNI1 ", 6)) return -1;
    if (sscanf(line + 6, "%d %d %31s", &nc, &a, r) != 3 || nc < 0 || nc > NI_CHARS || a < 0 || a > NI_ASSETS) return -1;
    *na = a; ni_copy(role, rcap, r); for (char *q = role; *q; q++) if (*q == '_') *q = ' '; return nc;
}
static int ni_format_char(const NiChar *c, char *out, size_t cap) {
    char f[6][NI_PATH], pieces[NI_SLOTS * (NI_PATH + 1)]; size_t w = 0; pieces[0] = 0;
    ni_clean(c->name, f[0], 64); ni_clean(c->player, f[1], 48); ni_clean(c->cls, f[2], NI_PATH); ni_clean(c->mesh, f[3], NI_PATH); ni_clean(c->anim, f[4], NI_PATH);
    for (int i = 0; i < NI_SLOTS; i++) { char p[NI_PATH]; ni_clean(c->piece[i], p, sizeof p); w += (size_t)snprintf(pieces + w, sizeof pieces - w, "%s%s", i ? "|" : "", p); if (w >= sizeof pieces) w = sizeof pieces - 1; }
    int n = snprintf(out, cap, "C\t%s\t%s\t%s\t%.0f\t%.0f\t%.0f\t%d\t%d\t%s\t%s\t%s\n", f[0], f[1], f[2], c->pos[0], c->pos[1], c->pos[2], c->role, c->fighting ? 1 : 0, f[3], f[4], pieces);
    return n > 0 && (size_t)n < cap;
}
// split on tabs in place; returns the field count (max m)
static int ni_split(char *s, char **f, int m, char sep) { int n = 0; f[n++] = s; for (; *s; s++) if (*s == sep) { *s = 0; if (n < m) f[n++] = s + 1; else return m + 1; } return n; }
static int ni_parse_char(const char *line, NiChar *c, char *why, size_t wcap) {
    memset(c, 0, sizeof *c); static char buf[NI_WIRE + 8]; ni_copy(buf, sizeof buf, line);
    char *f[13]; int n = ni_split(buf, f, 12, '\t');
    if (n != 12 || strcmp(f[0], "C")) { snprintf(why, wcap, "character line has %d fields, expected 12", n); return 0; }
    ni_copy(c->name, sizeof c->name, f[1]); ni_copy(c->player, sizeof c->player, f[2]); ni_copy(c->cls, sizeof c->cls, f[3]);
    for (int i = 0; i < 3; i++) { char *e; double v = strtod(f[4 + i], &e); if (e == f[4 + i] || *e || !isfinite(v)) { snprintf(why, wcap, "position '%.20s' invalid", f[4 + i]); return 0; } c->pos[i] = (float)v; }
    c->role = atoi(f[7]); if (c->role < -1 || c->role > 3) { snprintf(why, wcap, "role %d invalid", c->role); return 0; }
    c->fighting = f[8][0] == '1';
    ni_copy(c->mesh, sizeof c->mesh, f[9]); ni_copy(c->anim, sizeof c->anim, f[10]);
    char *p[NI_SLOTS + 1]; int np = ni_split(f[11], p, NI_SLOTS, '|');
    if (np != NI_SLOTS) { snprintf(why, wcap, "%d outfit slots, expected %d", np, NI_SLOTS); return 0; }
    for (int i = 0; i < NI_SLOTS; i++) ni_copy(c->piece[i], sizeof c->piece[i], p[i]);
    if (!c->cls[0] || c->cls[0] != '/') { snprintf(why, wcap, "class path '%.40s' invalid", c->cls); return 0; }
    return 1;
}
static int ni_format_asset(const NiAsset *a, char *out, size_t cap) { char p[NI_PATH], v[NI_PROV]; ni_clean(a->path, p, sizeof p); ni_clean(a->prov, v, sizeof v); int n = snprintf(out, cap, "A\t%s\t%s\n", p, v); return n > 0 && (size_t)n < cap; }
static int ni_parse_asset(const char *line, NiAsset *a, char *why, size_t wcap) {
    memset(a, 0, sizeof *a); char buf[NI_PATH + NI_PROV + 8]; ni_copy(buf, sizeof buf, line); char *f[4]; int n = ni_split(buf, f, 3, '\t');
    if (n != 3 || strcmp(f[0], "A") || f[1][0] != '/') { snprintf(why, wcap, "asset line invalid (%d fields)", n); return 0; }
    ni_copy(a->path, sizeof a->path, f[1]); ni_copy(a->prov, sizeof a->prov, f[2]); return 1;
}
static const char *ni_asset_prov(const NiAsset *a, int na, const char *path) { for (int i = 0; i < na; i++) if (!strcmp(a[i].path, path)) return a[i].prov; return ""; }
// add path to the table once (prov filled by the caller); returns its index or -1 when full / empty
static int ni_asset_add(NiAsset *a, int *na, const char *path) {
    if (!path || !path[0]) return -1; for (int i = 0; i < *na; i++) if (!strcmp(a[i].path, path)) return i;
    if (*na >= NI_ASSETS) return -1; memset(&a[*na], 0, sizeof a[0]); ni_copy(a[*na].path, NI_PATH, path); return (*na)++;
}

// ----- matching host characters to local ones: players by player name only; others the closest same-class pair within NI_MATCH_DIST first
// (a client names spawned actors on its own, so names only match for level-placed ones), then the same actor name + class at any distance
static float ni_dist(const float *a, const float *b) { float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return sqrtf(dx * dx + dy * dy + dz * dz); }
static void ni_match(const NiChar *loc, int nl, const NiChar *host, int nh, int *map) {
    int used[NI_CHARS] = { 0 };
    for (int h = 0; h < nh; h++) map[h] = -1;
    for (int h = 0; h < nh; h++) if (host[h].player[0]) for (int l = 0; l < nl; l++) if (!used[l] && !strcmp(loc[l].player, host[h].player)) { map[h] = l; used[l] = 1; break; }
    for (;;) {                                                                     // greedy: the globally closest pair first
        int bh = -1, bl = -1; float bd = NI_MATCH_DIST;
        for (int h = 0; h < nh; h++) if (map[h] < 0 && !host[h].player[0])
            for (int l = 0; l < nl; l++) if (!used[l] && !loc[l].player[0] && !strcmp(loc[l].cls, host[h].cls)) { float d = ni_dist(loc[l].pos, host[h].pos); if (d <= bd) { bd = d; bh = h; bl = l; } }
        if (bh < 0) break;
        map[bh] = bl; used[bl] = 1;
    }
    for (int h = 0; h < nh; h++) if (map[h] < 0 && !host[h].player[0]) for (int l = 0; l < nl; l++) if (!used[l] && !loc[l].player[0] && !strcmp(loc[l].name, host[h].name) && !strcmp(loc[l].cls, host[h].cls)) { map[h] = l; used[l] = 1; break; }
}

// ----- the page / report
typedef int (*NiHereFn)(const char *path, char *prov, size_t cap, void *u);   // 1 = this PC has the asset (prov = where it comes from here)
typedef int (*NiOfferFn)(const char *mod, void *u);                           // NIO_* for a host mod
typedef struct {
    int view; const char *host;                     // NIV_*, joined address ("" when not joined)
    const NiChar *loc; int nl;                      // this PC's characters
    const NiChar *hostc; int nh;                    // the host's characters; nh < 0 = no host list (host_note says why)
    const NiAsset *ha; int nha;                     // host asset table (client) or this PC's (solo / host)
    const char *host_note;                          // why there is no host list / the host's state
    int have_me; float me[3];                       // local player position (distances)
    int show_all, asked;                            // "every character" open; host: joiner requests served
    NiHereFn here; NiOfferFn offer; void *u;
} NiView;
typedef struct { char what[24]; char path[NI_PATH]; char prov[NI_PROV]; char here_prov[NI_PROV]; char local[NI_PATH]; int kind; } NiGap;
enum { NIG_MISSING = 1, NIG_UNRESOLVED, NIG_DIFFERS, NIG_OTHERCOPY, NIG_NEVER };

// gaps of host character h (matched to local l or -1). Returns the count (max cap).
static int ni_char_gaps(const NiView *v, const NiChar *h, const NiChar *l, NiGap *g, int cap) {
    int n = 0;
    if (!l) {                                                                          // not here: its class missing here = it can never arrive
        char hp[NI_PROV] = "";
        if (strncmp(h->cls, "/Script/", 8) && !v->here(h->cls, hp, sizeof hp, v->u) && n < cap) {
            memset(&g[n], 0, sizeof g[0]); g[n].kind = NIG_NEVER; ni_copy(g[n].what, sizeof g[n].what, "class"); ni_copy(g[n].path, NI_PATH, h->cls);
            ni_copy(g[n].prov, NI_PROV, ni_asset_prov(v->ha, v->nha, h->cls)); n++;
        }
        return n;
    }
    for (int k = -2; k < NI_SLOTS && n < cap; k++) {
        const char *hv = k == -2 ? h->mesh : k == -1 ? h->anim : h->piece[k], *lv = k == -2 ? l->mesh : k == -1 ? l->anim : l->piece[k];
        if (!hv[0]) continue;
        char hp[NI_PROV] = ""; int have = v->here(hv, hp, sizeof hp, v->u); const char *prov = ni_asset_prov(v->ha, v->nha, hv);
        int kind = 0;
        if (!strcmp(hv, lv)) { if (have && (!strncmp(prov, "mod:", 4) || !strncmp(prov, "pak:", 4)) && strcmp(prov, hp)) kind = NIG_OTHERCOPY; }
        else if (!have) kind = NIG_MISSING;
        else kind = lv[0] ? NIG_DIFFERS : NIG_UNRESOLVED;
        if (!kind) continue;
        memset(&g[n], 0, sizeof g[0]); g[n].kind = kind;
        if (k == -2) snprintf(g[n].what, sizeof g[n].what, "body mesh"); else if (k == -1) snprintf(g[n].what, sizeof g[n].what, "anim"); else snprintf(g[n].what, sizeof g[n].what, "outfit %s", ni_slot_name[k]);
        ni_copy(g[n].path, NI_PATH, hv); ni_copy(g[n].prov, NI_PROV, prov); ni_copy(g[n].here_prov, NI_PROV, hp); ni_copy(g[n].local, NI_PATH, lv); n++;
    }
    return n;
}
static void ni_who(const NiView *v, const NiChar *c, char *out, size_t cap) {
    char d[24] = ""; if (v->have_me) snprintf(d, sizeof d, " (%.0f m)", ni_dist(v->me, c->pos) / 100.0f);
    if (c->player[0]) snprintf(out, cap, "player %s%s", c->player, d); else snprintf(out, cap, "NPC %s%s", ni_short(c->cls), d);
}
static const char *ni_gap_word(int kind) { return kind == NIG_MISSING ? "MISSING" : kind == NIG_UNRESOLVED ? "NOT APPLIED" : kind == NIG_DIFFERS ? "DIFFERS" : kind == NIG_OTHERCOPY ? "OTHER COPY" : "NEVER ARRIVED"; }

#define NI_ROW(act_, arg_, ...) do { if (n < max) { snprintf(rows[n].text, NI_LINE - 3, __VA_ARGS__); rows[n].act = (act_); ni_copy(rows[n].arg, sizeof rows[n].arg, (arg_)); n++; } } while (0)
#define NI_RPT(...) do { if (full) fprintf(full, __VA_ARGS__); } while (0)
// Builds the page rows (max) and, when full != NULL, writes the complete report there. Returns the row count.
static int ni_report(const NiView *v, NiRow *rows, int max, FILE *full) {
    int n = 0; static NiGap g[NI_CHARS][NI_SLOTS + 2]; static int ng[NI_CHARS]; int map[NI_CHARS], gapchars = 0, notfound = 0, flagged = 0;
    char ft[48], ptx[NI_PROV + 24];
    memset(rows, 0, sizeof(NiRow) * (size_t)max);
    int nh = v->nh > 0 ? v->nh : 0;
    if (v->view == NIV_CLIENT && v->nh >= 0) {
        ni_match(v->loc, v->nl, v->hostc, nh, map);
        for (int h = 0; h < nh; h++) { ng[h] = ni_char_gaps(v, &v->hostc[h], map[h] >= 0 ? &v->loc[map[h]] : NULL, g[h], NI_SLOTS + 2); gapchars += ng[h] > 0; notfound += map[h] < 0 && !ng[h]; }
    }
    for (int l = 0; l < v->nl; l++) flagged += ni_flags(&v->loc[l]) != 0;
    // header
    const char *hostname = v->host && v->host[0] ? v->host : "the host";
    if (v->view == NIV_CLIENT) {
        if (v->nh >= 0) NI_ROW(NIA_REFRESH, "", "NET INSPECTOR: joined %s - %d here, host %d, %d with gaps (refresh)", hostname, v->nl, nh, gapchars);
        else NI_ROW(NIA_REFRESH, "", "NET INSPECTOR: joined %s - %d here, %d flagged, no host list (refresh)", hostname, v->nl, flagged);
    } else NI_ROW(NIA_REFRESH, "", "NET INSPECTOR (%s): %d here, %d flagged, %d request(s) (refresh)", v->view == NIV_HOST ? "listen host" : "solo", v->nl, flagged, v->asked);
    NI_RPT("Net Inspector report (loader edition)\r\nview: %s%s%s\r\ncharacters here: %d, flagged: %d\r\n", v->view == NIV_CLIENT ? "joined client" : v->view == NIV_HOST ? "listen host" : "solo",
           v->host && v->host[0] ? ", host " : "", v->host ? v->host : "", v->nl, flagged);
    if (v->view == NIV_CLIENT && v->nh < 0) { NI_ROW(NIA_INFO, "", "  Host list: %s", v->host_note && v->host_note[0] ? v->host_note : "not received"); NI_RPT("host list: %s\r\n", v->host_note ? v->host_note : "not received"); }
    // client: host characters with gaps + the mods behind them
    char mods[NI_MODS][NI_PROV]; int nmods = 0, shown = 0;
    if (v->view == NIV_CLIENT && v->nh >= 0) {
        NI_RPT("host characters: %d, with gaps: %d, not here: %d\r\n", nh, gapchars, notfound);
        for (int h = 0; h < nh; h++) {
            const NiChar *hc = &v->hostc[h]; const NiChar *lc = map[h] >= 0 ? &v->loc[map[h]] : NULL;
            char who[96]; ni_who(v, hc, who, sizeof who);
            ni_flags_text(lc ? ni_flags(lc) : 0, ft, sizeof ft);
            NI_RPT("\r\nhost %s | actor %s | class %s | here: %s%s\r\n", who, hc->name, hc->cls, lc ? lc->name : "(not here)", lc ? "" : "");
            if (lc) NI_RPT("  flags here: %s\r\n", ft);
            for (int k = 0; k < ng[h]; k++) {
                const NiGap *x = &g[h][k]; ni_prov_text(x->prov, ptx, sizeof ptx);
                NI_RPT("  %s %s %s <- host: %s%s%s%s%s\r\n", ni_gap_word(x->kind), x->what, x->path, ptx, x->here_prov[0] ? ", here: " : "", x->here_prov, x->local[0] ? ", here uses " : "", x->local);
                char mod[NI_PROV]; const char *key = ni_prov_mod(x->prov, mod, sizeof mod) ? x->prov : x->prov[0] ? x->prov : "?";
                int dup = 0; for (int m = 0; m < nmods; m++) if (!strcmp(mods[m], key)) dup = 1;
                if (!dup && nmods < NI_MODS) ni_copy(mods[nmods++], NI_PROV, key);
            }
            if (!ng[h] || shown >= NI_SHOW_CHARS) continue;
            shown++;
            NI_ROW(NIA_INFO, "", "  %s: %s", who, !lc ? "not here" : ni_flags(lc) ? ft : "drawn here, but not as on the host");
            for (int k = 0; k < ng[h] && k < NI_SHOW_GAPS; k++) { const NiGap *x = &g[h][k]; ni_prov_text(x->prov, ptx, sizeof ptx); char hu[NI_PATH + 16] = ""; if (x->local[0] && x->kind != NIG_OTHERCOPY) snprintf(hu, sizeof hu, " (here: %s)", ni_short(x->local));
                NI_ROW(NIA_INFO, "", "    %s %s %s <- %s%s", ni_gap_word(x->kind), x->what, ni_short(x->path), ptx, hu); }
            if (ng[h] > NI_SHOW_GAPS) NI_ROW(NIA_INFO, "", "    ... %d more (see the report file)", ng[h] - NI_SHOW_GAPS);
        }
        if (gapchars > shown) NI_ROW(NIA_INFO, "", "  ... %d more character(s) with gaps (see the report file)", gapchars - shown);
        if (notfound) NI_ROW(NIA_INFO, "", "  %d host character(s) not here (out of net range)", notfound);
        // what to do about each source
        for (int m = 0; m < nmods; m++) {
            char mod[NI_PROV]; const char *p = mods[m];
            if (ni_prov_mod(p, mod, sizeof mod)) {
                int st = v->offer ? v->offer(mod, v->u) : NIO_UNKNOWN; const char *rt = !strncmp(p, "runtime:", 8) ? " (made at run time)" : "";
                switch (st) {
                case NIO_OFFERED:    NI_ROW(NIA_INSTALL, mod, "  %s%s: OFFERED by the host - click here to INSTALL it (then restart, rejoin)", mod, rt); break;
                case NIO_DOWNLOADED: NI_ROW(NIA_INFO, "", "  %s%s: downloaded - restart the game and join again to load it", mod, rt); break;
                case NIO_NOSHARE:    NI_ROW(NIA_INFO, "", "  %s%s: not shareable - get it from its author", mod, rt); break;
                case NIO_OWN:        NI_ROW(NIA_INFO, "", "  %s%s: you load %s too, but your copy lacks these assets (other version?)", mod, rt, mod); break;
                case NIO_LISTING:    NI_ROW(NIA_INFO, "", "  %s%s: asking the host which mods it shares ...", mod, rt); break;
                case NIO_DOWNLOADING: NI_ROW(NIA_INFO, "", "  %s%s: downloading from the host ...", mod, rt); break;
                default:             NI_ROW(NIA_INFO, "", "  %s%s: the host runs it; not in its download offer", mod, rt); break;
                }
                NI_RPT("source %s: offer status %d\r\n", p, st);
            } else if (!strncmp(p, "pak:", 4)) { NI_ROW(NIA_INFO, "", "  host pak %s (Content\\Paks, not a loader mod): not offered, ask the host", p + 4); NI_RPT("source %s: a pak outside Mods\\\r\n", p); }
            else { ni_prov_text(p, ptx, sizeof ptx); NI_ROW(NIA_INFO, "", "  from %s: missing here (damaged install, or a pak override on the host)", ptx); NI_RPT("source %s\r\n", p); }
        }
    }
    // characters flagged on this PC (solo / host / client without a host list; a client with a list shows them through the gaps above)
    if (v->view != NIV_CLIENT || v->nh < 0) {
        int k = 0;
        for (int l = 0; l < v->nl && k < NI_SHOW_CHARS; l++) { int f = ni_flags(&v->loc[l]); if (!f) continue; char who[96]; ni_who(v, &v->loc[l], who, sizeof who); ni_flags_text(f, ft, sizeof ft); NI_ROW(NIA_INFO, "", "  %s: %s", who, ft); k++; }
        if (flagged > k) NI_ROW(NIA_INFO, "", "  ... %d more flagged (see the report file)", flagged - k);
        int modded = 0; char mod[NI_PROV];
        for (int a = 0; a < v->nha; a++) if (ni_prov_mod(v->ha[a].prov, mod, sizeof mod) || !strncmp(v->ha[a].prov, "pak:", 4)) modded++;
        if (modded) NI_ROW(NIA_INFO, "", "  %d asset(s) on characters come from mods (joiners without them see gaps)", modded);
    }
    // every character
    NI_ROW(NIA_ALL, "", "Every character here (%d): %s", v->nl, v->show_all ? "click to hide" : "click to show");
    NI_RPT("\r\ncharacters on this PC:\r\n");
    for (int l = 0; l < v->nl; l++) {
        const NiChar *c = &v->loc[l]; char who[96]; ni_who(v, c, who, sizeof who); ni_flags_text(ni_flags(c), ft, sizeof ft);
        char mp[NI_PROV] = "", ap[NI_PROV] = ""; ni_prov_text(ni_asset_prov(v->ha, v->view == NIV_CLIENT ? 0 : v->nha, c->mesh), mp, sizeof mp); ni_prov_text(ni_asset_prov(v->ha, v->view == NIV_CLIENT ? 0 : v->nha, c->anim), ap, sizeof ap);
        if (v->show_all) NI_ROW(NIA_INFO, "", "  %s%s: %s | %s | outfit %d/%d | %s", who, c->role == NI_ROLE_SIM ? " [from host]" : c->role == NI_ROLE_AUTO ? " [you]" : "", c->mesh[0] ? ni_short(c->mesh) : "-", c->anim[0] ? ni_short(c->anim) : "-", ni_pieces(c), c->fighting ? NI_SLOTS : 0, ft);
        NI_RPT("%s | actor %s | class %s | role %d%s | pos %.0f %.0f %.0f | %s\r\n  mesh %s%s%s\r\n  anim %s%s%s\r\n", who, c->name, c->cls, c->role, ni_from_server(c->role) ? " (from the server)" : "",
               c->pos[0], c->pos[1], c->pos[2], ft, c->mesh[0] ? c->mesh : "None", v->view == NIV_CLIENT ? "" : " <- ", v->view == NIV_CLIENT ? "" : mp, c->anim[0] ? c->anim : "None", v->view == NIV_CLIENT ? "" : " <- ", v->view == NIV_CLIENT ? "" : ap);
        for (int i = 0; i < NI_SLOTS; i++) if (c->piece[i][0]) { char pp[NI_PROV]; ni_prov_text(ni_asset_prov(v->ha, v->view == NIV_CLIENT ? 0 : v->nha, c->piece[i]), pp, sizeof pp); NI_RPT("  outfit %s %s%s%s\r\n", ni_slot_name[i], c->piece[i], v->view == NIV_CLIENT ? "" : " <- ", v->view == NIV_CLIENT ? "" : pp); }
    }
    if (v->view != NIV_CLIENT) { NI_RPT("\r\nassets (%d):\r\n", v->nha); for (int a = 0; a < v->nha; a++) { ni_prov_text(v->ha[a].prov, ptx, sizeof ptx); NI_RPT("  %s <- %s\r\n", v->ha[a].path, ptx); } }
    NI_ROW(NIA_INFO, "", "Full report: LanNative\\netinspect.txt (written at each refresh)");
    return n;
}
#undef NI_ROW
#undef NI_RPT
#endif

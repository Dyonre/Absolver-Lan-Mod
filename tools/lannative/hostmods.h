#ifndef LANNATIVE_HOSTMODS_H
#define LANNATIVE_HOSTMODS_H
// Host mod download (pure logic + SHA-256; engine / socket / file glue in hostmods.inc).
//   joiner -> host, own TCP side channel to the address the joiner joined (ip.txt), one request per connection, one line. The host answers
//   only addresses that passed its PreLogin (players in its game), so only the host a player is joined to can be asked:
//       LIST        -> "LNMDL2 <16 hex gameplay-mod fingerprint> <n> <m>\n"
//                      + n lines "<mod>\t<pak>\t<size>\t<sha256 hex>\t<0|1 has Config/*.ini>\t<label>\n"
//                      + m lines "<mod>\t<label>\n" (gameplay mods the host runs but does not share)
//       GET <index> -> "OK <size>\n" + the bytes, or "ERR <text>\n"
// Offered: the .pak files of pak-only mods whose author opted in with "shareable": true in mod.json (default false); never rows.json,
// content\, plugin.dll or Lua. Every other gameplay mod is only named ("not shareable - get it from its author"). The joiner re-checks
// everything (names, sizes, caps, SHA-256, that the bytes are a UE pak, the types of the files inside it) before anything is written.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include "rowsjson.h"
#include "modpak.h"

#define HM_PORT        7779u                       // default TCP port of the side channel (config "moddownload_port"); UDP 7778 is the LAN finder
#define HM_NAME_MAX    64                          // mod folder / pak file name (same limit as the loader's MOD_NAME_MAX)
#define HM_LABEL_MAX   48
#define HM_MAX         32                          // files in one offer
// Limit: fixed caps; a config key when someone ships a pak over 1 GB
#define HM_FILE_CAP    (1024ull * 1024 * 1024)     // largest single pak a joiner accepts
#define HM_TOTAL_CAP   (4ull * 1024 * 1024 * 1024) // largest offer in all
enum { HM_OFF = 0, HM_PROMPT = 1 };

typedef struct { char mod[HM_NAME_MAX], pak[HM_NAME_MAX], label[HM_LABEL_MAX], sha[65]; uint64_t size; int ini; } HmEntry;
typedef struct { char mod[HM_NAME_MAX], label[HM_LABEL_MAX]; } HmNoShare;       // a gameplay mod the host runs but may not share

// mod.json text -> 1 only when it says "shareable": true (the author's opt-in to host-to-joiner sharing; missing / false / bad JSON = 0)
static int hm_manifest_shareable(const char *json) {
    if (!json) return 0;
    JDoc d; int r = 0;
    if (jdoc_parse(&d, json) == 0 && d.count > 0 && d.n[0].t == J_OBJ) { int c = jfind(&d, 0, "shareable"); r = c >= 0 && d.n[c].t == J_BOOL && d.n[c].str && !strcmp(d.n[c].str, "True"); }
    jdoc_free(&d); return r;
}

// "off" -> HM_OFF; anything else (prompt, empty, unknown) -> HM_PROMPT (the default)
static int hm_parse_mode(const char *v) { return v && !_strnicmp(v, "off", 3) ? HM_OFF : HM_PROMPT; }

// Mod folder / pak names: [A-Za-z0-9._-], 1..63 chars, no leading '.', no "..": they become path parts on the joiner.
static int hm_name_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= HM_NAME_MAX || s[0] == '.' || strstr(s, "..")) return 0;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0; }
    static const char *const dev[] = { "CON", "PRN", "AUX", "NUL", "COM", "LPT" };          // Windows device names
    for (int i = 0; i < 6; i++) if (!_strnicmp(s, dev[i], 3) && (s[3] == 0 || s[3] == '.' || (i >= 4 && s[3] >= '0' && s[3] <= '9'))) return 0;
    return 1;
}
static int hm_pak_name_ok(const char *s) { size_t n = s ? strlen(s) : 0; return hm_name_ok(s) && n > 4 && !_stricmp(s + n - 4, ".pak"); }
static int hm_hex_ok(const char *s, size_t n) { if (strlen(s) != n) return 0; for (size_t i = 0; i < n; i++) { char c = s[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; } return 1; }

// label: printable ASCII only, tabs / newlines dropped (it travels inside a tab-separated line and is drawn on screen)
static void hm_clean_label(const char *in, char *out, size_t cap) {
    size_t w = 0; for (; in && *in && w + 1 < cap; in++) if ((unsigned char)*in >= 32 && (unsigned char)*in < 127) out[w++] = *in;
    out[w] = 0;
}


// ----- the LIST answer
static int hm_format_line(const HmEntry *e, char *out, size_t cap) {
    return snprintf(out, cap, "%s\t%s\t%llu\t%s\t%d\t%s\n", e->mod, e->pak, (unsigned long long)e->size, e->sha, e->ini ? 1 : 0, e->label) > 0;
}
// one line (without the newline) -> entry, every field validated. 1 = ok; why = the reason otherwise.
static int hm_parse_line(const char *line, HmEntry *e, char *why, size_t wcap) {
    char f[6][128]; int nf = 0; size_t n = 0; memset(e, 0, sizeof *e);
    for (const char *p = line;; p++) {
        if (*p == '\t' || *p == 0) { if (nf >= 6) { snprintf(why, wcap, "too many fields"); return 0; } f[nf][n] = 0; nf++; n = 0; if (!*p) break; }
        else if (n < sizeof f[0] - 1) f[nf < 6 ? nf : 5][n++] = *p;
        else { snprintf(why, wcap, "field too long"); return 0; }
    }
    if (nf != 6) { snprintf(why, wcap, "%d fields, expected 6", nf); return 0; }
    if (!hm_name_ok(f[0])) { snprintf(why, wcap, "mod name '%.40s' not allowed", f[0]); return 0; }
    if (!hm_pak_name_ok(f[1])) { snprintf(why, wcap, "file name '%.40s' is not a plain .pak name", f[1]); return 0; }
    char *end; unsigned long long sz = _strtoui64(f[2], &end, 10);
    if (f[2][0] < '0' || f[2][0] > '9' || end == f[2] || *end || sz < 44) { snprintf(why, wcap, "size '%.20s' invalid", f[2]); return 0; }
    if (!hm_hex_ok(f[3], 64)) { snprintf(why, wcap, "SHA-256 not 64 lower-case hex digits"); return 0; }
    if (strcmp(f[4], "0") && strcmp(f[4], "1")) { snprintf(why, wcap, "config flag '%.8s' invalid", f[4]); return 0; }
    strcpy(e->mod, f[0]); strcpy(e->pak, f[1]); e->size = sz; strcpy(e->sha, f[3]); e->ini = f[4][0] == '1'; hm_clean_label(f[5], e->label, sizeof e->label);
    return 1;
}
static int hm_format_header(uint64_t fp, int n, int m, char *out, size_t cap) { return snprintf(out, cap, "LNMDL2 %016llX %d %d\n", (unsigned long long)fp, n, m) > 0; }
// "LNMDL2 <16 hex fp> <n> <m>" -> n files (0..HM_MAX), *fp, *m not-shared mods (0..HM_MAX); -1 = not a valid header
static int hm_parse_header(const char *line, uint64_t *fp, int *m) {
    if (strncmp(line, "LNMDL2 ", 7)) return -1;
    const char *p = line + 7; long v[2]; uint64_t f = 0;
    for (int k = 0; k < 16; k++, p++) { char c = *p; int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; if (d < 0) return -1; f = (f << 4) | (uint64_t)d; }
    if (*p++ != ' ') return -1;
    *fp = f;
    for (int i = 0; i < 2; i++) {
        if (*p < '0' || *p > '9') return -1;
        char *end; v[i] = strtol(p, &end, 10); if (v[i] > HM_MAX) return -1;
        p = end; if (i == 0) { if (*p != ' ') return -1; p++; }
    }
    if (*p) return -1;
    *m = (int)v[1]; return (int)v[0];
}
static int hm_format_noshare(const HmNoShare *s, char *out, size_t cap) { return snprintf(out, cap, "%s\t%s\n", s->mod, s->label) > 0; }
// "<mod>\t<label>" -> s. 1 = ok
static int hm_parse_noshare(const char *line, HmNoShare *s, char *why, size_t wcap) {
    memset(s, 0, sizeof *s); const char *tab = strchr(line, '\t');
    if (!tab || strchr(tab + 1, '\t') || tab - line >= HM_NAME_MAX) { snprintf(why, wcap, "not-shared line is not '<mod> TAB <label>'"); return 0; }
    memcpy(s->mod, line, (size_t)(tab - line)); s->mod[tab - line] = 0;
    if (!hm_name_ok(s->mod)) { snprintf(why, wcap, "mod name '%.40s' not allowed", s->mod); return 0; }
    hm_clean_label(tab + 1, s->label, sizeof s->label); return 1;
}
// caps over the entries the joiner would download (want[i] = 1). 1 = within caps; why says which cap otherwise.
static int hm_caps_ok(const HmEntry *e, const int *want, int n, uint64_t *total, char *why, size_t wcap) {
    uint64_t t = 0;
    for (int i = 0; i < n; i++) {
        if (!want[i]) continue;
        if (e[i].size > HM_FILE_CAP) { snprintf(why, wcap, "%s/%s is %llu MB, over the %llu MB per-file limit", e[i].mod, e[i].pak, (unsigned long long)(e[i].size >> 20), (unsigned long long)(HM_FILE_CAP >> 20)); return 0; }
        t += e[i].size;
    }
    *total = t;
    if (t > HM_TOTAL_CAP) { snprintf(why, wcap, "the offer is %llu MB, over the %llu MB limit", (unsigned long long)(t >> 20), (unsigned long long)(HM_TOTAL_CAP >> 20)); return 0; }
    return 1;
}

// ----- what may be inside a downloaded pak (paths as modpak.h gives them, mount-relative, '/')
// Limit: allow-list by type of the packed files; extend it when a real mod needs another data type
static const char *const hm_allowed_ext[] = { "uasset", "uexp", "ubulk", "uptnl", "umap", "ini", "locres", "locmeta", "ushaderbytecode", "ushadercode",
    "upipelinecache", "bin", "ufont", "ttf", "otf", "bnk", "wem", "json", "txt", "csv", "png", "jpg", "tga", "bmp", "wav", "ogg", "mp4", "bk2", "usf", "ush", NULL };
static int hm_entry_type_ok(const char *path) {
    const char *slash = strrchr(path, '/'), *base = slash ? slash + 1 : path, *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return 0;
    for (int i = 0; hm_allowed_ext[i]; i++) if (!_stricmp(dot + 1, hm_allowed_ext[i])) return 1;
    return 0;
}
// a packed file that changes the game's configuration: .../Config/*.ini (DefaultEngine.ini, DefaultGame.ini, ...)
static int hm_entry_is_config(const char *path) {
    size_t n = strlen(path);
    if (n < 5 || _stricmp(path + n - 4, ".ini")) return 0;
    for (const char *p = path; (p = strchr(p, '/')) != NULL; p++) if (!_strnicmp(p + 1, "Config/", 7)) return 1;
    return !_strnicmp(path, "Config/", 7);
}
// 1 = every packed file has an allowed type. *has_ini = some file is Config/*.ini. why names the first bad file.
static int hm_pak_entries_ok(const char *const *paths, int n, int *has_ini, char *why, size_t wcap) {
    *has_ini = 0;
    for (int i = 0; i < n; i++) {
        if (!hm_entry_type_ok(paths[i])) { snprintf(why, wcap, "packed file '%.60s' is not a game data type", paths[i]); return 0; }
        if (hm_entry_is_config(paths[i])) *has_ini = 1;
    }
    return 1;
}
// the file itself: executables are refused whatever they are called (the pak check after this needs the UE footer magic too)
static int hm_head_is_executable(const unsigned char *b, size_t n) {
    return (n >= 2 && b[0] == 'M' && b[1] == 'Z') || (n >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') || (n >= 2 && b[0] == '#' && b[1] == '!');
}

// A pak file as a stream (the joiner's downloaded .part, the host's offered file): 1 = passes. Refused: an executable, no UE pak footer, an
// unreadable index, ANY packed file the path rules drop ("..", mounted above the game folders, path too long: those
// used to be skipped, so their types were never checked), any file that is not a game data type. why names the reason. *ini, ih as hm_pak_entries_ok.
static int hm_check_pak_stream(FILE *f, int *ini, unsigned char ih[20], char *why, size_t wcap) {
    unsigned char head[4] = { 0 }; size_t hn; *ini = 0;
    if (!f || _fseeki64(f, 0, SEEK_SET)) { snprintf(why, wcap, "cannot be read"); return 0; }
    hn = fread(head, 1, 4, f);
    if (hm_head_is_executable(head, hn)) { snprintf(why, wcap, "is an executable, not a pak"); return 0; }
    if (modpak_footer_ok(f) != 1) { snprintf(why, wcap, "is not a UE pak (no valid footer)"); return 0; }
    PakEntry *ents = NULL; int rejected = 0; char bad[260];
    int ne = modpak_read_ex(f, &ents, ih, &rejected, bad, sizeof bad);
    if (ne < 0) { snprintf(why, wcap, "pak index is not readable (encrypted, unsupported, or a path over 511 characters)"); return 0; }
    if (rejected) { snprintf(why, wcap, "%d packed path(s) leave the game folders or are too long (first: '%.70s')", rejected, bad); free(ents); return 0; }
    const char **paths = malloc(sizeof(char *) * (size_t)(ne ? ne : 1)); int ok = paths != NULL;
    if (ok) { for (int i = 0; i < ne; i++) paths[i] = ents[i].path; ok = hm_pak_entries_ok(paths, ne, ini, why, wcap); }
    else snprintf(why, wcap, "out of memory");
    free(paths); free(ents); return ok;
}

// ----- host: who may ask. Addresses that passed PreLogin, kept HM_PEER_MS after their last login.
// Limit: a time window, not the live connection list (the server thread must not read engine objects); 2 h covers a play session
#define HM_PEER_MS (2ull * 60 * 60 * 1000)
#define HM_PEERS 16
typedef struct { uint32_t ip[HM_PEERS]; uint64_t at[HM_PEERS]; } HmPeers;
static void hm_peer_add(HmPeers *p, uint32_t ip, uint64_t now) {
    int slot = 0;
    for (int i = 0; i < HM_PEERS; i++) { if (p->ip[i] == ip && p->at[i]) { slot = i; break; } if (p->at[i] < p->at[slot]) slot = i; }   // same address, else the oldest / an empty slot
    p->ip[slot] = ip; p->at[slot] = now ? now : 1;
}
static int hm_peer_ok(const HmPeers *p, uint32_t ip, uint64_t now) {
    for (int i = 0; i < HM_PEERS; i++) if (p->at[i] && p->ip[i] == ip && now - p->at[i] <= HM_PEER_MS) return 1;
    return 0;
}

// folder name for a host address: "192.168.1.5" / "192.168.1.5:7777" -> "192.168.1.5"
static void hm_host_key(const char *addr, char *out, size_t cap) {
    size_t w = 0; for (; addr && *addr && *addr != ':' && *addr != '/' && w + 1 < cap; addr++) { char c = *addr; out[w++] = ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-') ? c : '_'; }
    out[w] = 0; if (!w || !hm_name_ok(out)) snprintf(out, cap, "host");
}

// ----- prompt text. want[i] = offered; have_dl[i] = already downloaded earlier (no transfer needed). Returns the line count.
#define HM_LINE 100
static void hm_size_text(uint64_t b, char *out, size_t cap) {
    if (b >= 1024ull * 1024) snprintf(out, cap, "%.1f MB", (double)b / (1024.0 * 1024.0)); else snprintf(out, cap, "%llu KB", (unsigned long long)((b + 1023) / 1024));
}
// ns / nns: gameplay mods the host runs and the joiner lacks that the host may not share (their authors did not mark them shareable)
static int hm_prompt_lines(const char *host, const HmEntry *e, const int *want, const int *have_dl, int n, uint64_t total, const HmNoShare *ns, int nns,
                           char (*lines)[HM_LINE], int max) {
    int k = 0, cnt = 0, ini = 0; for (int i = 0; i < n; i++) if (want[i]) { cnt++; ini |= e[i].ini; }
    if ((!cnt && !nns) || max < 3) return 0;
    char sz[24]; hm_size_text(total, sz, sizeof sz);
    if (cnt) snprintf(lines[k++], HM_LINE, "HOST MODS: %s has %d mod file(s) you do not have (%s to download)", host, cnt, sz);
    else snprintf(lines[k++], HM_LINE, "HOST MODS: %s runs %d mod(s) you do not have; none may be shared", host, nns);
    for (int i = 0; i < n && k < max - 2; i++) {
        if (!want[i]) continue;
        char s[24]; hm_size_text(e[i].size, s, sizeof s);
        snprintf(lines[k++], HM_LINE, "  %s/%s  %s%s%s%s%s", e[i].mod, e[i].pak, s, e[i].label[0] ? "  '" : "", e[i].label, e[i].label[0] ? "'" : "",
                 e[i].ini ? "  CHANGES GAME CONFIG" : have_dl[i] ? "  (downloaded before)" : "");
    }
    for (int i = 0; i < nns && k < max - 2; i++)
        snprintf(lines[k++], HM_LINE, "  %s%s%s%s: not shareable - get it from its author", ns[i].mod, ns[i].label[0] ? " '" : "", ns[i].label, ns[i].label[0] ? "'" : "");
    if (ini && k < max - 1) snprintf(lines[k++], HM_LINE, "CONFIG: marked files carry Config/*.ini that override the game's settings");
    snprintf(lines[k++], HM_LINE, cnt ? "F2 > Mods: Install or Skip. Offered mods are marked shareable by their authors."
                                      : "Nothing to download: only mods their authors mark shareable can travel.");
    return k;
}

// ----- SHA-256 (Windows CNG). Incremental: hm_sha_begin / hm_sha_add / hm_sha_end (hex, lower case).
typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } HmSha;
static int hm_sha_begin(HmSha *s) {
    memset(s, 0, sizeof *s);
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    if (BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0) < 0) { BCryptCloseAlgorithmProvider(s->alg, 0); s->alg = NULL; return 0; }
    return 1;
}
static int hm_sha_add(HmSha *s, const void *p, size_t n) {
    while (n) { ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n; if (BCryptHashData(s->h, (PUCHAR)p, k, 0) < 0) return 0; p = (const uint8_t *)p + k; n -= k; }
    return 1;
}
static int hm_sha_end(HmSha *s, char hex[65]) {
    unsigned char d[32]; int ok = s->h && BCryptFinishHash(s->h, d, 32, 0) >= 0;
    if (s->h) BCryptDestroyHash(s->h); if (s->alg) BCryptCloseAlgorithmProvider(s->alg, 0); memset(s, 0, sizeof *s);
    if (!ok) { hex[0] = 0; return 0; }
    for (int i = 0; i < 32; i++) { hex[2 * i] = "0123456789abcdef"[d[i] >> 4]; hex[2 * i + 1] = "0123456789abcdef"[d[i] & 15]; }
    hex[64] = 0; return 1;
}
// whole file; size_out optional. 1 = ok
static int hm_sha_file(const wchar_t *path, char hex[65], uint64_t *size_out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HmSha s; uint64_t total = 0; int ok = hm_sha_begin(&s);
    static unsigned char buf[1 << 16]; DWORD got;                    // Limit: static buffer, one hashing thread at a time (callers hold g_hm_cs or run on the one worker)
    while (ok && ReadFile(f, buf, sizeof buf, &got, NULL) && got) { ok = hm_sha_add(&s, buf, got); total += got; }
    CloseHandle(f);
    if (!hm_sha_end(&s, hex) || !ok) return 0;
    if (size_out) *size_out = total;
    return 1;
}
#endif

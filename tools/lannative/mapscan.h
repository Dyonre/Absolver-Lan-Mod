#ifndef LANNATIVE_MAPSCAN_H
#define LANNATIVE_MAPSCAN_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const unsigned char *p; size_t left; } PakCursor;
static int pak_take(PakCursor *c, void *out, size_t n) {
    if (n > c->left) return 0;
    if (out) memcpy(out, c->p, n);
    c->p += n; c->left -= n; return 1;
}
static int pak_string(PakCursor *c, char *out, size_t cap) {
    int32_t n; if (!pak_take(c, &n, 4) || n <= 0 || (size_t)n > cap) return 0;
    if (!pak_take(c, out, (size_t)n) || out[n-1] != 0 || memchr(out, 0, n-1)) return 0;
    return 1;
}
static int mapscan_path(const char *mount, const char *entry, char *out, size_t cap) {
    char full[1024];
    if (snprintf(full, sizeof full, "%s%s", mount, entry) >= sizeof full) return 0;
    for (char *p = full; *p; p++) if (*p == '\\') *p = '/';
    const char *content = strstr(full, "/Absolver/Content/");
    if (!content) return 0;
    content += strlen("/Absolver/Content/");
    size_t n = strlen(content);
    if (n < 6 || _stricmp(content+n-5, ".umap") || strstr(content, "..") || strchr(content, '?') || strchr(content, ':') || strchr(content, ' ')) return 0;
    if (n+2 >= cap) return 0;
    snprintf(out, cap, "/Game/%.*s", (int)(n-5), content); return 1;
}
// F1 map list: a short label "<group>: <map>" and a sort order by group, so the 1v1 / 3v3 / Downfall rooms / cinematics read as groups.
// Groups: 0 1v1, 1 3v3, 2 Downfall (PVE, "T<n> <kind>", rotated copies marked), 3 cinematic / intro, 4 open world / tutorial / title, 5 other.
static int map_menu_group(const char *url) {
    if (strstr(url, "/1v1/")) return 0;
    if (strstr(url, "/3v3/") || strstr(url, "GM_3v3_")) return 1;
    if (strstr(url, "/PVE/")) return 2;
    if (strstr(url, "/Cinematics/") || strstr(url, "/IntroOutro/") || strstr(url, "Cutscene")) return 3;
    if (strstr(url, "/MainMap/") || strstr(url, "/StartMap/") || strstr(url, "/Tutorial/") || strstr(url, "/TrainingMap/") || strstr(url, "/Zoos/")) return 4;
    return 5;
}
static void map_menu_label(const char *url, char *out, size_t cap) {
    char path[400]; snprintf(path, sizeof path, "%.*s", (int)strcspn(url, "?"), url);
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    static const char *const G[] = { "1v1", "3v3", "Downfall", "Cinematic", "World", "Other" };
    int g = map_menu_group(path); char grp[64]; snprintf(grp, sizeof grp, "%s", G[g]);
    const char *t = g == 2 ? strstr(path, "/Theme_") : NULL;
    if (t) { const char *k = strchr(t + 7, '/'); char kind[24] = ""; if (k) snprintf(kind, sizeof kind, "%.*s", (int)strcspn(k + 1, "/"), k + 1);
             snprintf(grp, sizeof grp, "Downfall T%c%s%s%s", t[7], kind[0] ? " " : "", kind, strstr(path, "/Rotate/") ? " (rotated)" : ""); }
    snprintf(out, cap, "%s: %s", grp, name);
}
static int map_menu_compare(const void *a, const void *b) {
    int ga = map_menu_group(a), gb = map_menu_group(b); if (ga != gb) return ga - gb;
    char la[200], lb[200]; map_menu_label(a, la, sizeof la); map_menu_label(b, lb, sizeof lb); return _stricmp(la, lb);
}

static int mapscan_root(const char *path) {
    if (strstr(path, "/StartMap/")) return 0;   // the title screen: travelling there drops you to the main menu
    if (strstr(path, "/Sub-Levels/") || strstr(path, "/SubLevels/") || strstr(path, "/Lighting/") || strstr(path, "_Light") || strstr(path, "_LD") || strstr(path, "_Sound")) return 0;
    if (strstr(path, "/Maps/MainMap_GameModes/1v1/") || strstr(path, "/Maps/MainMap_GameModes/3v3/") || strstr(path, "/Maps/MainMap_GameModes/PVE/")) return 1;
    const char *name = strrchr(path, '/'); if (!name) return 0;
    // shortcut: conventional root names only; unusual streamed layouts need an explicit maps.txt entry.
    size_t n = strlen(name+1), parent = (size_t)(name-path);
    if (n >= 5 && !_stricmp(name+1+n-5, "_Main")) return 1;
    return parent > n && path[parent-n-1] == '/' && !_strnicmp(path+parent-n, name+1, n);
}

// UE4.18 legacy indexes only. No decompression, decryption, extraction or execution.
// Returns map count, or -1 for an unsupported/malformed index; callbacks run only after full validation.
static int mapscan_pak(FILE *f, void (*found)(const char *, void *), void *context) {
    unsigned char tail[256];
    if (_fseeki64(f, 0, SEEK_END)) return -1;
    int64_t size = _ftelli64(f); if (size < 44) return -1;
    size_t tn = size < 256 ? (size_t)size : 256;
    if (_fseeki64(f, size-(int64_t)tn, SEEK_SET) || fread(tail, 1, tn, f) != tn) return -1;
    int version = 0; int64_t offset = 0, bytes = 0;
    for (size_t i = 0; i+44 <= tn; i++) {
        uint32_t magic; memcpy(&magic, tail+i, 4);
        if (magic != 0x5A6F12E1 || i+44 != tn) continue;
        memcpy(&version, tail+i+4, 4); memcpy(&offset, tail+i+8, 8); memcpy(&bytes, tail+i+16, 8);
        if (version < 1 || version > 7 || (version >= 4 && (i == 0 || tail[i-1] != 0))) return -1;
        break;
    }
    if (!version || offset < 0 || bytes < 8 || bytes > 64*1024*1024 || offset > size-bytes || offset+bytes > size-44) return -1;
    unsigned char *data = malloc((size_t)bytes); if (!data) return -1;
    if (_fseeki64(f, offset, SEEK_SET) || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) { free(data); return -1; }
    int maps = 0, ok = 1;
    for (int pass = 0; pass < 2 && ok; pass++) {
        PakCursor c = {data, (size_t)bytes}; char mount[512], entry[512], path[512]; int32_t count;
        if (!pak_string(&c, mount, sizeof mount) || !pak_take(&c, &count, 4) || count < 0 || count > 1000000) { ok = 0; break; }
        for (int32_t i = 0; i < count; i++) {
            uint32_t compression, blocks;
            if (!pak_string(&c, entry, sizeof entry) || !pak_take(&c, NULL, 24) || !pak_take(&c, &compression, 4) ||
                (version == 1 && !pak_take(&c, NULL, 8)) || !pak_take(&c, NULL, 20)) { ok = 0; break; }
            if (version >= 3) {
                if (compression && (!pak_take(&c, &blocks, 4) || blocks > c.left/16 || !pak_take(&c, NULL, (size_t)blocks*16))) { ok = 0; break; }
                if (!pak_take(&c, NULL, 5)) { ok = 0; break; }
            }
            if (mapscan_path(mount, entry, path, sizeof path)) { if (pass) found(path, context); else maps++; }
        }
        if (c.left != 0) ok = 0;
    }
    free(data); return ok ? maps : -1;
}
#endif

#ifndef LANNATIVE_MODPAK_H
#define LANNATIVE_MODPAK_H
// Reads the file list of a mod .pak (UE4.18 legacy, unencrypted index) so the mod loader can report conflicts between mods
// and fingerprint a mod set. Read-only: no extraction, no decompression, no execution. Reuses the PakCursor helpers from mapscan.h.
#include "mapscan.h"

typedef struct { char path[512]; unsigned char sha1[20]; } PakEntry;

// "../../../" + "Absolver/Content/DB/x.uasset" -> "Absolver/Content/DB/x.uasset" (forward slashes, mount-relative root stripped).
static int modpak_path(const char *mount, const char *entry, char *out, size_t cap) {
    char full[1100];
    if (snprintf(full, sizeof full, "%s%s", mount, entry) >= (int)sizeof full) return 0;
    for (char *p = full; *p; p++) if (*p == '\\') *p = '/';
    const char *s = full; int ups = 0;
    while (!strncmp(s, "../", 3) || !strncmp(s, "./", 2)) { if (s[1] == '.') { ups++; s += 3; } else s += 2; }
    if (ups > 3) return 0;                                    // the engine root is ../../../ ; anything above it is a traversal
    while (*s == '/') s++;
    if (!*s || strstr(s, "..") || (strlen(s) >= cap)) return 0;
    strcpy(out, s); return 1;
}

// A pak ends with a 44-byte footer (magic 0x5A6F12E1, version, index offset, index size, index SHA-1). Cheap check done before the engine mounts a file.
// Returns 1 = plausible pak, 0 = too small or no footer magic, -1 = the footer's index lies outside the file.
static int modpak_footer_ok(FILE *f) {
    unsigned char tail[44]; uint32_t magic; int64_t offset, bytes;
    if (_fseeki64(f, 0, SEEK_END)) return 0;
    int64_t size = _ftelli64(f); if (size < 44) return 0;
    if (_fseeki64(f, size - 44, SEEK_SET) || fread(tail, 1, 44, f) != 44) return 0;
    memcpy(&magic, tail, 4); if (magic != 0x5A6F12E1) return 0;
    memcpy(&offset, tail + 8, 8); memcpy(&bytes, tail + 16, 8);
    if (offset < 0 || bytes < 0 || bytes > size - 44 || offset > size - 44 - bytes) return -1;     // the index must end before the footer (no overflow: compared by subtraction)
    return 1;
}

// Returns the number of entries (>= 0) with *out malloc'd (free it), or -1 for an unsupported/malformed/encrypted index.
// index_hash (optional) receives the footer's index SHA-1: it changes whenever any file in the pak changes.
static int modpak_read(FILE *f, PakEntry **out, unsigned char index_hash[20]) {
    unsigned char tail[256];
    *out = NULL;
    if (_fseeki64(f, 0, SEEK_END)) return -1;
    int64_t size = _ftelli64(f); if (size < 44) return -1;
    size_t tn = size < 256 ? (size_t)size : 256;
    if (_fseeki64(f, size - (int64_t)tn, SEEK_SET) || fread(tail, 1, tn, f) != tn) return -1;
    int version = 0; int64_t offset = 0, bytes = 0; unsigned char hash[20] = {0};
    for (size_t i = 0; i + 44 <= tn; i++) {
        uint32_t magic; memcpy(&magic, tail + i, 4);
        if (magic != 0x5A6F12E1 || i + 44 != tn) continue;
        memcpy(&version, tail + i + 4, 4); memcpy(&offset, tail + i + 8, 8); memcpy(&bytes, tail + i + 16, 8); memcpy(hash, tail + i + 24, 20);
        if (version < 1 || version > 7) return -1;
        if (version >= 4 && (i == 0 || tail[i - 1] != 0)) return -1;      // encrypted index: not readable
        break;
    }
    if (!version || offset < 0 || bytes < 8 || bytes > 256 * 1024 * 1024 || offset > size - bytes || offset + bytes > size - 44) return -1;
    unsigned char *data = malloc((size_t)bytes); if (!data) return -1;
    if (_fseeki64(f, offset, SEEK_SET) || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) { free(data); return -1; }
    PakCursor c = { data, (size_t)bytes }; char mount[512], name[512]; int32_t count;
    if (!pak_string(&c, mount, sizeof mount) || !pak_take(&c, &count, 4) || count < 0 || count > 2000000) { free(data); return -1; }
    PakEntry *list = calloc(count ? (size_t)count : 1, sizeof *list); if (!list) { free(data); return -1; }
    int ok = 1, n = 0;
    for (int32_t i = 0; i < count && ok; i++) {
        uint32_t compression, blocks;
        if (!pak_string(&c, name, sizeof name) || !pak_take(&c, NULL, 24) || !pak_take(&c, &compression, 4) ||
            (version == 1 && !pak_take(&c, NULL, 8)) || !pak_take(&c, list[n].sha1, 20)) { ok = 0; break; }
        if (version >= 3) {
            if (compression && (!pak_take(&c, &blocks, 4) || blocks > c.left / 16 || !pak_take(&c, NULL, (size_t)blocks * 16))) { ok = 0; break; }
            if (!pak_take(&c, NULL, 5)) { ok = 0; break; }
        }
        if (modpak_path(mount, name, list[n].path, sizeof list[n].path)) n++;
    }
    free(data);
    if (!ok || c.left != 0) { free(list); return -1; }
    if (index_hash) memcpy(index_hash, hash, 20);
    *out = list; return n;
}
#endif

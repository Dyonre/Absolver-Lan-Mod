// Pure URL formatter used by the live save reader and its offline test.
#pragma once
#include <wchar.h>

// ip.txt line 2 = "none" means: send no URL options at all (a PvP host has no co-op altars, so CheckPoint/zone would be meaningless there).
static int join_options_is_none(const char *line2) {
    if (!line2) return 0;
    while (*line2 == ' ' || *line2 == '\t') line2++;
    return (line2[0] == 'n' || line2[0] == 'N') && (line2[1] == 'o' || line2[1] == 'O') && (line2[2] == 'n' || line2[2] == 'N') &&
           (line2[3] == 'e' || line2[3] == 'E') && (line2[4] == 0 || line2[4] == ' ' || line2[4] == '\t' || line2[4] == '\r' || line2[4] == '\n');
}

static int join_url_format(const char *ip, const char *manual_options, const wchar_t *checkpoint,
                           unsigned zone, wchar_t *out, size_t cap) {
    if (!ip || !*ip || !out || cap == 0) return 0;
    wchar_t host[300];
    int host_n = MultiByteToWideChar(CP_UTF8, 0, ip, -1, host, (int)(sizeof host / sizeof host[0]));
    if (!host_n) return 0;
    const wchar_t *port = strchr(ip, ':') ? L"" : L":7777";
    wchar_t opts[600] = L"";
    if (manual_options && *manual_options) {
        if (!MultiByteToWideChar(CP_UTF8, 0, manual_options, -1, opts, (int)(sizeof opts / sizeof opts[0]))) return 0;
    } else if (checkpoint && *checkpoint) {
        _snwprintf(opts, sizeof opts / sizeof opts[0], L"?CheckPoint=%ls?zone=%u", checkpoint, zone);
        opts[(sizeof opts / sizeof opts[0]) - 1] = 0;
    }
    return _snwprintf(out, cap, L"%ls%ls%ls", host, port, opts) >= 0;
}

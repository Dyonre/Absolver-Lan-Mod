// Pure command-line logic for AbsolverModLauncher (unit-tested by test.c, no Windows calls).
#pragma once
#include <string.h>
#include <wchar.h>

// True if arg is a path ending in "Absolver.exe" (what Steam substitutes for %command%), case-insensitive, quotes ignored.
static int is_steam_command_path(const wchar_t *a) {
    size_t n = wcslen(a);
    while (n && a[n - 1] == L'"') n--;
    const wchar_t *tail = L"Absolver.exe"; size_t t = wcslen(tail);
    if (n < t) return 0;
    if (_wcsnicmp(a + n - t, tail, t) != 0) return 0;
    return n == t || a[n - t - 1] == L'\\' || a[n - t - 1] == L'/';
}

static int has_arg(int argc, wchar_t **argv, const wchar_t *flag) {
    for (int i = 1; i < argc; i++) if (_wcsicmp(argv[i], flag) == 0) return 1;
    return 0;
}

// How a start goes: ask, modded (game exe + -NoEAC + loader) or vanilla (the original EasyAntiCheat launcher, no loader).
enum { MODE_ASK = 0, MODE_MODDED = 1, MODE_VANILLA = 2 };
// -Vanilla / -Modded on the command line win; then Shift held at start asks; then the saved choice ("vanilla" / "modded",
// first word of LanNative\launch.txt, NULL = no file); anything else asks.
static int launch_mode(int argc, wchar_t **argv, const char *saved, int shift) {
    if (has_arg(argc, argv, L"-Vanilla")) return MODE_VANILLA;
    if (has_arg(argc, argv, L"-Modded")) return MODE_MODDED;
    if (shift || !saved) return MODE_ASK;
    if (_strnicmp(saved, "vanilla", 7) == 0) return MODE_VANILLA;
    if (_strnicmp(saved, "modded", 6) == 0) return MODE_MODDED;
    return MODE_ASK;
}

// Builds the command line: quoted exe, -NoEAC once if noeac (modded), then the user's args minus Steam's %command% path,
// minus launcher-only flags and minus any -NoEAC of their own (a vanilla start must keep EasyAntiCheat on).
// Returns the number of characters written, or -1 if cap is too small.
static int build_game_cmdline(wchar_t *out, size_t cap, const wchar_t *exe, int noeac, int argc, wchar_t **argv) {
    static const wchar_t *const own[] = { L"--setup", L"--migrate", L"-NoEAC", L"-Vanilla", L"-Modded" };
    int n = swprintf(out, cap, noeac ? L"\"%ls\" -NoEAC" : L"\"%ls\"", exe);
    if (n < 0) return -1;
    for (int i = 1; i < argc; i++) {
        if (i == 1 && is_steam_command_path(argv[i])) continue;
        int skip = 0; for (size_t k = 0; k < sizeof own / sizeof *own; k++) if (_wcsicmp(argv[i], own[k]) == 0) skip = 1;
        if (skip) continue;
        int w = wcschr(argv[i], L' ') ? swprintf(out + n, cap - n, L" \"%ls\"", argv[i]) : swprintf(out + n, cap - n, L" %ls", argv[i]);
        if (w < 0) return -1;
        n += w;
    }
    return n;
}

// The game's own paks: pakchunk0/1/2-WindowsNoEditor.pak and .sig. Everything else in Content\Paks is a mod (or leftover); --migrate moves those into Mods\.
static int is_vanilla_pak_name(const wchar_t *n) {
    if (_wcsnicmp(n, L"pakchunk", 8) != 0 || n[8] < L'0' || n[8] > L'2') return 0;
    return _wcsicmp(n + 9, L"-WindowsNoEditor.pak") == 0 || _wcsicmp(n + 9, L"-WindowsNoEditor.sig") == 0;
}
// "zzzProxyMod.pak" -> "ProxyMod": drops the extension and a leading "zzz" (the usual load-last prefix), replaces characters that are
// not allowed in a folder name. Returns 0 if nothing usable is left.
static int mod_folder_name(const wchar_t *file, wchar_t *out, size_t cap) {
    size_t n = wcslen(file); if (n >= 4 && _wcsicmp(file + n - 4, L".pak") == 0) n -= 4;
    size_t start = (n > 3 && _wcsnicmp(file, L"zzz", 3) == 0) ? 3 : 0;
    if (n - start == 0 || n - start >= cap) return 0;
    size_t w = 0;
    for (size_t i = start; i < n; i++) out[w++] = wcschr(L"\\/:*?\"<>|", file[i]) ? L'_' : file[i];
    out[w] = 0;
    while (w && (out[w - 1] == L'.' || out[w - 1] == L' ')) out[--w] = 0;      // Windows drops trailing dots and spaces from folder names
    return w > 0;
}

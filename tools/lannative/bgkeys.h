// bgkeys.h - background input for test runs : the keyboard state of the game window as its own WM_KEYDOWN / WM_KEYUP messages say it,
// instead of GetAsyncKeyState (= the real keyboard, which only the focused window sees). A test driver that posts keys to an unfocused game window
// (SE_KEYMODE=post) then drives loader / plugin binds (K, F9, ...) without ever taking the foreground.
// OFF unless the command line has -LNBgInput (a test driver passes it; a player's command line never does). Pure logic.
// A posted tap is down for only ~50 ms and a mod polls once per frame (a background game may run at a few fps): a key counts as down for BGK_TAIL_MS after
// its WM_KEYDOWN even when the WM_KEYUP already came, so no tap falls between two polls.
#ifndef BGKEYS_H
#define BGKEYS_H
#include <wchar.h>

#define BGK_TAIL_MS 250
static volatile long g_bgk_down[256];                               // 1 = the game window saw this key go down and not up yet
static volatile unsigned long long g_bgk_t[256];                    // tick (ms) of the last WM_KEYDOWN, 0 = never

// "-LNBgInput" or "/LNBgInput" as a whole token, case-insensitive (same rule as cmdline_has_flag in modglue.inc: -LNBgInputX or a path part does not count)
static int bgk_flag(const wchar_t *cl) {
    static const wchar_t flag[] = L"LNBgInput"; const size_t n = 9;
    for (const wchar_t *p = cl; p && *p; p++) {
        int left = p == cl || p[-1] == L' ' || p[-1] == L'\t' || p[-1] == L'"';
        if (left && (p[0] == L'-' || p[0] == L'/') && _wcsnicmp(p + 1, flag, n) == 0 &&
            (p[1 + n] == 0 || p[1 + n] == L' ' || p[1 + n] == L'"' || p[1 + n] == L'\t')) return 1;
    }
    return 0;
}
// feed one window message (window thread): 0x100 / 0x104 = down, 0x101 / 0x105 = up (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP); now = a millisecond tick
static void bgk_msg(unsigned msg, unsigned long long vk, unsigned long long now) {
    if (vk >= 256) return;
    if (msg == 0x100 || msg == 0x104) { g_bgk_down[vk] = 1; g_bgk_t[vk] = now ? now : 1; } else if (msg == 0x101 || msg == 0x105) g_bgk_down[vk] = 0;
}
static int bgk_down(int vk, unsigned long long now) {
    if (vk <= 0 || vk >= 256) return 0;
    return g_bgk_down[vk] != 0 || (g_bgk_t[vk] != 0 && now >= g_bgk_t[vk] && now - g_bgk_t[vk] < BGK_TAIL_MS);
}
static void bgk_clear(void) { for (int i = 0; i < 256; i++) { g_bgk_down[i] = 0; g_bgk_t[i] = 0; } }
#endif

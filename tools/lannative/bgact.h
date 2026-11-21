// bgact.h - pure decisions of the background activation guard (used by overlay.cpp under -LNBgInput). The game and the plugins must never take the foreground from the user's windows: UE4 activates its own window
// after creating it (ShowWindow / SetForegroundWindow / SetFocus ...), so the loader routes those calls of the game's and of every mod DLL's import table through
// non-activating versions. The mods also see the game window as "foreground" and the window's own key state, so their focus checks and GetAsyncKeyState
// polls work on an unfocused window.
#ifndef BGACT_H
#define BGACT_H
#include <string.h>
#include <ctype.h>

// ShowWindow command -> the same without activation. SW_SHOWNORMAL 1 -> SW_SHOWNOACTIVATE 4, SW_SHOW 5 -> SW_SHOWNA 8, SW_RESTORE 9 -> 4, SW_SHOWDEFAULT 10 -> 4,
// SW_SHOWMINIMIZED 2 -> SW_SHOWMINNOACTIVE 7. SW_SHOWMAXIMIZED 3 has no non-activating form and is left alone; hide / minimize / already non-activating stay.
static int bga_showcmd(int c) {
    switch (c) { case 1: case 9: case 10: return 4; case 5: return 8; case 2: return 7; default: return c; }
}
// SetWindowPos flags: always SWP_NOACTIVATE (0x10); a hide stays a hide
static unsigned bga_swpflags(unsigned f) { return f | 0x10u; }
// WM_MOUSEACTIVATE (0x21): a click never activates the window -> MA_NOACTIVATE (3). Returns 1 and the result when the message is handled here.
static int bga_wndmsg(unsigned msg, long long *result) { if (msg == 0x21) { *result = 3; return 1; } return 0; }
// what a mod's GetAsyncKeyState reads: the game window's own key state (down = the window saw the key go down, see bgkeys.h); never the real mouse buttons (vk 1-6)
static short bga_asynckey(int vk, int down) { return (short)((vk > 6 && vk < 256 && down) ? 0x8000 : 0); }
// a module path belongs to a mod when it sits under a Mods folder (the game's own DLLs and Windows' are not patched for the focus / key reads)
static int bga_is_mod_path(const char *p) {
    if (!p) return 0;
    for (; *p; p++) if (*p == '\\' && (p[1] == 'M' || p[1] == 'm') && (p[2] == 'O' || p[2] == 'o') && (p[3] == 'D' || p[3] == 'd') && (p[4] == 'S' || p[4] == 's') && p[5] == '\\') return 1;
    return 0;
}
#endif

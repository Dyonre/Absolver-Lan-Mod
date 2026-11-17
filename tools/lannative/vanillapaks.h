// vanillapaks.h - the loader's "content paks" rule (pure code).
// Under the loader only the game's own paks (pakchunk0/1/2-WindowsNoEditor.pak directly in Absolver\Content\Paks) and the
// mods in Mods\ are mounted. Every other pak anywhere under Content\Paks (root, ~~\, VanillaOnly\, any subfolder) is a vanilla (pak-only) mod: the
// engine mounts it in Vanilla starts, the loader refuses it (modglue.inc, hook on FPakPlatformFile::Mount). Paks outside Content\Paks (Mods\) are
// never touched by this rule. Setting content_paks = all (or the test switch vanilla_paks = load / -VanillaPaks) turns the rule off.
#ifndef LANNATIVE_VANILLAPAKS_H
#define LANNATIVE_VANILLAPAKS_H
#include <wchar.h>

static wchar_t vp_low(wchar_t c) { return c == L'\\' ? L'/' : (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; }

// the part of the path after the first "Content/Paks/" segment (case-insensitive, '/' or '\'; the segment must start the path or follow a
// separator, so "MyContent/Paks/" does not count); NULL when the path is not under a Content/Paks folder
static const wchar_t *vp_after_content_paks(const wchar_t *path) {
    static const wchar_t key[] = L"content/paks/";
    const size_t kn = sizeof key / sizeof key[0] - 1;
    for (const wchar_t *p = path; p && *p; p++) {
        if (p != path && p[-1] != L'/' && p[-1] != L'\\') continue;
        size_t i = 0;
        while (i < kn && p[i] && vp_low(p[i]) == key[i]) i++;
        if (i == kn) return p + kn;
    }
    return NULL;
}
static int vp_ieq(const wchar_t *a, const char *b) { while (*a && *b && vp_low(*a) == (wchar_t)*b) { a++; b++; } return !*a && !*b; }

// 1 = one of the game's own paks: pakchunk0/1/2-WindowsNoEditor.pak directly in Content/Paks (not in a subfolder; pakchunk3+ and .sig are not stock paks)
static int vp_is_stock(const wchar_t *path) {
    static const char *const stock[] = { "pakchunk0-windowsnoeditor.pak", "pakchunk1-windowsnoeditor.pak", "pakchunk2-windowsnoeditor.pak" };
    const wchar_t *t = vp_after_content_paks(path);
    if (!t) return 0;
    for (const wchar_t *q = t; *q; q++) if (*q == L'/' || *q == L'\\') return 0;      // in a subfolder
    for (size_t i = 0; i < sizeof stock / sizeof stock[0]; i++) if (vp_ieq(t, stock[i])) return 1;
    return 0;
}
// the rule: 1 = the loader keeps this pak unmounted (under Content/Paks and not a stock pak)
static int vp_is_vanilla_pak(const wchar_t *path) { return vp_after_content_paks(path) && !vp_is_stock(path); }
#endif

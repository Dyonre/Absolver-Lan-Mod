// LanNative only runs when the game was started with -NoEAC (Steam launch option), i.e. when EasyAntiCheat is not protecting the process.
// It never tries to hide from, disable or work around EAC; with EAC active it does nothing at all.
#pragma once
#include <wchar.h>

static int cmdline_has_noeac(const wchar_t *cl) {
    if (!cl) return 0;
    const wchar_t *start = cl;
    for (; *cl; cl++) {
        int left = cl == start || cl[-1] == L' ' || cl[-1] == L'\t' || cl[-1] == L'"';
        if (left && (cl[0] == L'-' || cl[0] == L'/') && _wcsnicmp(cl + 1, L"noeac", 5) == 0 &&
            (cl[6] == 0 || cl[6] == L' ' || cl[6] == L'"' || cl[6] == L'\t'))
            return 1;
    }
    return 0;
}

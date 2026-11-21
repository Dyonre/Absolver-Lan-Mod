// gleamui.h - small Dear ImGui widgets shared by the loader's overlay (overlay.cpp) and plugins that draw ImGui themselves (Cheats). Header only, C++.
// Include it after imgui.h. Nothing here touches the game; every widget returns true on the frame its value changed.
#ifndef GLEAMUI_H
#define GLEAMUI_H
#include <stdio.h>
#include <stdlib.h>

// A number field: type a value (digits, sign, point only) or step it with - / +; the result is clamped to [mn, mx] (mx < mn = no limit).
// step 0 = no buttons. integer = 1 rounds to whole numbers. width <= 0 = the default item width.
static inline bool gleam_number(const char *id, double *v, double mn, double mx, double step, int integer, float width = 0.f) {
    double x = *v;
    if (width > 0.f) ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputDouble(id, &x, step, step * 10.0, integer ? "%.0f" : "%.6g", ImGuiInputTextFlags_CharsScientific);
    if (mx >= mn) { if (x < mn) x = mn; if (x > mx) x = mx; }
    if (integer) x = (double)(long long)(x < 0 ? x - 0.5 : x + 0.5);
    (void)changed;
    if (x != *v) { *v = x; return true; }
    return false;
}
// The same over a text value (the settings forms keep every value as text): an empty / unparsable text shows as mn (or 0).
static inline bool gleam_number_text(const char *id, char *buf, size_t cap, double mn, double mx, int integer, float width = 0.f) {
    char *end = NULL; double v = strtod(buf, &end); if (end == buf) v = mn > -1e17 ? mn : 0.0;
    double step = integer ? 1.0 : (mx > mn && mx - mn < 1e9 ? (mx - mn) / 100.0 : 0.1);
    if (!gleam_number(id, &v, mn, mx, step, integer, width)) return false;
    if (integer) snprintf(buf, cap, "%lld", (long long)v); else snprintf(buf, cap, "%.6g", v);
    return true;
}
#endif

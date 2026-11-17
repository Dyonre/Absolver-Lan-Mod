// modprofile.h - mod profiles: named copies of the on/off + order set. Pure C on modconf.h's ModList.
// A profile is Mods\profiles\<name>.txt in loadorder.txt's own format ("Mod" on, "-Mod" off, last line = highest priority). Switching applies it to the mods
// installed now: the profile's order and on/off for every mod it names that is installed, mods it does not name keep their state and go after them
// (newer mods stay where loadorder.txt puts new folders: at the end), mods it names that are gone are left out (counted).
#ifndef LANNATIVE_MODPROFILE_H
#define LANNATIVE_MODPROFILE_H
#include "modconf.h"

#define PROFILE_NAME_MAX 40
static const char *const g_profile_suggest[3] = { "Everything", "Light", "Testing" };   // offered on the Mods page as names; no profile is created by default

// letters, digits, space, '-', '_'; 1..40 characters, no space at either end (the name is a file name)
static int profile_name_ok(const char *n) {
    size_t len = n ? strlen(n) : 0; if (!len || len > PROFILE_NAME_MAX || n[0] == ' ' || n[len - 1] == ' ') return 0;
    for (size_t i = 0; i < len; i++) { char c = n[i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')) return 0; }
    static const char *const dev[] = { "CON", "PRN", "AUX", "NUL" };                     // Windows device names (also COM1-9, LPT1-9)
    for (int i = 0; i < 4; i++) if (!_stricmp(n, dev[i])) return 0;
    if (len == 4 && (!_strnicmp(n, "COM", 3) || !_strnicmp(n, "LPT", 3)) && n[3] >= '1' && n[3] <= '9') return 0;
    return 1;
}
static void profile_apply(const ModList *now, const ModList *prof, ModList *out, int *missing, int *extra) {
    int miss = 0, ext = 0; out->count = 0;
    for (int i = 0; i < prof->count; i++) {
        int at = modlist_find(now, prof->mods[i].name);
        if (at < 0) { miss++; continue; }
        if (modlist_find(out, prof->mods[i].name) >= 0) continue;
        out->mods[out->count] = now->mods[at]; out->mods[out->count].enabled = prof->mods[i].enabled; out->count++;
    }
    for (int i = 0; i < now->count; i++) if (modlist_find(out, now->mods[i].name) < 0 && out->count < MODS_MAX) { out->mods[out->count++] = now->mods[i]; ext++; }
    if (missing) *missing = miss; if (extra) *extra = ext;
}
static int modlist_same(const ModList *a, const ModList *b) {
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; i++) if (_stricmp(a->mods[i].name, b->mods[i].name) || !a->mods[i].enabled != !b->mods[i].enabled) return 0;
    return 1;
}
static int modlist_on(const ModList *l) { int n = 0; for (int i = 0; i < l->count; i++) n += l->mods[i].enabled != 0; return n; }
// the profile file's text; returns its length, -1 = cap too small
static int profile_render(const ModList *l, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "# Gleam Loader mod profile '%s': the Mods page's \"Switch to\" writes this set into loadorder.txt (after a restart it is the game's).\r\n"
                               "# Same format as loadorder.txt: one mod folder per line, a - in front = off, the last line wins when two mods change the same file.\r\n", name);
    for (int i = 0; i < l->count && n >= 0 && (size_t)n < cap; i++) n += snprintf(out + n, cap - (size_t)n, "%s%s\r\n", l->mods[i].enabled ? "" : "-", l->mods[i].name);
    return n >= 0 && (size_t)n < cap ? n : -1;
}
#endif

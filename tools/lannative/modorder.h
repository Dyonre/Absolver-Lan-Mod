#ifndef LANNATIVE_MODORDER_H
#define LANNATIVE_MODORDER_H
// Mods page: pure logic for changing Mods\loadorder.txt from the page (no Windows calls).
// A change is edited in a ModList (modconf.h), written as text (the caller keeps a .bak of the old file), and compared with the list the game booted with to
// tell what is still waiting for a restart. Only a mod that is OFF at boot and holds nothing but rows.json / content\ files can be switched on without one.
#include "modconf.h"

typedef struct { int paks, plugin, lua, requires; } ModKind;       // what a mod folder holds that a running game cannot pick up (rows / content are picked up by the hot reload)

// 1 = the mod can be loaded into the running game (reload of its rows.json and content\), 0 = needs a restart
static int mo_hot_enable_ok(const ModKind *k) { return !k->paks && !k->plugin && !k->lua && !k->requires; }

static int mo_set_enabled(ModList *l, const char *name, int enabled) {
    int i = modlist_find(l, name); if (i < 0) return -1;
    l->mods[i].enabled = enabled ? 1 : 0; return i;
}
// moves entry i one place (dir -1 = earlier = lower priority, +1 = later = wins conflicts); returns the new index, or -1 if it cannot move
static int mo_move(ModList *l, int i, int dir) {
    int j = i + (dir < 0 ? -1 : 1); if (i < 0 || i >= l->count || j < 0 || j >= l->count) return -1;
    ModEntry t = l->mods[i]; l->mods[i] = l->mods[j]; l->mods[j] = t; return j;
}
// What differs between the list the game booted with and the list now: mods switched on or off, and mods whose place among the mods that are both in the lists changed.
typedef struct { int turned_on, turned_off, moved; } ModDiff;
static ModDiff mo_diff(const ModList *boot, const ModList *now) {
    ModDiff d = { 0, 0, 0 };
    int bi[MODS_MAX], n = 0;                                               // mods present in both, in boot order -> their index in now
    for (int i = 0; i < boot->count; i++) {
        int j = modlist_find(now, boot->mods[i].name); if (j < 0) continue;
        if (boot->mods[i].enabled && !now->mods[j].enabled) d.turned_off++;
        if (!boot->mods[i].enabled && now->mods[j].enabled) d.turned_on++;
        if (n < MODS_MAX) bi[n++] = j;
    }
    for (int a = 1; a < n; a++) if (bi[a] < bi[a - 1]) { d.moved++; }      // every descent in the new order is one displaced mod (a single swap counts 1)
    return d;
}
static int mo_pending(const ModDiff *d) { return d->turned_on + d->turned_off + d->moved; }
static int mo_describe(const ModDiff *d, char *out, size_t cap) {
    int w = snprintf(out, cap, "%d switched on, %d switched off, %d moved", d->turned_on, d->turned_off, d->moved); return w;
}
// The text to write. Same format the loader writes at boot.
static int mo_render(const ModList *l, char *out, size_t cap) { return modlist_render(l, out, cap); }
#endif

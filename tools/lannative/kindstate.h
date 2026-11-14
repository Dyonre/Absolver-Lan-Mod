// kindstate.h - pure logic of the weapon-kind state (a kind is an item on a base family).
// Runtime state is PER CHARACTER (a table keyed by the character pointer; NULL = the local player, which keeps its entry across travel), so bots / NPCs
// can hold kinds too. Persistent state is PER CHARACTER AND PER KIND in that character's sidecar document (sidecar.h):
//   kind.<item id>.deck    the kind's deck used last            (fallback: legacy.staff_deck, migrated from "<name>.staff_deck" in LanNative\data\Content.txt)
//   family.<n>.deck        the base family's deck for its other weapons (fallback: legacy.sword_deck, from "<name>.sword_deck")
#ifndef KINDSTATE_H
#define KINDSTATE_H
#include <stdio.h>
#include <string.h>

#define KS_MAX 32
typedef struct { void *ch; int used, equipped, drawn; } KsChar;      // equipped / drawn = index of the kind (weaponslot's item table), -1 = none
static KsChar *ks_get(KsChar *t, void *ch, int create) {
    for (int i = 0; i < KS_MAX; i++) if (t[i].used && t[i].ch == ch) return &t[i];
    if (!create) return NULL;
    for (int i = 0; i < KS_MAX; i++) if (!t[i].used) { t[i].used = 1; t[i].ch = ch; t[i].equipped = t[i].drawn = -1; return &t[i]; }
    return NULL;                                                      // full: the caller logs it
}
static int ks_drop_others(KsChar *t) { int n = 0; for (int i = 0; i < KS_MAX; i++) if (t[i].used && t[i].ch) { t[i].used = 0; n++; } return n; }   // a new world: other characters are gone
static int ks_count(const KsChar *t) { int n = 0; for (int i = 0; i < KS_MAX; i++) n += t[i].used; return n; }
// persistent keys; return 0 when the result does not fit or an argument is unusable
static int ks_key(char *out, int cap, const char *kind, const char *what) { if (!kind || !*kind || !what || !*what || strpbrk(kind, "=\r\n")) return 0; int n = snprintf(out, (size_t)cap, "kind.%s.%s", kind, what); return n > 0 && n < cap; }
static int ks_family_key(char *out, int cap, int family) { if (family < 0 || family > 2) return 0; int n = snprintf(out, (size_t)cap, "family.%d.deck", family); return n > 0 && n < cap; }
#endif

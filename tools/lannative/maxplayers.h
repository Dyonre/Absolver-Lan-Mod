#ifndef LANNATIVE_MAXPLAYERS_H
#define LANNATIVE_MAXPLAYERS_H
// Host player cap (pure logic; PreLogin glue in hostmods.inc).
//   config.txt  maxplayers = 6 | 2..16 | off      (players in the host's world, the host included; default 6)
// The host refuses a login in PreLogin when the world would hold more than that. Vanilla has no cap at the door, so this is the only one.
// Local test instances (-InstanceID<n>) are ordinary joiners here: the cap counts connections, never ids.
#include <stdlib.h>
#include <string.h>

#define MP_DEFAULT 6
#define MP_MIN 2
#define MP_MAX 16

// value text -> cap (0 = off). Unknown or out-of-range text -> *bad = 1 and the default.
static int mp_parse(const char *v, int *bad) {
    *bad = 0;
    if (!v || !*v) return MP_DEFAULT;
    if (!_strnicmp(v, "off", 3) || !_strnicmp(v, "unlimited", 9)) return 0;
    char *end; long n = strtol(v, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (end == v || *end || n < MP_MIN || n > MP_MAX) { *bad = 1; return MP_DEFAULT; }
    return (int)n;
}
// peers = connections the host has right now, the joining one included (the engine adds it before PreLogin). 1 = refuse.
static int mp_refuse(int cap, int peers) { return cap > 0 && 1 + peers > cap; }
// the menu's cycle: 2, 3, ... 8, off, 2 ...   (values above 8 stay reachable through config.txt)
static int mp_next(int cap) { return cap == 0 ? MP_MIN : cap >= 8 ? 0 : cap + 1; }
#endif

// netrole.inc - API 2.26 net_role. The rule is netrole.h.
// Reads engine pointers only (no ProcessEvent): any thread works, the game thread gives a stable answer during a travel.
#include "netrole.h"
static int g_nr_last = -2;
static int api_p_net_role(const AbsApi *self) {
    int r = NR_NONE;
    __try {
        void *w = world(), *nd = w ? *(void **)((uint8_t *)w + OFF_W_NETDRIVER) : NULL, *gm = w ? *(void **)((uint8_t *)w + OFF_W_AUTHGM) : NULL;
        if (nd && !object_is_a(nd, api_cls_netdriver())) nd = NULL;
        void *sc = nd ? *(void **)((uint8_t *)nd + OFF_ND_SERVERCONN) : NULL;
        r = nr_role(w != NULL, nd != NULL, sc != NULL, nd ? L_is_joiner() : 0, gm != NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("net_role: EXCEPTION 0x%08lX reading the world - none", GetExceptionCode()); r = NR_NONE; }
    if (r != g_nr_last) { g_nr_last = r; logf_("net_role: %s (asked by %s)", nr_name(r), plug_name_of(plug_of(self))); }   // one line per change, not per call
    return r;
}
LN_API(netrole) { a->net_role = api_p_net_role; }

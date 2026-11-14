// kindstate.inc - weapon-kind state per character and per kind. Included by weaponslot.inc. Pure logic + key names: kindstate.h.
// Runtime table + persistent keys and their legacy fallbacks: kindstate.h.
#include "kindstate.h"
static KsChar g_ks[KS_MAX]; static unsigned g_ks_gen;
static KsChar *ks_char(void *ch, int create) {
    if (ch && ch == cd_local_pawn()) ch = NULL;
    if (g_ks_gen != g_se_world_gen) { g_ks_gen = g_se_world_gen; int n = ks_drop_others(g_ks); if (n) logf_("kindstate: new world: %d character entr%s dropped", n, n == 1 ? "y" : "ies"); }
    KsChar *k = ks_get(g_ks, ch, create); if (!k && create) { static int n; if (n++ < 10) logf_("kindstate: table full (%d characters) - character %p has no kind state", KS_MAX, ch); }
    return k;
}
static int ks_kind_get(const char *kind, const char *what, const char *legacy, char *out, int cap) {   // 1 = own key, 2 = legacy key, 0 = none
    char k[200]; out[0] = 0; if (ks_key(k, sizeof k, kind, what) && sc_read_key(k, out, cap) && out[0]) return 1;
    if (legacy && sc_read_key(legacy, out, cap) && out[0]) return 2; out[0] = 0; return 0;
}
static void ks_kind_set(const char *kind, const char *what, const char *v) { char k[200]; if (ks_key(k, sizeof k, kind, what)) sc_write_key(k, v); else logf_("kindstate: unusable kind id '%s'", kind ? kind : "?"); }
// the base family's deck for its other weapons; the "legacy.sword_deck" fallback applies to Sword (family 1) only
static int ks_family_get(int fam, char *out, int cap) { char k[40]; out[0] = 0; if (ks_family_key(k, sizeof k, fam) && sc_read_key(k, out, cap) && out[0]) return 1; if (fam == 1 && sc_read_key("legacy.sword_deck", out, cap) && out[0]) return 2; out[0] = 0; return 0; }
static void ks_family_set(int fam, const char *v) { char k[40]; if (ks_family_key(k, sizeof k, fam)) sc_write_key(k, v); }

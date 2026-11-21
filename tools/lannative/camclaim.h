// camclaim.h - the rule behind API 2.28 camera_claim / camera_release / camera_holder (docs/API-REFERENCE.md).
// One claim per plugin (claiming again updates it). The view belongs to the claim with the highest priority; equal priorities: the newest claim.
// Pure code. camclaim.inc keeps the player's view target on the winner every frame.
#ifndef LANNATIVE_CAMCLAIM_H
#define LANNATIVE_CAMCLAIM_H
#define CC_MAX 64
typedef struct { int plugin; void *target; int priority; float blend; unsigned seq; } CcClaim;
typedef struct { CcClaim c[CC_MAX]; int n; unsigned seq; } CcTable;

static int cc_find(const CcTable *t, int plugin) { for (int i = 0; i < t->n; i++) if (t->c[i].plugin == plugin) return i; return -1; }
// add or update the plugin's claim; returns its index, -1 = table full / bad arguments
static int cc_set(CcTable *t, int plugin, void *target, int priority, float blend) {
    if (!target || plugin < 0) return -1;
    int i = cc_find(t, plugin);
    if (i < 0) { if (t->n >= CC_MAX) return -1; i = t->n++; t->c[i].plugin = plugin; t->c[i].seq = ++t->seq; }
    else if (t->c[i].target != target || t->c[i].priority != priority) t->c[i].seq = ++t->seq;   // a changed claim counts as new (wins ties again)
    t->c[i].target = target; t->c[i].priority = priority; t->c[i].blend = blend < 0.f ? 0.f : blend;
    return i;
}
static int cc_remove_at(CcTable *t, int i) { if (i < 0 || i >= t->n) return 0; t->c[i] = t->c[--t->n]; return 1; }
static int cc_remove(CcTable *t, int plugin) { return cc_remove_at(t, cc_find(t, plugin)); }
// index of the claim that has the view, -1 = none
static int cc_top(const CcTable *t) {
    int b = -1;
    for (int i = 0; i < t->n; i++) if (b < 0 || t->c[i].priority > t->c[b].priority || (t->c[i].priority == t->c[b].priority && t->c[i].seq > t->c[b].seq)) b = i;
    return b;
}
// camera_holder's answer for a plugin: 1 = its claim has the view, 0 = it has a claim that waits, -1 = no claim
static int cc_state(const CcTable *t, int plugin) { int i = cc_find(t, plugin), b = cc_top(t); return i < 0 ? -1 : i == b ? 1 : 0; }
#endif

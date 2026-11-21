// modeclaim.h - the rules behind API 2.31 mode_claim / mode_release / mode_holder.
// One owner per pawn: a plugin claims a pawn for its mode ("ride") and picks what it takes from the game with MC_* flags
// (MOVEMENT: the CMC tick off; COLLISION: the capsule to query-only; POSE: the AnimBP update paused). Everything the claim changes is saved
// first and put back exactly on release. Pure code. modeclaim.inc does the engine side.
#ifndef LANNATIVE_MODECLAIM_H
#define LANNATIVE_MODECLAIM_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MC_MOVEMENT = 1, MC_COLLISION = 2, MC_POSE = 4, MC_ALL = 7 };   // = ABS_MODE_* in AbsolverSE.h
#define MC_MAX 16
#define MC_CHANNELS 32

// what a claim saved (and what a release reads back to prove the restore). have = the MC_* groups that were read.
typedef struct {
    unsigned have;
    int tick_on;                       // MOVEMENT: CMC IsComponentTickEnabled
    uint8_t move_mode, custom_mode;    // MOVEMENT: MovementMode / CustomMovementMode (not restored as is: see mc_release_mode)
    float vel[3];                      // MOVEMENT: Velocity (log only; the claiming plugin's last velocity is handed to the CMC as it is)
    uint64_t profile;                  // COLLISION: capsule GetCollisionProfileName (FName)
    uint8_t coll_enabled, obj_type;    // COLLISION: GetCollisionEnabled / GetCollisionObjectType
    uint8_t resp[MC_CHANNELS];         // COLLISION: GetCollisionResponseToChannel 0..31
    int pause_anims;                   // POSE: mesh bPauseAnims
    int auto_only, auto_is;            // POSE (log only): mesh bOnlyAllowAutonomousTickPose / bIsAutonomousTickPose
} McSnap;

typedef struct { int plugin; void *pawn; unsigned flags; char mode[32]; McSnap saved; unsigned gen; } McClaim;
typedef struct { McClaim c[MC_MAX]; int n; } McTable;

static int mc_flags_ok(unsigned f) { return f && !(f & ~(unsigned)MC_ALL); }
static int mc_find(const McTable *t, const void *pawn) { for (int i = 0; i < t->n; i++) if (t->c[i].pawn == pawn) return i; return -1; }
static int mc_remove_at(McTable *t, int i) { if (i < 0 || i >= t->n) return 0; t->c[i] = t->c[--t->n]; return 1; }

// what a mode_claim call does
enum { MC_NEW = 1, MC_UPDATE = 2, MC_SAME = 3, MC_BUSY = -1, MC_FULL = -2, MC_BAD = -3 };
typedef struct { int what, index; unsigned apply, restore; } McDecision;   // apply: groups to save + take now; restore: groups to give back now
static McDecision mc_decide(const McTable *t, int plugin, const void *pawn, unsigned flags) {
    McDecision d = { MC_BAD, -1, 0, 0 };
    if (plugin < 0 || !pawn || !mc_flags_ok(flags)) return d;
    int i = mc_find(t, pawn); d.index = i;
    if (i < 0) { if (t->n >= MC_MAX) { d.what = MC_FULL; return d; } d.what = MC_NEW; d.apply = flags; return d; }
    if (t->c[i].plugin != plugin) { d.what = MC_BUSY; return d; }
    unsigned old = t->c[i].flags; d.apply = flags & ~old; d.restore = old & ~flags; d.what = d.apply || d.restore ? MC_UPDATE : MC_SAME;
    return d;
}

// the movement mode to give back on release (EMovementMode: 0 None 1 Walking 2 NavWalking 3 Falling 4 Swimming 5 Flying 6 Custom).
// Ground modes become Falling: the pawn is wherever the plugin left it, so the CMC lands it by itself (with bForceNextFloorCheck) instead of
// snapping it to a floor; the other modes come back as they were (custom with its sub-mode).
static uint8_t mc_release_mode(uint8_t saved) { return saved == 1 || saved == 2 || saved == 3 ? 3 : saved; }

// channels whose response differs (after the profile was set back): their indexes into out, returns the count
static int mc_resp_diff(const uint8_t saved[MC_CHANNELS], const uint8_t now[MC_CHANNELS], int out[MC_CHANNELS]) {
    int n = 0; for (int c = 0; c < MC_CHANNELS; c++) if (saved[c] != now[c]) out[n++] = c; return n;
}

// fields of the groups in `groups` that differ between a (saved) and b (read back after the restore); "name a->b" list into buf.
// Only exactly restored fields count (movement mode / velocity / the autonomous bits are not compared). 0 = restored exactly.
static int mc_snap_diff(const McSnap *a, const McSnap *b, unsigned groups, char *buf, int cap) {
    int n = 0, w = 0; if (buf && cap > 0) buf[0] = 0;
    // append "item" (", " between items); w never passes cap - 1, so the text is cut, never overrun
#define MC_D_(cond, ...) do { if (cond) { n++; if (buf && w < cap - 1) { char it_[64]; snprintf(it_, sizeof it_, __VA_ARGS__); int k_ = snprintf(buf + w, (size_t)(cap - w), "%s%s", w ? ", " : "", it_); w = k_ < 0 ? w : w + k_ > cap - 1 ? cap - 1 : w + k_; } } } while (0)
    if (groups & MC_MOVEMENT) MC_D_(a->tick_on != b->tick_on, "cmc tick %d->%d", a->tick_on, b->tick_on);
    if (groups & MC_COLLISION) {
        MC_D_(a->profile != b->profile, "profile %llx->%llx", (unsigned long long)a->profile, (unsigned long long)b->profile);
        MC_D_(a->coll_enabled != b->coll_enabled, "enabled %u->%u", a->coll_enabled, b->coll_enabled);
        MC_D_(a->obj_type != b->obj_type, "object type %u->%u", a->obj_type, b->obj_type);
        for (int c = 0; c < MC_CHANNELS; c++) MC_D_(a->resp[c] != b->resp[c], "channel %d %u->%u", c, a->resp[c], b->resp[c]);
    }
    if (groups & MC_POSE) MC_D_(a->pause_anims != b->pause_anims, "bPauseAnims %d->%d", a->pause_anims, b->pause_anims);
#undef MC_D_
    return n;
}

// the claim's snapshot as one log line ("tick 1 mode 1/0 vel 0,0,0 | profile ... | pause 0 auto 0/1")
static void mc_snap_text(const McSnap *s, char *buf, int cap) {
    if (!buf || cap <= 0) return;
    char r[MC_CHANNELS + 1]; for (int c = 0; c < MC_CHANNELS; c++) r[c] = (char)('0' + (s->resp[c] < 10 ? s->resp[c] : 9)); r[MC_CHANNELS] = 0;
    char m[96] = "", k[160] = "", p[64] = "";
    if (s->have & MC_MOVEMENT) snprintf(m, sizeof m, "cmc tick %d mode %u/%u vel %.0f,%.0f,%.0f", s->tick_on, s->move_mode, s->custom_mode, s->vel[0], s->vel[1], s->vel[2]);
    if (s->have & MC_COLLISION) snprintf(k, sizeof k, "profile %llx enabled %u object type %u responses %s", (unsigned long long)s->profile, s->coll_enabled, s->obj_type, r);
    if (s->have & MC_POSE) snprintf(p, sizeof p, "bPauseAnims %d autonomous only/is %d/%d", s->pause_anims, s->auto_only, s->auto_is);
    snprintf(buf, (size_t)cap, "%s%s%s%s%s", m, m[0] && (k[0] || p[0]) ? " | " : "", k, k[0] && p[0] ? " | " : "", p);
}
#endif

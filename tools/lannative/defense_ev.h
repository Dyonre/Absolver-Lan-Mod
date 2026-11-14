// defense_ev.h - pure logic of on_defense (loader glue: defense_ev.inc).
// No game access, no allocation: classification of the game's defence notifications and the "time into the attack" table.
//
// Where each defence comes from.
//   GenerateImpact (@1403ffea0) out flags  guarded / guardBroken / absorbed     -> guard / guard_break / absorb (Kahlt special; guard break wins, then absorb, then guard,
//                                                                                  the same order as on_hit's outcome)
//   UDefenseComponent::NotifyParrySuccessful(this = the ATTACKER's defense component, delay, actor = the PARRIER, FName attack, order id)  (called by HandleParried)
//                                                                               -> parry, or deflect when the parrier's guard is up (a guard-opened parry window:
//                                                                                  a guard-opened deflect; a vanilla parry comes from the special ability)
//   UDefenseComponent::NotifyAvoidSuccessful(this = the DEFENDER's defense component, delay, avoided actor = attacker, EAvoidType, EOrderType, origin order id,
//                                            FName attack, FName executing attack, AvoidPropertyDB)
//                                                                               -> EAvoidType Dodge (4) = dodge; Duck / Jump / LeftSide / RightSide (0-3, Windfall),
//                                                                                  AllHitAvoid (5), DrunkenSouth (6, Stagger) = avoid
#ifndef DEFENSE_EV_H
#define DEFENSE_EV_H
#include <string.h>

enum { DE_GUARD, DE_GUARD_BREAK, DE_ABSORB, DE_PARRY, DE_DEFLECT, DE_DODGE, DE_AVOID, DE_N };      // = ABS_DEF_* in AbsolverSE.h
static const char *const de_names[DE_N] = { "guard", "guard_break", "absorb", "parry", "deflect", "dodge", "avoid" };
static const char *de_name(int t) { return t >= 0 && t < DE_N ? de_names[t] : "?"; }

// GenerateImpact result -> defence type, -1 = not a defence (a real hit or no effect)
static int de_from_impact(int guarded, int broken, int absorbed) { return broken ? DE_GUARD_BREAK : absorbed ? DE_ABSORB : guarded ? DE_GUARD : -1; }
// EAvoidType -> dodge / avoid, -1 = not a valid avoid type (Count 7, None 8, garbage)
#define DE_AVOID_DODGE 4
static int de_from_avoid(int avoid_type) { return avoid_type == DE_AVOID_DODGE ? DE_DODGE : avoid_type >= 0 && avoid_type <= 6 ? DE_AVOID : -1; }
// parry: guard up at the moment of the parry = deflect (guard-opened window), else the special-ability parry
static int de_from_parry(int parrier_guarding) { return parrier_guarding ? DE_DEFLECT : DE_PARRY; }

// ---- "time into the attack": the last attack start per attacker (on_attack_start), looked up when a defence happens.
// 64 slots, linear scan: fights have a handful of characters.
#define DE_STARTS 64
#define DE_STALE_S 10.0          // an attack older than this is not "the" attack being defended (no attack lasts that long)
#define DE_FPS 30.0              // combat frame data is in 1/30 s frames
typedef struct { const void *ch; char attack[32]; double t; } DeStart;
typedef struct { DeStart s[DE_STARTS]; } DeStarts;
static void de_start_note(DeStarts *d, const void *ch, const char *attack, double t) {
    if (!ch || !attack) return;
    int k = -1, oldest = 0;
    for (int i = 0; i < DE_STARTS; i++) {
        if (d->s[i].ch == ch) { k = i; break; }
        if (k < 0 && !d->s[i].ch) k = i;
        if (d->s[i].t < d->s[oldest].t) oldest = i;
    }
    if (k < 0) k = oldest;                                     // full: the oldest entry goes
    d->s[k].ch = ch; strncpy(d->s[k].attack, attack, sizeof d->s[k].attack - 1); d->s[k].attack[sizeof d->s[k].attack - 1] = 0; d->s[k].t = t;
}
// seconds since `ch` started `attack`, -1 = unknown (no start seen, another attack started since, or stale)
static double de_start_age(const DeStarts *d, const void *ch, const char *attack, double now) {
    if (!ch || !attack) return -1;
    for (int i = 0; i < DE_STARTS; i++) if (d->s[i].ch == ch) {
        double a = now - d->s[i].t;
        return strcmp(d->s[i].attack, attack) || a < 0 || a > DE_STALE_S ? -1 : a;
    }
    return -1;
}
static void de_start_drop(DeStarts *d, const void *ch) { for (int i = 0; i < DE_STARTS; i++) if (d->s[i].ch == ch) memset(&d->s[i], 0, sizeof d->s[i]); }
static double de_frames(double sec) { return sec < 0 ? -1 : sec * DE_FPS; }
#endif

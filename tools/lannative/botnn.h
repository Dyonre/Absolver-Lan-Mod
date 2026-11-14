// botnn.h - observation vector of the neural-network bot, built from a BotObs (filled by the game adapter, botai.inc).
// The layout must match the one the loaded policy was trained with (NN_OBS_VERSION). Pure C, no game access, no allocation.
// Human limits are applied BEFORE this (the bot reacts to what it sees, never to inputs): the opponent side passed in is the reaction-delayed snapshot and
// bo_perceive() has hidden its move for the first reveal_f frames; this header only normalises.
#ifndef BOTNN_H
#define BOTNN_H
#include "botobs.h"

#define NN_OBS 128          // fixed vector length (118 used, padded with 0); bump NN_OBS_VERSION when the layout changes
#define NN_OBS_VERSION 5   // layout version of the vector below
// what BotObs does not carry: equipped powers, timed effects, the match score (filled by the adapter from its own state)
typedef struct {
    int power_kind[2];      // bitmask: 1 defensive window, 2 acts on the opponent, 4 self buff, 8 makes space (0 = empty slot)
    float power_cost[2];    // shards
    float power_cd_left[2]; // seconds until usable again
    float wpn_cost;         // shards to draw the equipped weapon (0 = none)
    float effects[6];       // seconds left: gravity, exhaust, silence (on this side), light heal, sharp impact, unbreakable (own buffs)
    float rounds_won;       // in the running first-to-3 match
    float geo[10];          // 2D (own side only): [8] walk slow on, [9] rush on; sin / cos of (my facing - bearing to the opponent), sin / cos of (its facing - bearing to me),
                            // closing speed / 300, lateral speed (+ = it moves to my left) / 300, my speed / 400, distance to the arena edge / 1000 (0 = unknown)
} NnExtra;

static int nn_put_row(float *o, int k, const BotSide *s, int known) {
    // the running attack of a side: its frame data (only when the observer can know it: own attack, or the opponent's after reveal_f)
    const BotRow *r = &s->r; int on = known && bo_attacking(s) && r->ok;
    o[k++] = on ? 1.f : 0.f;
    o[k++] = on ? (float)r->startup / 30.f : 0.f;
    o[k++] = on ? (float)r->strike / 30.f : 0.f;
    o[k++] = on ? (float)r->release / 60.f : 0.f;
    o[k++] = on ? (float)r->cancel / 30.f : 0.f;
    for (int m = 0; m < 3; m++) o[k++] = on && r->motion == m ? 1.f : 0.f;          // thrust / horizontal / vertical
    for (int m = 0; m < 3; m++) o[k++] = on && r->origin == m ? 1.f : 0.f;          // left / right / middle
    o[k++] = on ? r->range / 400.f : 0.f;
    o[k++] = on ? (float)r->adv_hit / 30.f : 0.f;
    o[k++] = on ? (float)r->adv_guard / 30.f : 0.f;
    o[k++] = on ? bo_frames_to_hit(s) / 30.f : 0.f;
    return k;
}
static int nn_put_side(float *o, int k, const BotSide *s, int own) {
    float mh = bo_v(s, BO_MAXHEALTH), mg = bo_v(s, BO_MAXGUARD);
    o[k++] = s->valid ? 1.f : 0.f;
    o[k++] = mh > 0 ? bo_v(s, BO_HEALTH) / mh : 0.f;
    o[k++] = mg > 0 ? bo_v(s, BO_GUARD) / mg : 0.f;
    o[k++] = own && bo_v(s, BO_SHARDS) > 0 ? bo_v(s, BO_SHARDS) / 5.f : 0.f;   // the opponent's shards are not shown in the game
    int q = bo_stance(s); for (int i = 0; i < 4; i++) o[k++] = q == i ? 1.f : 0.f;
    o[k++] = bo_v(s, BO_ATTACKING) > 0 ? 1.f : 0.f;                                  // also "an attack started" when its move is still hidden
    o[k++] = bo_v(s, BO_ATTACKING) > 0 ? s->frame / 60.f : 0.f;
    int ph = bo_phase(s); for (int i = 1; i < 4; i++) o[k++] = ph == i ? 1.f : 0.f;
    o[k++] = (float)s->gold_open; o[k++] = (float)s->intr_open; o[k++] = (float)s->ability_ok; o[k++] = (float)s->stunned;
    o[k++] = bo_v(s, BO_DOWN) > 0 ? 1.f : 0.f; o[k++] = (float)s->locked_on; o[k++] = bo_v(s, BO_GUARDING) > 0 ? 1.f : 0.f;
    o[k++] = (float)s->has_weapon; o[k++] = (float)s->weapon_out; o[k++] = s->wdur > 0 ? s->wdur : 0.f;
    return k;
}
static int nn_put_extra(float *o, int k, const NnExtra *x, int own) {
    for (int i = 0; i < 2; i++) { for (int b = 0; b < 4; b++) o[k++] = own && (x->power_kind[i] >> b & 1) ? 1.f : 0.f;   // the opponent's powers are not seen (only their effects)
        o[k++] = own ? x->power_cost[i] / 5.f : 0.f; o[k++] = own ? x->power_cd_left[i] / 6.f : 0.f; }
    o[k++] = own ? x->wpn_cost / 5.f : 0.f;
    for (int i = 0; i < 6; i++) o[k++] = x->effects[i] / 20.f;
    o[k++] = x->rounds_won / 3.f;
    if (own) for (int i = 0; i < 10; i++) o[k++] = x->geo[i];   // 118 + 10 = 128 = NN_OBS
    return k;
}
// me / op: BotSide's as the observer sees them (op already delayed + perceived); round_t: seconds since the round started. Returns the number written (<= NN_OBS).
static int nn_obs(const BotSide *me, const BotSide *op, float dist, const NnExtra *xme, const NnExtra *xop, float round_t, float *out) {
    int k = 0; for (int i = 0; i < NN_OBS; i++) out[i] = 0.f;
    k = nn_put_side(out, k, me, 1); k = nn_put_row(out, k, me, 1);
    k = nn_put_side(out, k, op, 0); k = nn_put_row(out, k, op, op->row[0] != 0);
    k = nn_put_extra(out, k, xme, 1); k = nn_put_extra(out, k, xop, 0);
    out[k++] = dist / 1000.f; out[k++] = round_t / 60.f;
    return k;
}
#endif

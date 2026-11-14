/* ranks.h - pure helpers of the "ranks" content kind and the progression API (bots.inc): the XP curve, the next threshold, the stock 1v1 trial reward.
 * No game access. */
#ifndef RANKS_H
#define RANKS_H
#include <math.h>

/* DB/Stats/XP/RequiredGlobalXPForLevel: keys (1,2000) (20,6000) (30,10000) (40,17000) (60,20000), linear, constant outside. It is the per-level INCREMENT. */
static double rk_curve(int level) {
    static const int k[5][2] = { { 1, 2000 }, { 20, 6000 }, { 30, 10000 }, { 40, 17000 }, { 60, 20000 } };
    if (level <= k[0][0]) return k[0][1];
    if (level >= k[4][0]) return k[4][1];
    for (int i = 0; i < 4; i++) if (level <= k[i + 1][0]) return k[i][1] + (level - k[i][0]) * (double)(k[i + 1][1] - k[i][1]) / (k[i + 1][0] - k[i][0]);
    return k[4][1];
}
/* BPStatsComponent.BPF_GetRequiredXPForLevel: CUMULATIVE XP at which `level` is reached. Req(<=1) = 2000, Req(L) = Req(L-1) + 50 * round(curve(L) * 0.02) (round half up).
 * Req(60) = 672,000; Req(61) = 692,000 = one Prestige grade. Levels above 61 keep adding 20,000 (the curve is constant). */
static int rk_required_xp(int level) {
    int xp = 2000; for (int l = 2; l <= level; l++) xp += 50 * (int)floor(rk_curve(l) * 0.02 + 0.5);
    return xp;
}
/* the level (0..60) the game's loop settles on for a running XP total: the highest L with Req(L) <= xp */
static int rk_level_for_xp(int xp) { int l = 0; while (l < 60 && rk_required_xp(l + 1) <= xp) l++; return l; }
/* XP total at which the next step is reached: below level 60 (no Prestige grade yet) the next level, else the next Prestige level
 * (Prestige level 60 -> Req(61): the next grade, where the game subtracts Req(61) again). */
static int rk_next_xp(int level, int grade, int prestige_level) {
    if (grade <= 0 && level < 60) return rk_required_xp(level + 1);
    return rk_required_xp((prestige_level < 0 ? 0 : prestige_level) + 1);
}

/* a reward table: [0] finish, [1] victory, [2] performance (needs min_rounds rounds won), [3] mentoring. points feed AddGameModeXP, xp BPF_AddGlobalXP. */
typedef struct { int pts[4], xp[4], min_rounds; double xp_scale; int gm_points; } RkTable;
/* 1V1_GameState_C.BPE_GetRewards (stock): 1 / 500, 1 / 4000, 1 / 1500 for 2+ rounds won, 1 / 500 mentoring */
static void rk_stock(RkTable *t) {
    static const int p[4] = { 1, 1, 1, 1 }, x[4] = { 500, 4000, 1500, 500 };
    for (int i = 0; i < 4; i++) { t->pts[i] = p[i]; t->xp[i] = x[i]; }
    t->min_rounds = 2; t->xp_scale = 1.0; t->gm_points = 1;
}
/* XP of one finished trial (rounded to whole XP after xp_scale); *points = game-mode points (0 when the table has gm_points off) */
static int rk_reward(const RkTable *t, int won, int rounds_won, int mentoring, int *points) {
    int x = 0, p = 0, got[4] = { 1, won != 0, rounds_won >= t->min_rounds, mentoring != 0 && won != 0 };
    for (int i = 0; i < 4; i++) if (got[i]) { x += t->xp[i]; p += t->pts[i]; }
    if (points) *points = t->gm_points ? p : 0;
    return (int)floor(x * t->xp_scale + 0.5);
}
/* Rift Coins a points gain gives: one per game-mode level crossed (GameModeProgressionDB.m_iXPRequiredPerLevel = 5) */
static int rk_rift_coins(int gm_xp_before, int points, int per_level) {
    if (per_level <= 0 || points <= 0 || gm_xp_before < 0) return 0;
    return (gm_xp_before + points) / per_level - gm_xp_before / per_level;
}
#endif

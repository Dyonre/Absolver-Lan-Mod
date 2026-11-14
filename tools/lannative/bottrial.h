/* bottrial.h - pure helpers of the "bot_trials" content kind (bottrial.inc): round / stage bookkeeping of a solo bot trial (one bot, or a gauntlet of bots in a row),
 * the AI level of each stage, HUD texts, the m_Scores text that feeds the stock team score. No game access. */
#ifndef BOTTRIAL_H
#define BOTTRIAL_H
#include <stdio.h>
#include <string.h>

#define BTR_BOTS_MAX 8
/* a trial in progress: stage = index of the current bot (gauntlet), p / b = rounds won in this stage, tp / tb = over the whole trial */
typedef struct { int stages, rounds_to_win, stage, p, b, tp, tb, done, won; } BtrMatch;
enum { BTR_NEXT_ROUND = 0, BTR_NEXT_STAGE = 1, BTR_TRIAL_WON = 2, BTR_TRIAL_LOST = 3, BTR_IGNORED = -1 };

static void btr_init(BtrMatch *m, int stages, int rounds_to_win) {
    memset(m, 0, sizeof *m);
    m->stages = stages < 1 ? 1 : stages > BTR_BOTS_MAX ? BTR_BOTS_MAX : stages;
    m->rounds_to_win = rounds_to_win < 1 ? 1 : rounds_to_win > 9 ? 9 : rounds_to_win;
}
/* a round ended: player_won = 1 (the bot was knocked out) or 0 (the player died). Returns what comes next. A finished trial ignores further rounds. */
static int btr_round(BtrMatch *m, int player_won) {
    if (m->done) return BTR_IGNORED;
    if (player_won) { m->p++; m->tp++; } else { m->b++; m->tb++; }
    if (m->b >= m->rounds_to_win) { m->done = 1; m->won = 0; return BTR_TRIAL_LOST; }
    if (m->p >= m->rounds_to_win) {
        if (m->stage + 1 >= m->stages) { m->done = 1; m->won = 1; return BTR_TRIAL_WON; }
        m->stage++; m->p = m->b = 0; return BTR_NEXT_STAGE;
    }
    return BTR_NEXT_ROUND;
}
/* AI level of stage i: base + i * step, clamped to 0..100 (gauntlet scaling) */
static int btr_level(int base, int step, int stage) { int v = base + step * stage; return v < 0 ? 0 : v > 100 ? 100 : v; }
/* score line: "You 2 - 1 Duelist" (+ "   bot 2/3" in a gauntlet) */
static int btr_score_text(const BtrMatch *m, const char *bot_name, char *out, int cap) {
    int n = snprintf(out, cap, "You %d - %d %s", m->p, m->b, bot_name && *bot_name ? bot_name : "Bot");
    if (m->stages > 1 && n >= 0 && n < cap) n += snprintf(out + n, cap - n, "   bot %d/%d", m->stage + 1, m->stages);
    return n;
}
/* the stock AAdversarialPlayerState::m_Scores text for `kills` points (TArray<FPlayerKill>: victim player state + count). BPF_GetTeamScore sums m_uiNbKills over the
 * team's player states (AAdversarialGameState::BPF_GetTeamScore -> EnumerateTeamPlayers lambda), so ONE entry with the count is enough.
 * kills 0 = "()" (tr_write_score also writes the count directly). */
static int btr_scores_text(int kills, char *out, int cap) {
    if (kills <= 0) return snprintf(out, cap, "()");
    return snprintf(out, cap, "((m_uiNbKills=%d))", kills);
}
/* end banner, fake-player duel wording (never "failed" / "lost"): "VICTORY   You 3 - 1 Duelist   +6000 XP" / "Duelist wins 3 - 1   +500 XP" */
static int btr_end_text(const BtrMatch *m, const char *bot_name, int xp, char *out, int cap) {
    const char *b = bot_name && *bot_name ? bot_name : "Bot";
    return m->won ? snprintf(out, cap, "VICTORY   You %d - %d %s   +%d XP", m->tp, m->tb, b, xp) : snprintf(out, cap, "%s wins %d - %d   +%d XP", b, m->tb, m->tp, xp);
}
/* the bot's spawn point, like a second player: among the map's player starts (Respawn_PVP_C), the "first spawn" one farthest from the player (the other team's start);
 * no first-spawn start -> the farthest of any. -1 = none at least min_dist away (the caller falls back to "spawn_distance in front of the player"). */
static int btr_pick_start(const float (*loc)[3], const int *first, int n, const float player[3], float min_dist) {
    int best = -1; float bd = -1; int pass_first = 1;
    for (int pass = 0; pass < 2 && best < 0; pass++, pass_first = 0)
        for (int i = 0; i < n; i++) {
            if (pass_first && !first[i]) continue;
            float dx = loc[i][0] - player[0], dy = loc[i][1] - player[1], d = dx * dx + dy * dy;
            if (d >= min_dist * min_dist && d > bd) { bd = d; best = i; }
        }
    return best;
}
/* approach mode: 0 = nothing (dead, attacking, already within `stop` or out of reach), 1 = RUN: beyond lock_range, a move to the player
 * (AIController.MoveToActor), 2 = LOCK-MOVE: within lock_range, the bot locks onto the player (SetTarget) and closes in
 * with movement input until `stop` (95 uu: the attack range; from much farther away a bot never attacks) */
enum { BTR_AP_NONE = 0, BTR_AP_RUN = 1, BTR_AP_LOCK = 2 };
static int btr_approach(float d, int attacking, int dead, float lock_range, float stop) {
    if (dead || attacking > 0 || d < 0 || d < stop || d > 5000.f) return BTR_AP_NONE;
    return d > lock_range ? BTR_AP_RUN : BTR_AP_LOCK;
}
/* the "re-spawn near the player" fallback for a blocked bot: at most ONCE per round, only while the bot is still on its run-in from the spawn (never engaged = never
 * came within the lock range), and only when the bot's OWN position has not moved BTR_STILL_UU for BTR_STILL_S - a player who retreats leaves a moving bot, never a blocked one.
 * Otherwise the bot keeps chasing (MoveToActor / lock-move). bot_moved: 2D distance of the bot from its last progress mark (< 0 = unknown), still_s: seconds since that mark. */
#define BTR_STILL_UU 60.f
#define BTR_STILL_S 1.5
#define BTR_BLOCK_MIN_D 600.f   /* closer than this the bot is slow to start, not blocked */
static int btr_block_respawn(int used_this_round, int engaged, float bot_moved, double still_s, float d) {
    return !used_this_round && !engaged && d >= BTR_BLOCK_MIN_D && bot_moved >= 0 && bot_moved < BTR_STILL_UU && still_s >= BTR_STILL_S;
}
/* map path -> the short name a world is named after ("/Game/Maps/.../OPTI_GM_1v1_City_Coliseum" or "...Name.Name" -> "OPTI_GM_1v1_City_Coliseum") */
static int btr_map_short(const char *path, char *out, int cap) {
    out[0] = 0; if (!path) return 0;
    const char *s = strrchr(path, '/'); s = s ? s + 1 : path;
    int n = 0; while (s[n] && s[n] != '.' && s[n] != '?') n++;
    return snprintf(out, cap, "%.*s", n, s);
}
/* 1 if the world object name (short name of the UWorld) is the map of the trial */
static int btr_is_map(const char *world_name, const char *map_path) {
    char s[160]; if (!world_name || !*world_name || !btr_map_short(map_path, s, sizeof s)) return 0;
    return !strcmp(world_name, s);
}
#endif

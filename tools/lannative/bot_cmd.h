// bot_cmd.h - pure command queue of the scripted bot. No game, no Windows, no allocation.
// Only botai.inc (the adapter) touches the game.
//
// A brain (botcore.h) decides WHAT to do; it pushes a BotCmd with the time it SAW the trigger. The queue adds the bot's human knobs:
//   reaction delay  : due = seen + max(floor, N(mean, sd))            (BotSkill react_*; superhuman = 0 / 0 / 0)
//   execution error : due += N(0, exec_err_f) frames                  (sloppy timing of presses)
//   gold miss       : with probability gold_miss the gold press is pushed late by gold_miss_late_f frames (a human misses the window)
// The adapter pops due commands every tick (bc_pop_due) and executes them through the game's own orders.
//
// Gold-link presses: the bot cannot write into the AI pawn's input stack, so the adapter emulates the player's buffer rules with bc_gold_resolve:
//   a press at time p makes a gold link when the gold window (layer PerfectAttackOnly) is open at p, or opens within BC_GOLD_BUFFER_S (0.08 s) after p;
//   a press after the window (or too early) is a plain link if the interruptible window (GenericAll) opens within BC_PLAIN_BUFFER_S (0.35 s); else it is lost.
#ifndef BOT_CMD_H
#define BOT_CMD_H
#include <string.h>
#include <math.h>
#include <stdint.h>

#define BC_FRAME 0.0333333333                   // 1 combat frame (30 F/s)
#define BC_GOLD_BUFFER_S 0.08                   // Attack1Perfect / Attack2Perfect lifespan in the input stack
#define BC_PLAIN_BUFFER_S 0.35                  // Attack1 / Attack2 lifespan
#define BC_QCAP 32

enum { BC_NONE = 0, BC_ATTACK, BC_GUARD, BC_UNGUARD, BC_ABILITY, BC_DODGE, BC_STANCE, BC_POWER, BC_LOCK, BC_UNLOCK, BC_MOVE, BC_STOP, BC_DRAW, BC_TYPES };   // BC_DRAW: draw / sheathe toggle
static const char *const BC_NAMES[BC_TYPES] = { "none", "attack", "guard", "unguard", "ability", "dodge", "stance", "power", "lock", "unlock", "move", "stop", "draw" };

// one command. attack: input 0 = Attack1 (main chain), 1 = Attack2 (alternate); row "" = the bot's own deck (combo manager, like a player), else a forced row id;
//   gold 1 = press for a gold link (offset_f = press time relative to the predicted window opening, frames; negative = early); feint 1 = the attack is a feint (cancelled at its cancel frame).
// dodge: dir 0 back, 1 left, 2 right, 3 forward (relative to the opponent). stance: quad 0 FL 1 FR 2 BR 3 BL. power: slot 0 left / 1 right. move: dir as dodge, secs = duration.
typedef struct { int type; double seen, due; int input, gold, feint, quad, dir, slot; float offset_f, secs; char row[24]; char tech[8]; uint32_t id; } BotCmd;
typedef struct { BotCmd q[BC_QCAP]; int n; uint32_t next_id; long long dropped; } BotQueue;
typedef struct { float react_mean_ms, react_sd_ms, react_floor_ms, exec_err_f, gold_miss, gold_miss_late_f; } BotSkill;

// ----- rng (xorshift64*), uniform [0,1), gaussian (Box-Muller)
typedef struct { uint64_t s; int has; double spare; } BotRng;
static void bc_rng_seed(BotRng *r, uint64_t seed) { r->s = seed ? seed : 0x9E3779B97F4A7C15ull; r->has = 0; }
static double bc_rand(BotRng *r) { r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27; return (double)((r->s * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0; }
static double bc_gauss(BotRng *r) {
    if (r->has) { r->has = 0; return r->spare; }
    double u = bc_rand(r), v = bc_rand(r); if (u < 1e-12) u = 1e-12;
    double m = sqrt(-2.0 * log(u)); r->spare = m * sin(6.283185307179586 * v); r->has = 1; return m * cos(6.283185307179586 * v);
}
// reaction delay in seconds for one decision
static double bc_reaction(const BotSkill *k, BotRng *r) {
    double ms = k->react_mean_ms + (k->react_sd_ms > 0 ? bc_gauss(r) * k->react_sd_ms : 0.0);
    if (ms < k->react_floor_ms) ms = k->react_floor_ms; if (ms < 0) ms = 0; return ms / 1000.0;
}
// timing jitter in seconds (execution error)
static double bc_jitter(const BotSkill *k, BotRng *r) { return k->exec_err_f > 0 ? bc_gauss(r) * k->exec_err_f * BC_FRAME : 0.0; }

// ----- queue
static void bc_clear(BotQueue *q) { q->n = 0; }
// push with the knobs: due = c->seen + reaction + jitter (+ late gold miss). react = 0: no reaction delay (a planned follow-up, not a reaction). Returns the id (0 = queue full).
static uint32_t bc_push(BotQueue *q, BotCmd c, const BotSkill *k, BotRng *r, int react) {
    if (q->n >= BC_QCAP) { q->dropped++; return 0; }
    double d = c.seen + (react ? bc_reaction(k, r) : 0.0) + bc_jitter(k, r);
    if (c.type == BC_ATTACK && c.gold && k->gold_miss > 0 && bc_rand(r) < k->gold_miss) c.offset_f += k->gold_miss_late_f;   // deliberate human miss: press late
    if (d < c.seen) d = c.seen; c.due = d; c.id = ++q->next_id; q->q[q->n++] = c; return c.id;
}
// exact time, no knobs
static uint32_t bc_push_at(BotQueue *q, BotCmd c, double at) { if (q->n >= BC_QCAP) { q->dropped++; return 0; } c.due = at; c.id = ++q->next_id; q->q[q->n++] = c; return c.id; }
// the earliest command with due <= now (removed); 1 = got one
static int bc_pop_due(BotQueue *q, double now, BotCmd *out) {
    int best = -1; for (int i = 0; i < q->n; i++) if (q->q[i].due <= now && (best < 0 || q->q[i].due < q->q[best].due)) best = i;
    if (best < 0) return 0; *out = q->q[best]; q->q[best] = q->q[--q->n]; return 1;
}
static int bc_pending(const BotQueue *q, int type) { int n = 0; for (int i = 0; i < q->n; i++) if (type < 0 || q->q[i].type == type) n++; return n; }
static int bc_drop(BotQueue *q, int type) { int n = 0; for (int i = 0; i < q->n;) if (type < 0 || q->q[i].type == type) { q->q[i] = q->q[--q->n]; n++; } else i++; return n; }

// ----- gold press emulation (adapter, per tick while a press is held)
// press: time of the press; now; gold_open / plain_open: the windows are open now (IsAvailable(Attack1Perfect) / IsAvailable(Attack1)).
// Returns BC_LINK_WAIT (keep holding), BC_LINK_GOLD (launch with the perfect flag now), BC_LINK_PLAIN (launch plain now), BC_LINK_LOST (the press expired).
enum { BC_LINK_WAIT = 0, BC_LINK_GOLD = 1, BC_LINK_PLAIN = 2, BC_LINK_LOST = 3 };
static int bc_gold_resolve(double press, double now, int gold_open, int plain_open) {
    double age = now - press; if (age < 0) return BC_LINK_WAIT;
    if (gold_open && age <= BC_GOLD_BUFFER_S + 1e-6) return BC_LINK_GOLD;
    if (plain_open && age <= BC_PLAIN_BUFFER_S + 1e-6) return BC_LINK_PLAIN;
    if (age > BC_PLAIN_BUFFER_S + 1e-6) return BC_LINK_LOST;
    return BC_LINK_WAIT;
}
// press time for a gold link: predicted window opening (attack start + learned delay; learned < 0 = unknown -> -1 = press when the window is seen) + offset frames
static double bc_gold_press_time(double attack_start, double learned_open_after_s, float offset_f) {
    if (learned_open_after_s < 0) return -1.0;
    return attack_start + learned_open_after_s + offset_f * BC_FRAME;
}
// learning the window: exponential average of observed opening delays (first sample taken as is)
static double bc_learn(double old, double sample) { return old < 0 ? sample : old * 0.7 + sample * 0.3; }

#endif

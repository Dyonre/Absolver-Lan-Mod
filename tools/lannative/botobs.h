// botobs.h - pure observation of the bot. Shared by the recorder (inputrec.h side vector) and the bot.
// The first IR_SIDE_N fields of a side are EXACTLY the recorder's side vector (inputrec.h IrSide: health, max health, guard, max guard, stance, style, attacking,
// guarding, down, shards, attack frame), so an imitation model trained on recordings reads the same numbers the live bot sees. Added for the bot:
//   row        the side's running attack row ("" none), its data (BotRow) and the frame inside it (attack frame)
//   windows    gold (PerfectAttackOnly: IsAvailable(Attack1Perfect)) / interruptible (IsAvailable(Attack1)) / ability available, per side
//   locked_on  the side is locked on its opponent (GetTarget == opponent): the back-stance guard hole needs the DEFENDER locked
//   distance   2D, and the derived attack phase of the opponent (startup / active / recovery) + frames until its first strike
// No game access; the adapter (botai.inc) fills it.
#ifndef BOTOBS_H
#define BOTOBS_H
#include <string.h>
#include "inputrec.h"

enum { BO_HEALTH = 0, BO_MAXHEALTH, BO_GUARD, BO_MAXGUARD, BO_STANCE, BO_STYLE, BO_ATTACKING, BO_GUARDING, BO_DOWN, BO_SHARDS, BO_ATKFRAME };
enum { BO_PH_NONE = 0, BO_PH_STARTUP, BO_PH_ACTIVE, BO_PH_RECOVERY };
// attack row data (attacks DataTable, offsets in botai.inc): frames are 30 F/s
typedef struct {
    char name[24]; int ok;
    int startup;        // m_iFrameBuildUpToPlay (played startup; falls back to m_iFrameBuildUp)
    int cancel;         // m_iFrameCancel (< 0 = cannot be feinted)
    int strike;         // hit frame, absolute (= startup; m_iFrameStrike1 counts from the end of the build-up, see bai_row)
    int perfect;        // m_iFramePerfectRelease (gold window opens)
    int release;        // end of the move, absolute: startup + max(m_iFrameRelease, m_iFrameInterruptibleRelease) (fallback +15)
    int irelease;       // m_iFrameInterruptibleRelease
    int start_q, end_q; // m_eStartQuadrant / m_eEndQuadrant (0 FL 1 FR 2 BR 3 BL)
    int motion;         // m_eMovementType1 (EAttackMovementType: 0 Thrust 1 Horizontal 2 Vertical - as the table stores it)
    int origin;         // 0 left 1 right 2 middle (remapped from EAttackOrigin Left 0 Middle 1 Right 2, see bai_row)
    float range;        // m_fRange (uu)
    int adv_hit, adv_guard;   // m_iAdvFrameOnPLHit / m_iAdvFrameOnPLGuard
} BotRow;
typedef struct {
    IrSide s;                     // recorder side vector
    char row[24]; BotRow r; float frame;   // running attack ("" = none) and the frame inside it
    int gold_open, intr_open, ability_ok, locked_on, valid;
    int stunned;                  // nothing is available (no attack, guard or dodge) and no own attack runs: hit stun / knock-down
    unsigned seq;                 // attack-start counter of this side (a new attack = a new number)
    double atk_t;                 // time of that attack start
    int has_weapon, weapon_out;   // a weapon is equipped / it is in hand (weapon deck live)
    float wdur;                   // durability of the weapon, 1 = new, 0 = broken, -1 = unknown
} BotSide;
typedef struct { BotSide me, op; float dist; double t; } BotObs;

static float bo_v(const BotSide *s, int i) { return s->s.v[i]; }
static int bo_attacking(const BotSide *s) { return s->valid && s->s.v[BO_ATTACKING] > 0 && s->row[0]; }
static int bo_stance(const BotSide *s) { int q = (int)s->s.v[BO_STANCE]; return q >= 0 && q <= 3 ? q : -1; }
static int bo_is_back(int q) { return q == 2 || q == 3; }
static float bo_hp_frac(const BotSide *s) { float m = s->s.v[BO_MAXHEALTH]; return m > 0 ? s->s.v[BO_HEALTH] / m : 1.0f; }
// phase of a side's running attack
static int bo_phase(const BotSide *s) {
    if (!bo_attacking(s) || !s->r.ok) return BO_PH_NONE;
    int st = s->r.strike > 0 ? s->r.strike : s->r.startup;
    if (s->frame < st) return BO_PH_STARTUP;
    // The adapter keeps a side 'attacking' only until the END of the move (release).
    // ACTIVE = the hit window (6 F after the strike frame, shorter if the move ends sooner); RECOVERY = the rest of the move: the side cannot act, the opponent can.
    int act = st + 6; if (s->r.release > st && s->r.release < act) act = s->r.release;
    return s->frame < act ? BO_PH_ACTIVE : BO_PH_RECOVERY;
}
// frames until the first strike of the opponent's attack (<= 0: active now or over; 99 unknown)
static float bo_frames_to_hit(const BotSide *s) {
    if (!bo_attacking(s) || !s->r.ok) return 99.0f;
    int st = s->r.strike > 0 ? s->r.strike : s->r.startup; return (float)st - s->frame;
}
// Perception: a pawn cannot know WHICH move the opponent started on its first frames. For the first `reveal_f` frames of an attack
// the opponent is seen as 'an attack started' (attacking, sequence, frame) but its row identity and data are hidden, so no startup / strike / motion is known and no defence can be planned from them.
// Used by the decision core (botcore.h bot_step); reveal_f = 0 switches it off (perfect information).
static void bo_perceive(BotSide *op, int reveal_f) {
    if (reveal_f > 0 && op->valid && op->s.v[BO_ATTACKING] > 0 && op->frame < (float)reveal_f) { op->row[0] = 0; op->r.ok = 0; memset(&op->r, 0, sizeof op->r); }
}
#endif

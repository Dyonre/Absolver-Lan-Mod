// botcore.h - pure decision engine of the scripted bot.
// Utility scorer over a 5-state FSM: NEUTRAL (spacing, starters, stance change), OFFENCE (own string: gold link / feint / stop), DEFENCE (opponent attack in flight:
// style ability / Shockwave / dodge / guard), PUNISH (opponent in recovery), RECOVER (own hit stun / down). Each tick bot_step reads a BotObs and pushes BotCmd's
// (bot_cmd.h); the adapter executes them. No game, no Windows, no allocation (only botai.inc is game specific).
// Techs (ids in BT_IDS) with a per-tech toggle and try / ok counters ("bot: tech <id> try|ok" lines):
//   U1 back-stance guard hole, U2 feint (AI-pawn feint = attack launched with the cancel flag), U3 gold-link timing, U4 stance select (stance change before a starter),
//   U12 Shockwave as an all-direction parry (ruleset bug tech: off unless allowed), U15 dodge timing, X9 perfect-reaction defence (style ability at the hit frame), GD guard, PN punish.
#ifndef BOTCORE_H
#define BOTCORE_H
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "bot_cmd.h"
#include "botobs.h"

enum { BT_U1 = 0, BT_U2, BT_U3, BT_U4, BT_U12, BT_U15, BT_X9, BT_GD, BT_PN, BT_FU, BT_OP, BT_CC, BT_TG, BT_NF, BT_N };   // FU feint follow-up, OP attack a stunned opponent, CC counter after being countered, TG tired guard, NF neutral fallback; all genome switches (never fixed rules)
static const char *const BT_IDS[BT_N] = { "U1", "U2", "U3", "U4", "U12", "U15", "X9", "GD", "PN", "FU", "OP", "CC", "TG", "NF" };
enum { BR_VANILLA = 0 };
static const char *const BR_NAMES[1] = { "vanilla" };
enum { BD_SPARRING = 0, BD_HARD = 1, BD_PEAK = 2 };
static const char *const BD_NAMES[3] = { "sparring", "hard", "peak" };
enum { BP_TECH = 0, BP_PROOF = 1, BP_IDLE = 2, BP_NN = 3 };   // BP_NN: the neural network drives the bot (botai.inc bai_nn_step); the scripted step is skipped
enum { BS_NEUTRAL = 0, BS_OFFENCE, BS_DEFENCE, BS_PUNISH, BS_RECOVER };
static const char *const BS_NAMES[5] = { "neutral", "offence", "defence", "punish", "recover" };
enum { BA_NONE = 0, BA_PARRY, BA_AVOID, BA_ABSORB, BA_DRUNKEN };   // style ability kind (adapter fills it from UDefenseComponent::BPF_GetSpecialAbility)
#define BOT_PROOF_MAX 12
#define BOT_DECK_MAX 6

// learned strategy knobs (BotCfg.kx[KX_*]), defaults in KX_DEF
enum { KX_ABILITY_W, KX_GUARD_W, KX_DEF_NOISE, KX_INTERRUPT_W, KX_IGNORE_W, KX_DEF_RANGE_UU, KX_PUNISH_RATE, KX_PUNISH_ALT_RATE, KX_PUNISH_RANGE_EXTRA, KX_PUNISH_WINDOW_S, KX_DEF_HOLD_S, KX_OP_RATE, KX_OP_ALT_RATE, KX_OP_RANGE_EXTRA, KX_TIRED_PUNISH, KX_TIRED_MAX_S, KX_CHAIN_ALT_RATE, KX_PLAIN_OFFSET_F, KX_PRESS_CHAIN_EXTRA, KX_PRESS_DEF_S, KX_PRESS_HIT_S, KX_PRESSURE_GUARDED, KX_NEUTRAL_FEINT_RATE, KX_NEUTRAL_TICK_S, KX_NF_WAIT_S, KX_NF_VS_GUARD, KX_AGG_SCALE, KX_U1_RATE, KX_STARTER_GAP_S, KX_STANCE_WAIT_S, KX_STANCE_IDLE_RATE, KX_STANCE_AFTER_STRING, KX_STANCE_AFTER_FEINT, KX_FEINT_FU_ALT, KX_DODGE_AWAY_ORIGIN, KX_DODGE_FWD_W, KX_FOLLOWUP_GUARD_RATE, KX_POWER_SPACE_W, KX_UNLOCK_GUARD_W, KX_RELOCK_S, KX_STRAFE_RATE, KX_STEP_OUT_RATE, KX_MOVE_S, KX_PW_BREAK_W, KX_WC_RATE, KX_WC_DELAY_S, KX_DODGE_IN_RATE, KX_DODGE_AFTER_STRING, KX_DRAW_TICK_S, KX_DRAW_GAP_S, KX_DRUNK_VAR_W0, KX_DRUNK_VAR_W1, KX_DRUNK_VAR_W2, KX_FU_DELAY_F0, KX_FU_ALT0, KX_FU_DELAY_F1, KX_FU_ALT1, KX_FU_DELAY_F2, KX_FU_ALT2, KX_FU_DELAY_F3, KX_FU_ALT3, KX_FU_DELAY_F4, KX_FU_ALT4, KX_TIRED_CD_S, KX_N };
static const char *const KX_NAMES[KX_N] = { "ability_w", "guard_w", "def_noise", "interrupt_w", "ignore_w", "def_range_uu", "punish_rate", "punish_alt_rate", "punish_range_extra", "punish_window_s", "def_hold_s", "op_rate", "op_alt_rate", "op_range_extra", "tired_punish", "tired_max_s", "chain_alt_rate", "plain_offset_f", "press_chain_extra", "press_def_s", "press_hit_s", "pressure_guarded", "neutral_feint_rate", "neutral_tick_s", "nf_wait_s", "nf_vs_guard", "agg_scale", "u1_rate", "starter_gap_s", "stance_wait_s", "stance_idle_rate", "stance_after_string", "stance_after_feint", "feint_fu_alt", "dodge_away_origin", "dodge_fwd_w", "followup_guard_rate", "power_space_w", "unlock_guard_w", "relock_s", "strafe_rate", "step_out_rate", "move_s", "pw_break_w", "wc_rate", "wc_delay_s", "dodge_in_rate", "dodge_after_string", "draw_tick_s", "draw_gap_s", "drunk_var_w0", "drunk_var_w1", "drunk_var_w2", "fu_delay_f0", "fu_alt0", "fu_delay_f1", "fu_alt1", "fu_delay_f2", "fu_alt2", "fu_delay_f3", "fu_alt3", "fu_delay_f4", "fu_alt4", "tired_cd_s" };
static const float KX_DEF[KX_N] = { 1.0f, 0.2f, -1.0f, 0.0f, 0.0f, 450.0f, 1.0f, 0.0f, 40.0f, 1.0f, 1.2f, 1.0f, 0.0f, 60.0f, 0.0f, 1.5f, 0.0f, 6.0f, 2.0f, 1.5f, 1.2f, 1.0f, 0.0f, 0.25f, 1.0f, 0.0f, 0.5f, 1.0f, 0.6f, 0.9f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.6f, 0.0f, 0.0f, 0.3f, 0.0000f, -1.0000f, 0.3500f, 0.0000f, 0.0000f, 0.2500f, 1.5000f, 0.0000f, 0.0000f, 0.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, -1.0000f, 2.0f };
//   ability_w            defence score of the style ability
//   guard_w              defence score of the guard
//   def_noise            defence score noise (-1 = by difficulty: 0.1 peak / 0.6)
//   interrupt_w          defence score of attacking INTO the startup with the stance starter when it lands first
//   ignore_w             defence score of doing nothing
//   def_range_uu         answer opponent attacks within this distance
//   punish_rate          chance to punish a recovery
//   punish_alt_rate      punish with the alternate input
//   punish_range_extra   punish range beyond range_uu
//   punish_window_s      punish window after a defended hit
//   def_hold_s           DEFENCE posture length
//   op_rate              chance to attack a stunned opponent
//   op_alt_rate          opening with the alternate input
//   op_range_extra       opening range beyond range_uu
//   tired_punish         a tired bot may still punish (chance)
//   tired_max_s          longest tired phase
//   chain_alt_rate       string follow-ups with the alternate input
//   plain_offset_f       plain link press after the gold window (frames)
//   press_chain_extra    extra links inside a pressure window
//   press_def_s          pressure window after a defence
//   press_hit_s          pressure window after an own hit
//   pressure_guarded     pressure after a GUARDED hit: x pressure
//   neutral_feint_rate   neutral starters thrown as feints
//   neutral_tick_s       neutral decision tick
//   nf_wait_s            stall breaker: after this long in range the attack roll is certain
//   nf_vs_guard          stall breaker also vs a guarding opponent (chance)
//   agg_scale            attack roll = aggression x this per tick
//   u1_rate              take the back-stance guard hole when open
//   starter_gap_s        gap after a starter
//   stance_wait_s        gap after a neutral stance change
//   stance_idle_rate     neutral: change stance on its own
//   stance_after_string  string over: change stance (also in the release)
//   stance_after_feint   after a feint: change stance instead of the follow-up starter
//   feint_fu_alt         after a feint: alternate starter
//   dodge_away_origin    side dodge away from the strike origin (chance) [side mapping guessed]
//   dodge_fwd_w          dodge direction weight: forward
//   followup_guard_rate  counter after a guard (-1 = followup_rate AND pressure)
//   power_space_w        use a space power (kind 8, Shockwave) when the opponent is close
//   unlock_guard_w       unlock while guarding (no back-stance hole)
//   relock_s             relock after an unlocked guard
//   strafe_rate          neutral: strafe
//   step_out_rate        neutral: step back when closer than spacing_uu
//   move_s               length of a neutral step
//   pw_break_w           defence score of firing a power AT ONCE to break the string (powers interrupt like attacks)
//   wc_rate              whiff cancel: dodge out of a whiffed avoid / absorb (-1 = the whiff_cancel switch)
//   wc_delay_s           whiff cancel delay after the ability
//   dodge_in_rate        neutral: dodge forward to close in (out of range)
//   dodge_after_string   string over: dodge back
//   draw_tick_s          weapon decision tick
//   draw_gap_s           gap after a draw / sheathe
//   drunk_var_w0..2      Drunken ability variant weights (all 0 = fixed rule: Stagger side avoid, Faejin by the strike origin)
//   fu_delay_f0..4 / fu_alt0..4   follow-up delay (frames) / alternate rate after defence kind 0 guard 1 parry 2 avoid 3 absorb 4 dodge (-1 = table + followup_delay_f / followup_alt_rate)
typedef struct {
    int policy, ruleset, difficulty;
    BotSkill skill;
    float aggression;        // 0..1: chance per second (x 2) to start a string in neutral when in range
    float feint_rate;        // share of follow-ups thrown as feints (U2)
    float stance_rate;       // chance a starter is preceded by a stance change (U4)
    float alt_rate;          // starter choice: chance a neutral starter is the ALTERNATE attack (input 1) instead of the stance's main string (0 = starters never use the alternates)
    float qpref[4];          // starter choice: weights of the four stances (FL FR BR BL) when a stance change precedes a starter (all 1 = uniform pick)
    int reveal_f;            // perception: frames before the opponent's attack row is identified (0 = perfect information; hard 4, peak 2, sparring 6)
    int drunken_driven;      // 1 = the adapter can launch the Drunken style ability (Stagger / Faejin ability rows): X9 may press it (the game adapter sets it when ability_kind is BA_DRUNKEN)
    float punish_min_f;      // PUNISH only when the opponent's recovery still lasts this many frames (the fastest own starter must land before it can act)
    float gold_rate;         // share of follow-ups pressed for a gold link (U3)
    float gold_offset_f;     // press offset vs the window opening (frames, negative = early; the buffer accepts -2.4 .. window)
    float ability_lead_f;    // style ability pressed this many frames before the strike frame
    float dodge_lead_f;      // dodge this many frames before the strike frame
    float approach_uu, range_uu;   // walk in beyond approach_uu; start strings within range_uu
    int chain_max;           // follow-ups per string (deck length)
    // aggression set: press the advantage, keep the initiative
    float pressure;          // 0..1: chance to keep attacking after an own hit / a guarded hit / a defence (instead of resetting to neutral)
    float spacing_uu;        // preferred distance: step in beyond it (also while the opponent recovers)
    float defend_rate;       // 0..1: chance to answer an opponent attack at all
    float followup_rate;     // 0..1: chance to throw the planned continuation after an own style move / guard (ability follow-up table)
    // high-level play habits (values are estimates): delay pressing, stamina stop, delayed feints, post-absorb options
    float delay_rate;        // share of follow-up presses thrown OFF-timed (after the gold window, delay_min_f..delay_max_f frames) instead of on the gold frame
    float delay_min_f, delay_max_f;
    float stamina_stop;      // own stamina (guard gauge) fraction below which the bot stops pressing and blocks; resumes above stamina_resume
    float stamina_resume;
    float post_absorb_switch; // after an absorb: chance of a stance switch (ASS-like) instead of the counter
    // dodge budget / commitment: a dodge is only an option when the read fits the dodge's timing:
    // i-frames from ~3 F, the burst at ~7 F, plus up to dodge_input_f of stick wait -> it must be pressed dodge_lead_f before the strike with dodge_lead_f in
    // [dodge_iframe_f + dodge_input_f, dodge_burst_f + dodge_input_f]; plus a cooldown, a budget per 5 s and a stamina floor (a dodge costs stamina).
    float dodge_iframe_f, dodge_burst_f, dodge_input_f, dodge_cooldown_s, dodge_stamina_min; int dodge_budget_5s;
    int whiff_cancel;        // Windfall / Kahlt: cancel a whiffed style ability with a dodge (tech WC, adapter side)
    int tech[BT_N];          // 1 = use
    int ability_kind;        // BA_* (filled by the adapter)
    float power_w;           // weight of the equipped DEFENSIVE power (Shockwave parry, Earthquake / Shield absorb, ...) among the defence options (ability 1.0, power_w, dodge 0.35, guard 0.2); genome knob
    int power_slot;          // power to use as a parry (U12): slot 0 / 1, -1 none
    // weapon economy, learned, not scripted: draw_w = chance per neutral decision (0.25 s) to draw when the weapon is affordable and the moment is
    // calm; draw_reserve = shards to keep AFTER paying for the draw (for a power); sheathe_dur = put it away below this durability at a moment the opponent cannot hit; wpn_cost = the weapon's draw cost
    // (preset / trainer; 0 = no weapon decisions: the game adapter's fixed policy runs instead)
    float draw_w, draw_reserve, sheathe_dur, wpn_cost, draw_dist;   // draw_dist: a draw only when the opponent is at least this far (or stunned)
    // learned knobs (defaults in bot_cfg_init)
    float dodge_w_thrust, dodge_w_other;   // dodge score vs a thrust / any other motion
    float dodge_lead_frac;                 // dodge pressed this share of the opponent's startup before the strike (<= 0: dodge_lead_f)
    float dodge_back_w, dodge_side_w;      // direction weights: back (keeps space) / side
    float guard_hold_s, tired_guard_s;     // guard kept this long after the expected hit / while tired (tap blocking: the guard gauge regenerates x0.2 while guarding)
    float backoff_rate;                    // after an own string ends / a defence without a follow-up: chance to step back instead of staying in
    float backoff_stamina;                 // below this guard-gauge share the bot steps back rather than pressing (0 = never)
    float backoff_s;                       // length of a step back (s)
    float tired_retreat;                   // tired: chance to step back instead of raising the guard
    float followup_alt_rate, followup_delay_f;   // after a defence: share of follow-ups thrown with the alternate input / frames added to the table delay (defaults: table only)
    // powers: kind bitmask per slot (set by the trainer / preset from the equipped item), shard cost, cooldown
    int power_kind[2]; float power_cost[2], power_cd[2];   // kind bitmask: 1 defensive window, 2 acts on the opponent, 4 self buff, 8 makes space; power_cd: cooldown (s); the adapter charges cost + cooldown (the game never charges an NPC)
    float kx[KX_N];          // learned knobs (KX_*)
    float starter_f[4];      // startup frames of each stance's first deck row (the adapter fills it; 0 = unknown): the interrupt option
    float power_lead_f;      // defensive power pressed this many frames before the expected hit
    float power_off_w;       // chance per neutral decision to use an opponent power (kind 2) when the opponent is stunned, recovering or out of range
    float power_buff_w;      // chance per neutral decision to use a self buff (kind 4) out of range
    float power_reserve;     // shards kept after paying for an offensive / buff power
    // deck (bot_decks shape): rows per start quadrant (0 FL 1 FR 2 BR 3 BL) and the alternate per quadrant; the adapter plays the real deck (combo manager), this
    // copy is for planning: guard-hole rows (U1) and stance choice.
    char deck[4][BOT_DECK_MAX][16]; int deck_n[4]; char alt[4][16];
    char proof[BOT_PROOF_MAX][12]; int proof_n;   // policy proof: ordered action steps
    uint64_t seed;
} BotCfg;
typedef struct {
    double t_next_draw, t_next_power, relock_at; int unlocked;   // relock_at / unlocked: an unlocked guard (kx unlock_guard_w)                           // draw / sheathe gate; offensive / buff power gate
    BotSide last_me;                                            // the last own observation (callbacks read the stamina)
    unsigned punish_seq; int punish_roll; double tired_block;   // the learned punish decision for the opponent's current attack
    double ab_t; int ab_pending, wc_done;                       // own defensive ability pressed (whiff cancel)
    int fsm; double t_state, t_last_start, t_next_neutral, guard_until, punish_until, feint_fu_until, t_next_open, tired_since, t_wait_start; int guard_up, chain, string_on, feint_fu;
    unsigned def_seq, own_seq, gold_seq; int def_kind;       // last opponent attack answered, last own attack seen, own attack a follow-up was planned for
    int tries[BT_N], oks[BT_N];
    double press_until; int follow_pending; int tired;   // pressure window after an own hit / defence; a planned follow-up is queued; tired: stamina stop active
    double dodge_t[4]; int dodge_i;                             // last dodges (budget / cooldown)
    int proof_i; double proof_t; int proof_wait;              // proof policy: step index, time the step started, 1 = waiting for an opponent attack
    BotRng rng;
} BotState;

// ----- configuration
static void bot_cfg_difficulty(BotCfg *c, int d) {
    c->difficulty = d < 0 ? 0 : d > 2 ? 2 : d;
    static const BotSkill sk[3] = { { 280, 60, 180, 2.0f, 0.35f, 4 }, { 180, 40, 120, 1.0f, 0.15f, 4 }, { 100, 25, 83, 0.5f, 0.03f, 3 } };
    static const float ag[3] = { 0.35f, 0.55f, 0.75f }, fr[3] = { 0.05f, 0.15f, 0.25f }, sr[3] = { 0.1f, 0.25f, 0.4f }, gr[3] = { 0.4f, 0.75f, 1.0f };
    static const float pr[3] = { 0.3f, 0.6f, 0.9f }, dr[3] = { 0.45f, 0.65f, 0.8f }, fu[3] = { 0.4f, 0.75f, 0.95f }, dl[3] = { 0.0f, 0.15f, 0.3f }, ss[3] = { 0.2f, 0.3f, 0.35f };
    static const int rv[3] = { 6, 4, 2 }; c->reveal_f = rv[c->difficulty]; c->alt_rate = 0.0f; for (int qi = 0; qi < 4; qi++) c->qpref[qi] = 1.0f; c->delay_rate = dl[c->difficulty]; c->stamina_stop = ss[c->difficulty]; c->stamina_resume = 0.45f; c->delay_min_f = 3; c->delay_max_f = 12; c->post_absorb_switch = c->difficulty == BD_SPARRING ? 0.0f : 0.3f;
    c->dodge_iframe_f = 3; c->dodge_burst_f = 7; c->dodge_input_f = c->difficulty == BD_PEAK ? 0.0f : 3.0f; c->dodge_cooldown_s = 1.0f; c->dodge_stamina_min = 0.35f; c->dodge_budget_5s = 2; c->whiff_cancel = c->difficulty != BD_SPARRING;
    c->skill = sk[c->difficulty]; c->aggression = ag[c->difficulty]; c->feint_rate = fr[c->difficulty]; c->stance_rate = sr[c->difficulty]; c->gold_rate = gr[c->difficulty];
    c->pressure = pr[c->difficulty]; c->defend_rate = dr[c->difficulty]; c->followup_rate = fu[c->difficulty];
    for (int i = 0; i < BT_N; i++) c->tech[i] = 1;
    if (c->difficulty == BD_SPARRING) { c->tech[BT_U1] = 0; c->tech[BT_U4] = 0; c->tech[BT_X9] = 1; }
    c->tech[BT_U12] = 0;                                                          // bug tech: only with an explicit preset entry
}
static void bot_cfg_init(BotCfg *c) {
    memset(c, 0, sizeof *c); c->policy = BP_TECH; c->ruleset = BR_VANILLA; bot_cfg_difficulty(c, BD_HARD);
    c->gold_offset_f = 0; c->ability_lead_f = 4; c->dodge_lead_f = 5; c->approach_uu = 260; c->range_uu = 230; c->spacing_uu = 200; c->punish_min_f = 14; c->chain_max = 3; c->power_slot = -1; c->power_w = 0.8f; c->draw_dist = 300;
    c->dodge_w_thrust = 0.35f; c->dodge_w_other = 0.12f; c->dodge_lead_frac = 0.5f; c->dodge_back_w = 0.0f; c->dodge_side_w = 1.0f; c->guard_hold_s = 0.5f; c->tired_guard_s = 1.0f;
    c->backoff_rate = 0.0f; c->backoff_stamina = 0.0f; c->backoff_s = 0.4f; c->tired_retreat = 0.0f; c->followup_alt_rate = -1.0f; c->followup_delay_f = 0.0f; c->power_lead_f = 3.0f; for (int i = 0; i < KX_N; i++) c->kx[i] = KX_DEF[i]; c->seed = 12345;
}
static int bot_tech_id(const char *s) { for (int i = 0; i < BT_N; i++) if (s && !strcmp(s, BT_IDS[i])) return i; return -1; }
static int bot_enum(const char *s, const char *const *names, int n) { for (int i = 0; i < n; i++) if (s && !strcmp(s, names[i])) return i; return -1; }
// one "brain" key (preset JSON). Returns 1 = taken, 0 = unknown key / bad value (the adapter logs it). "difficulty" applies its defaults, so it must come first (bai_load_cfg does that).
static int bot_cfg_set(BotCfg *c, const char *k, const char *v) {
    if (!k || !v) return 0; float f = (float)atof(v); int on = !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "use");
    if (!strcmp(k, "policy")) { static const char *const p[4] = { "tech", "proof", "idle", "nn" }; int i = bot_enum(v, p, 4); if (i < 0) return 0; c->policy = i; return 1; }
    if (!strcmp(k, "ruleset")) { int i = bot_enum(v, BR_NAMES, 1); if (i < 0) return 0; c->ruleset = i; return 1; }
    if (!strcmp(k, "difficulty")) { int i = bot_enum(v, BD_NAMES, 3); if (i < 0) return 0; bot_cfg_difficulty(c, i); return 1; }
    if (!strcmp(k, "react_mean_ms")) { c->skill.react_mean_ms = f; return 1; }
    if (!strcmp(k, "react_sd_ms")) { c->skill.react_sd_ms = f; return 1; }
    if (!strcmp(k, "react_floor_ms")) { c->skill.react_floor_ms = f; return 1; }
    if (!strcmp(k, "exec_error_f")) { c->skill.exec_err_f = f; return 1; }
    if (!strcmp(k, "gold_miss")) { c->skill.gold_miss = f; return 1; }
    if (!strcmp(k, "aggression")) { c->aggression = f; return 1; }
    if (!strcmp(k, "feint_rate")) { c->feint_rate = f; return 1; }
    if (!strcmp(k, "stance_rate")) { c->stance_rate = f; return 1; }
    if (!strcmp(k, "punish_min_f")) { c->punish_min_f = f; return 1; }
    if (!strcmp(k, "reveal_f")) { c->reveal_f = (int)f; return 1; }
    if (!strcmp(k, "alt_rate")) { c->alt_rate = f < 0 ? 0 : f > 1 ? 1 : f; return 1; }
    if (!strncmp(k, "qpref", 5) && k[5] >= '0' && k[5] <= '3' && !k[6]) { c->qpref[k[5] - '0'] = f < 0 ? 0 : f; return 1; }
    if (!strcmp(k, "drunken_driven")) { c->drunken_driven = f != 0; return 1; }
    if (!strcmp(k, "gold_rate")) { c->gold_rate = f; return 1; }
    if (!strcmp(k, "gold_offset_f")) { c->gold_offset_f = f; return 1; }
    if (!strcmp(k, "ability_lead_f")) { c->ability_lead_f = f; return 1; }
    if (!strcmp(k, "dodge_lead_f")) { c->dodge_lead_f = f; return 1; }
    if (!strcmp(k, "approach_uu")) { c->approach_uu = f; return 1; }
    if (!strcmp(k, "range_uu")) { c->range_uu = f; return 1; }
    if (!strcmp(k, "chain_max")) { c->chain_max = atoi(v); return 1; }
    if (!strcmp(k, "pressure")) { c->pressure = f; return 1; }
    if (!strcmp(k, "spacing_uu")) { c->spacing_uu = f; return 1; }
    if (!strcmp(k, "defend_rate")) { c->defend_rate = f; return 1; }
    if (!strcmp(k, "followup_rate")) { c->followup_rate = f; return 1; }
    if (!strcmp(k, "delay_rate")) { c->delay_rate = f; return 1; }
    if (!strcmp(k, "delay_min_f")) { c->delay_min_f = f; return 1; }
    if (!strcmp(k, "delay_max_f")) { c->delay_max_f = f; return 1; }
    if (!strcmp(k, "stamina_stop")) { c->stamina_stop = f; return 1; }
    if (!strcmp(k, "stamina_resume")) { c->stamina_resume = f; return 1; }
    if (!strcmp(k, "post_absorb_switch")) { c->post_absorb_switch = f; return 1; }
    if (!strcmp(k, "dodge_cooldown_s")) { c->dodge_cooldown_s = f; return 1; }
    if (!strcmp(k, "dodge_stamina_min")) { c->dodge_stamina_min = f; return 1; }
    if (!strcmp(k, "dodge_budget_5s")) { c->dodge_budget_5s = atoi(v); return 1; }
    if (!strcmp(k, "whiff_cancel")) { c->whiff_cancel = on; return 1; }
    if (!strcmp(k, "power_slot")) { c->power_slot = atoi(v); return 1; }
    if (!strcmp(k, "power_w")) { c->power_w = f < 0 ? 0 : f; return 1; }
    if (!strcmp(k, "draw_w")) { c->draw_w = f < 0 ? 0 : f > 1 ? 1 : f; return 1; }
    if (!strcmp(k, "draw_reserve")) { c->draw_reserve = f < 0 ? 0 : f; return 1; }
    if (!strcmp(k, "sheathe_dur")) { c->sheathe_dur = f < 0 ? 0 : f > 1 ? 1 : f; return 1; }
    if (!strcmp(k, "wpn_cost")) { c->wpn_cost = f < 0 ? 0 : f; return 1; }
    if (!strcmp(k, "draw_dist")) { c->draw_dist = f < 0 ? 0 : f; return 1; }
    if (!strncmp(k, "power_kind", 10) && (k[10] == '0' || k[10] == '1') && !k[11]) { c->power_kind[k[10] - '0'] = atoi(v); return 1; }
    if (!strncmp(k, "power_cost", 10) && (k[10] == '0' || k[10] == '1') && !k[11]) { c->power_cost[k[10] - '0'] = f < 0 ? 0 : f; return 1; }
    if (!strncmp(k, "power_cd", 8) && (k[8] == '0' || k[8] == '1') && !k[9]) { c->power_cd[k[8] - '0'] = f < 0 ? 0 : f; return 1; }
    if (!strcmp(k, "power_lead_f")) { c->power_lead_f = f < 0 ? 0 : f; return 1; }
    if (!strcmp(k, "power_off_w")) { c->power_off_w = f < 0 ? 0 : f > 1 ? 1 : f; return 1; }
    if (!strcmp(k, "power_buff_w")) { c->power_buff_w = f < 0 ? 0 : f > 1 ? 1 : f; return 1; }
    if (!strcmp(k, "power_reserve")) { c->power_reserve = f < 0 ? 0 : f; return 1; }
    for (int i = 0; i < KX_N; i++) if (!strcmp(k, KX_NAMES[i])) { c->kx[i] = f; return 1; }
    if (!strncmp(k, "starter_f", 9) && k[9] >= '0' && k[9] <= '3' && !k[10]) { c->starter_f[k[9] - '0'] = f; return 1; }
    { static const char *const fk[] = { "dodge_w_thrust", "dodge_w_other", "dodge_lead_frac", "dodge_back_w", "dodge_side_w", "guard_hold_s", "tired_guard_s", "backoff_rate", "backoff_stamina", "backoff_s", "tired_retreat", "followup_alt_rate", "followup_delay_f" };
      float *fv[] = { &c->dodge_w_thrust, &c->dodge_w_other, &c->dodge_lead_frac, &c->dodge_back_w, &c->dodge_side_w, &c->guard_hold_s, &c->tired_guard_s, &c->backoff_rate, &c->backoff_stamina, &c->backoff_s, &c->tired_retreat, &c->followup_alt_rate, &c->followup_delay_f };
      for (int i = 0; i < (int)(sizeof fk / sizeof fk[0]); i++) if (!strcmp(k, fk[i])) { *fv[i] = (f < 0 && i != 11 && i != 12) ? 0 : f; return 1; } }
    if (!strcmp(k, "seed")) { c->seed = (uint64_t)_strtoui64(v, NULL, 10); return 1; }
    if (!strncmp(k, "tech.", 5)) { int i = bot_tech_id(k + 5); if (i < 0) return 0; c->tech[i] = on; return 1; }
    if (!strcmp(k, "proof")) {
        c->proof_n = 0; const char *p = v;
        while (*p && c->proof_n < BOT_PROOF_MAX) { int n = 0; while (p[n] && p[n] != ',') n++; if (n > 0 && n < 12) { memcpy(c->proof[c->proof_n], p, n); c->proof[c->proof_n][n] = 0; c->proof_n++; } p += n; if (*p == ',') p++; }
        return c->proof_n > 0;
    }
    return 0;
}
// preset sanity: returns 0 + reason when a preset cannot work (U12 needs a defensive power, reaction times, proof list)
static int bot_cfg_check(const BotCfg *c, char *why, int cap) {
    if (c->tech[BT_U12] && c->power_slot < 0 && !(c->power_kind[0] & 1) && !(c->power_kind[1] & 1)) { snprintf(why, cap, "tech U12 needs power_slot or a defensive power (power_kind0/1 = 1)"); return 0; }
    if (c->skill.react_floor_ms < 0 || c->skill.react_mean_ms < 0) { snprintf(why, cap, "negative reaction time"); return 0; }
    if (c->policy == BP_PROOF && !c->proof_n) { snprintf(why, cap, "policy proof needs a proof list"); return 0; }
    return 1;
}
static void bot_state_init(BotState *s, const BotCfg *c) { memset(s, 0, sizeof *s); bc_rng_seed(&s->rng, c->seed); }

// ----- helpers
static void bot_note(BotState *s, int tech, int ok) { if (tech < 0 || tech >= BT_N) return; if (ok) s->oks[tech]++; else s->tries[tech]++; }
static BotCmd bot_cmd(int type, double now, const char *tech) { BotCmd c; memset(&c, 0, sizeof c); c.type = type; c.seen = now; if (tech) snprintf(c.tech, sizeof c.tech, "%s", tech); return c; }
// a reaction aimed at an absolute time: due = max(now + reaction, target) + jitter
static uint32_t bot_push_target(BotQueue *q, BotCmd c, double target, BotState *s, const BotCfg *cfg) {
    double r = c.seen + bc_reaction(&cfg->skill, &s->rng); double at = (target > r ? target : r) + bc_jitter(&cfg->skill, &s->rng); if (at < c.seen) at = c.seen;
    return bc_push_at(q, c, at);
}
// may the bot commit to a dodge for a strike `to_hit_s` seconds away? (timing window, cooldown, budget, stamina). Returns 1 and the lead to use (frames).
static int bot_dodge_ok(const BotCfg *c, const BotState *s, const BotSide *me, double now, double to_hit_s, double react_s, float *lead_f) {
    float lo = c->dodge_iframe_f + c->dodge_input_f, hi = c->dodge_burst_f + c->dodge_input_f, lead = c->dodge_lead_f < lo ? lo : c->dodge_lead_f > hi ? hi : c->dodge_lead_f;
    if (to_hit_s - lead * BC_FRAME < react_s) return 0;                                       // cannot press early enough for the i-frames to cover the strike
    int n = 0; for (int i = 0; i < 4; i++) { if (s->dodge_t[i] > 0 && now - s->dodge_t[i] < c->dodge_cooldown_s) return 0; if (s->dodge_t[i] > 0 && now - s->dodge_t[i] < 5.0) n++; }
    if (n >= c->dodge_budget_5s) return 0;
    float mg = bo_v(me, BO_MAXGUARD), g = bo_v(me, BO_GUARD); if (mg > 0 && g >= 0 && g / mg < c->dodge_stamina_min) return 0;
    *lead_f = lead; return 1;
}
static void bot_dodge_used(BotState *s, double now) { s->dodge_t[s->dodge_i++ & 3] = now; }
static int bot_side_dir(const BotRow *r) { (void)r; return 1; }
static int bot_dodge_dir(const BotCfg *c, BotState *s, const BotRow *r) { float fw = c->kx[KX_DODGE_FWD_W], tw = c->dodge_back_w + c->dodge_side_w + fw; if (tw <= 0) return 1; double x = bc_rand(&s->rng) * tw;
    if (x < c->dodge_back_w) return 0; if (x < c->dodge_back_w + fw) return 3;
    if (r && r->ok && bc_rand(&s->rng) < c->kx[KX_DODGE_AWAY_ORIGIN]) return r->origin == 0 ? 2 : 1;   // away from the strike's origin [the side-number mapping is unverified]
    return bc_rand(&s->rng) < 0.5 ? 1 : 2; }
// a stance by the learned preferences (qpref), not the current one; -1 when none
static int bot_pick_quad(const BotCfg *c, BotState *s, int cur) { float tw = 0; for (int i = 0; i < 4; i++) if (i != cur) tw += c->qpref[i]; if (tw <= 0) return -1; double r = bc_rand(&s->rng) * tw, acc = 0;
    for (int i = 0; i < 4; i++) { if (i == cur) continue; acc += c->qpref[i]; if (r < acc) return i; } return -1; }
// relock before attacking when an unlocked guard is still active
static void bot_relock(BotQueue *q, BotState *s, double now) { if (s->unlocked) { bc_push_at(q, bot_cmd(BC_LOCK, now, "LK"), now); s->unlocked = 0; } }
// U1: an opener in quadrant q whose origin hits the side the defender's back stance leaves open: BL -> left origin, BR -> right origin
static int bot_guard_hole_quad(int op_stance) { return op_stance == 3 ? 0 : op_stance == 2 ? 1 : -1; }

// ----- ability follow-up module (a style move is never followed by a pause). What to throw right after an own SUCCESSFUL defence,
// planned as part of the move (no reaction delay: the bot knows it pressed the ability). Values are starting estimates, not tuned to frame advantage per style.
// delay_f = frames to wait after the defence event before the press (the input buffer takes it early anyway), input 0 main / 1 alternate.
typedef struct { int input; float delay_f; int gold; const char *note; } BotFollow;
enum { BF_GUARD = 0, BF_PARRY, BF_AVOID, BF_ABSORB, BF_DODGE, BF_N };
static BotFollow bot_followup(int def_kind, int ruleset) {
    // def_kind: what succeeded (BF_*). Rules: parry -> the parried attacker is stunned (ParryDB parried profile): fastest main-chain starter at once;
    // avoid (Windfall / Faejin slip) -> the attacker is still in its move: counter at once; absorb (Kahlt) -> the absorbed hit's attacker keeps attacking: counter after the
    // absorb anim (alternate = heavier option); guard -> only with pressure (guard stun favours the attacker): main chain after a short delay; dodge -> main chain.
    static const BotFollow v[BF_N] = { { 0, 4, 0, "guard: counter after block stun" }, { 0, 0, 0, "parry: punish the parried stun" }, { 0, 0, 0, "avoid: counter inside the attack" },
                                       { 1, 3, 0, "absorb: alternate counter" }, { 0, 2, 0, "dodge: punish the whiff" } };
    BotFollow f = def_kind >= 0 && def_kind < BF_N ? v[def_kind] : v[0];
    return f;
}
// the adapter reports an own successful defence (BF_*); returns 1 when a follow-up was queued
static int bot_on_defended(const BotCfg *c, BotState *s, int def_kind, double now, BotQueue *q) {
    if (c->policy != BP_TECH) return 0;
    s->ab_pending = 0;
    s->press_until = now + c->kx[KX_PRESS_DEF_S];
    { float mg = bo_v(&s->last_me, BO_MAXGUARD), g = bo_v(&s->last_me, BO_GUARD); int low = c->backoff_stamina > 0 && mg > 0 && g >= 0 && g / mg < c->backoff_stamina;
      int skip = def_kind == BF_GUARD && c->kx[KX_FOLLOWUP_GUARD_RATE] >= 0 ? bc_rand(&s->rng) >= c->kx[KX_FOLLOWUP_GUARD_RATE] : (bc_rand(&s->rng) >= c->followup_rate || (def_kind == BF_GUARD && bc_rand(&s->rng) >= c->pressure));
      if (low || skip) {
          if (low || bc_rand(&s->rng) < c->backoff_rate) { BotCmd m = bot_cmd(BC_MOVE, now, "BO"); m.dir = 0; m.secs = c->backoff_s; bc_push_at(q, m, now); s->press_until = 0; }
          return 0; } }
    if (def_kind == BF_ABSORB && bc_rand(&s->rng) < c->post_absorb_switch) {            // switch stance instead of waiting (ASS-like), then the string
        BotCmd k = bot_cmd(BC_STANCE, now, "U18"); k.quad = bot_pick_quad(c, s, bo_stance(&s->last_me)); if (k.quad < 0) k.quad = (int)(bc_rand(&s->rng) * 4) % 4; bc_push_at(q, k, now); s->string_on = 1; return 1; }
    BotFollow f = bot_followup(def_kind, c->ruleset); BotCmd a = bot_cmd(BC_ATTACK, now, "PN"); a.input = c->followup_alt_rate >= 0 ? bc_rand(&s->rng) < c->followup_alt_rate : f.input;
    f.delay_f += c->followup_delay_f; if (f.delay_f < 0) f.delay_f = 0;
    if (def_kind >= 0 && def_kind < 5) { if (c->kx[KX_FU_DELAY_F0 + 2 * def_kind] >= 0) f.delay_f = c->kx[KX_FU_DELAY_F0 + 2 * def_kind]; if (c->kx[KX_FU_ALT0 + 2 * def_kind] >= 0) a.input = bc_rand(&s->rng) < c->kx[KX_FU_ALT0 + 2 * def_kind]; }   // per defence kind
    bc_drop(q, BC_UNGUARD); bc_push_at(q, bot_cmd(BC_UNGUARD, now, "PN"), now); s->guard_up = 0; bot_relock(q, s, now);
    bc_push_at(q, a, now + f.delay_f * BC_FRAME); s->chain = 0; s->string_on = 1; s->follow_pending = 1; s->fsm = BS_PUNISH; s->t_state = now; bot_note(s, BT_PN, 0);
    return 1;
}
// the adapter reports an own hit that landed (or was guarded): keep the initiative
static void bot_on_landed(const BotCfg *c, BotState *s, int guarded, double now) { if (bc_rand(&s->rng) < c->pressure * (guarded ? c->kx[KX_PRESSURE_GUARDED] : 1.0f)) { s->press_until = now + c->kx[KX_PRESS_HIT_S]; s->string_on = 1; } }

// ----- the step. now = seconds; dt = seconds since the last step. Pushes into q; returns the FSM state.
static int bot_step(const BotCfg *c, BotState *s, const BotObs *o, double now, double dt, BotQueue *q) {
    BotObs perceived = *o; if (c->reveal_f > 0) bo_perceive(&perceived.op, c->reveal_f); o = &perceived;   // perception: the opponent's move is not known on its first reveal_f frames
    const BotSide *me = &o->me, *op = &o->op; char b[8]; s->last_me = *me;
    if (c->policy == BP_IDLE || !me->valid) return s->fsm;
    int prev = s->fsm;
    if (bo_v(me, BO_DOWN) > 0 || me->stunned) s->fsm = BS_RECOVER;
    // own attack started: OFFENCE bookkeeping
    if (me->seq != s->own_seq) { s->own_seq = me->seq; s->t_last_start = me->atk_t; s->chain++; }
    // ================= proof policy: one action after the other (scripted checks of the action interface)
    if (c->policy == BP_PROOF) {
        if (s->proof_i >= c->proof_n) return s->fsm;
        const char *st = c->proof[s->proof_i]; int defensive = !strcmp(st, "guard") || !strcmp(st, "ability") || !strcmp(st, "dodge") || !strcmp(st, "dodgeside") || !strcmp(st, "dodgeback") || !strcmp(st, "shockwave");
        if (s->proof_t == 0) s->proof_t = now;
        if (defensive) {
            if (bo_attacking(op) && op->seq != s->def_seq && bo_phase(op) == BO_PH_STARTUP) {
                s->def_seq = op->seq; float fh = bo_frames_to_hit(op); double hit = now + fh * BC_FRAME;
                if (!strcmp(st, "guard")) { bc_push_at(q, bot_cmd(BC_GUARD, now, "GD"), now); BotCmd u = bot_cmd(BC_UNGUARD, now, "GD"); bc_push_at(q, u, hit + 0.8); }
                else if (!strcmp(st, "ability")) { BotCmd a = bot_cmd(BC_ABILITY, now, "X9"); bc_push_at(q, a, hit - c->ability_lead_f * BC_FRAME > now ? hit - c->ability_lead_f * BC_FRAME : now); }
                else if (!strcmp(st, "shockwave")) { BotCmd a = bot_cmd(BC_POWER, now, "U12"); a.slot = c->power_slot < 0 ? 0 : c->power_slot; bc_push_at(q, a, hit - 3 * BC_FRAME > now ? hit - 3 * BC_FRAME : now); }
                else { BotCmd d = bot_cmd(BC_DODGE, now, "U15"); d.dir = !strcmp(st, "dodgeside") ? 1 : !strcmp(st, "dodgeback") ? 0 : bot_side_dir(&op->r); bc_push_at(q, d, hit - c->dodge_lead_f * BC_FRAME > now ? hit - c->dodge_lead_f * BC_FRAME : now); }
                s->proof_i++; s->proof_t = now + 2.5;
            }
            return s->fsm;
        }
        if (now < s->proof_t) return s->fsm;
        if (!strcmp(st, "stance")) { BotCmd k = bot_cmd(BC_STANCE, now, "U4"); int cur = bo_stance(me); k.quad = cur < 0 ? 1 : (cur + 2) % 4; bc_push_at(q, k, now); }
        else if (!strcmp(st, "feint")) { BotCmd a = bot_cmd(BC_ATTACK, now, "U2"); a.feint = 1; bc_push_at(q, a, now); }
        else if (!strcmp(st, "gold")) { BotCmd a = bot_cmd(BC_ATTACK, now, "PN"); bc_push_at(q, a, now); BotCmd g = bot_cmd(BC_ATTACK, now, "U3"); g.gold = 1; g.offset_f = c->gold_offset_f; bc_push_at(q, g, now + 0.05); }
        else if (!strcmp(st, "plain")) { BotCmd a = bot_cmd(BC_ATTACK, now, "PN"); bc_push_at(q, a, now); BotCmd g = bot_cmd(BC_ATTACK, now, "U3"); g.gold = 1; g.offset_f = 14; bc_push_at(q, g, now + 0.05); }
        else if (!strcmp(st, "power")) { BotCmd p = bot_cmd(BC_POWER, now, "PW"); p.slot = c->power_slot < 0 ? 0 : c->power_slot; bc_push_at(q, p, now); }
        else if (!strcmp(st, "unlock")) bc_push_at(q, bot_cmd(BC_UNLOCK, now, "LK"), now);
        else if (!strcmp(st, "lock")) bc_push_at(q, bot_cmd(BC_LOCK, now, "LK"), now);
        else if (!strcmp(st, "attack")) bc_push_at(q, bot_cmd(BC_ATTACK, now, "PN"), now);
        else if (!strcmp(st, "movein") || !strcmp(st, "moveout") || !strcmp(st, "strafe")) { BotCmd m = bot_cmd(BC_MOVE, now, "MP"); m.dir = !strcmp(st, "movein") ? 3 : !strcmp(st, "moveout") ? 0 : 1; m.secs = 1.0f; bc_push_at(q, m, now); }   // movement proofs
        else if (!strcmp(st, "dodgefwd")) { BotCmd d = bot_cmd(BC_DODGE, now, "U15"); d.dir = 3; bc_push_at(q, d, now); }
        s->proof_i++; s->proof_t = now + 3.0; return s->fsm;
    }
    // ================= tech policy
    // RECOVER (stunned / down / low stamina): nothing to decide except one thing: a style ability that is still
    // available (the counter-to-the-counter window after OrderParried / OrderAvoided) may answer the opponent's next attack, so the DEFENCE block below must still run in that case.
    if (s->fsm == BS_RECOVER) { if (bo_v(me, BO_DOWN) <= 0 && !me->stunned && !bo_attacking(me)) s->fsm = BS_NEUTRAL; else if (!(c->tech[BT_CC] && me->ability_ok && bo_v(me, BO_DOWN) <= 0 && bo_attacking(op) && op->seq != s->def_seq)) return s->fsm; }
    // DEFENCE: a new opponent attack in its startup
    if (bo_attacking(op) && op->seq != s->def_seq && bo_phase(op) == BO_PH_STARTUP && o->dist < c->kx[KX_DEF_RANGE_UU]) {
        float fh = bo_frames_to_hit(op); double hit = now + fh * BC_FRAME, react = c->skill.react_mean_ms / 1000.0;
        // busy = own attack running and not in its interruptible window (a recovering side cannot guard / dodge). def_seq is only consumed once the bot is free to decide,
        // so an attack met while busy can still be defended when the own attack ends inside the opponent's startup (a skipped read still consumes it).
        int own_busy = bo_attacking(me) && !me->intr_open;
        if (!own_busy) { s->def_seq = op->seq; if (bc_rand(&s->rng) >= c->defend_rate) own_busy = 1; }   // not every attack is read (difficulty knob defend_rate)
        if (!own_busy) {
            // scores: ability (X9) > Shockwave (U12) > dodge (U15) > guard, each only if the reaction can make it before the hit minus its lead
            // options: 0 ability 1 power 2 dodge 3 guard 4 interrupt (attack into the startup) 5 ignore 6 power at once (string breaker); scores are learned (kx), an option the reaction cannot make in time is dropped
            float sc[7] = { -1, -1, -1, c->kx[KX_GUARD_W], -1, c->kx[KX_IGNORE_W] > 0 ? c->kx[KX_IGNORE_W] : -1, -1 };
            int bslot = -1; if (c->kx[KX_PW_BREAK_W] > 0) for (int i = 0; i < 2; i++) if (c->power_kind[i] && bo_v(me, BO_SHARDS) >= c->power_cost[i]) { bslot = i; sc[6] = c->kx[KX_PW_BREAK_W]; break; }   // a power breaks the string like an interrupt
            double lead[4] = { c->ability_lead_f * BC_FRAME, c->power_lead_f * BC_FRAME, c->dodge_lead_f * BC_FRAME, 0 };
            int dslot = c->power_slot; if (dslot < 0) for (int i = 0; i < 2; i++) if ((c->power_kind[i] & 1) && bo_v(me, BO_SHARDS) >= c->power_cost[i]) { dslot = i; break; }
            if (c->tech[BT_X9] && c->ability_kind != BA_NONE && (c->ability_kind != BA_DRUNKEN || c->drunken_driven) && me->ability_ok) sc[0] = c->kx[KX_ABILITY_W];
            { int cq = bo_stance(me); float sf = cq >= 0 ? c->starter_f[cq] : 0; if (c->kx[KX_INTERRUPT_W] > 0 && sf > 0 && !bo_attacking(me) && now + react + sf * BC_FRAME < hit - BC_FRAME) sc[4] = c->kx[KX_INTERRUPT_W]; }
            if (c->tech[BT_U12] && dslot >= 0) sc[1] = c->power_w;   // any equipped defensive power, not only Shockwave
            // a manual dodge is a LAST RESORT pressed about half way into the attack's startup -> lead = startup / 2 (at least the i-frame minimum)
            float dl = op->r.ok && op->r.startup > 0 && c->dodge_lead_frac > 0 ? (float)op->r.startup * c->dodge_lead_frac : (float)c->dodge_lead_f; BotCfg c2 = *c; c2.dodge_lead_f = dl; c2.dodge_burst_f = dl > c->dodge_burst_f ? dl : c->dodge_burst_f;
            if (c->tech[BT_U15] && bot_dodge_ok(&c2, s, me, now, hit - now, react, &dl)) { sc[2] = op->r.motion == 0 ? c->dodge_w_thrust : c->dodge_w_other; if (sc[2] <= 0) sc[2] = -1; lead[2] = dl * BC_FRAME; }   /* separate dodge scores vs thrusts and vs other motions */   // dodge budget / commitment
            for (int i = 0; i < 3; i++) if (sc[i] > 0 && hit - lead[i] < now + react) sc[i] = -1;          // cannot make it in time: not an option
            { float nz = c->kx[KX_DEF_NOISE] >= 0 ? c->kx[KX_DEF_NOISE] : (c->difficulty == BD_PEAK ? 0.1f : 0.6f); for (int i = 0; i < 7; i++) if (sc[i] > 0) sc[i] += (float)bc_rand(&s->rng) * nz; }
            int best = 3; for (int i = 0; i < 7; i++) if (sc[i] > sc[best]) best = i;
            if (best == 3 && !c->tech[BT_GD] && sc[5] > 0) best = 5;
            if (best == 0) { BotCmd ab = bot_cmd(BC_ABILITY, now, "X9"); float vw = c->kx[KX_DRUNK_VAR_W0] + c->kx[KX_DRUNK_VAR_W1] + c->kx[KX_DRUNK_VAR_W2];
                if (c->ability_kind == BA_DRUNKEN && vw > 0) { double r = bc_rand(&s->rng) * vw; ab.dir = r < c->kx[KX_DRUNK_VAR_W0] ? 1 : r < c->kx[KX_DRUNK_VAR_W0] + c->kx[KX_DRUNK_VAR_W1] ? 2 : 3; }   // 1..3 = variant, 0 = fixed rule
                bot_push_target(q, ab, hit - lead[0], s, c); bot_note(s, BT_X9, 0); s->ab_pending = 1; s->wc_done = 0; s->ab_t = hit - lead[0] > now ? hit - lead[0] : now; }
            else if (best == 6) { BotCmd p = bot_cmd(BC_POWER, now, "PX"); p.slot = bslot; bot_push_target(q, p, now, s, c); }   // power fired at once: string breaker
            else if (best == 1) { BotCmd p = bot_cmd(BC_POWER, now, "U12"); p.slot = dslot; bot_push_target(q, p, hit - lead[1], s, c); bot_note(s, BT_U12, 0); }
            else if (best == 4) { bot_relock(q, s, now); bot_push_target(q, bot_cmd(BC_ATTACK, now, "IN"), now, s, c); s->chain = 0; s->string_on = 1; }   // interrupt: our starter lands first
            else if (best == 5) { }                                                                                                         // ignore: take it / trust the spacing
            else if (best == 2) { BotCmd d = bot_cmd(BC_DODGE, now, "U15"); d.dir = bot_dodge_dir(c, s, &op->r); bot_push_target(q, d, hit - lead[2], s, c); bot_note(s, BT_U15, 0); bot_dodge_used(s, now); }
            else if (c->tech[BT_GD]) { if (me->locked_on && bc_rand(&s->rng) < c->kx[KX_UNLOCK_GUARD_W]) { bc_push_at(q, bot_cmd(BC_UNLOCK, now, "UG"), now); s->unlocked = 1; }   // unlocked guard: no back-stance hole
                bot_push_target(q, bot_cmd(BC_GUARD, now, "GD"), now, s, c); s->guard_up = 1; s->guard_until = hit + c->guard_hold_s; s->relock_at = s->guard_until + c->kx[KX_RELOCK_S]; bot_note(s, BT_GD, 0); }
            s->def_kind = best; s->fsm = BS_DEFENCE; s->t_state = now; s->punish_until = hit + c->kx[KX_PUNISH_WINDOW_S];
        }
    }
    if (s->guard_up && now > s->guard_until) {   /* drop the guard at guard_until even while the opponent attacks: sustained pressure cannot hold it up forever */ bc_push_at(q, bot_cmd(BC_UNGUARD, now, "GD"), now); s->guard_up = 0; }
    if (s->unlocked && now > s->relock_at && !s->guard_up) bot_relock(q, s, now);
    if (s->ab_pending && !s->wc_done && now - s->ab_t > c->kx[KX_WC_DELAY_S] && (c->ability_kind == BA_AVOID || c->ability_kind == BA_ABSORB) && bo_attacking(op)) {
        s->wc_done = 1; s->ab_pending = 0; float wr = c->kx[KX_WC_RATE] >= 0 ? c->kx[KX_WC_RATE] : (c->whiff_cancel ? 1.0f : 0.0f);
        if (bc_rand(&s->rng) < wr) { BotCmd d = bot_cmd(BC_DODGE, now, "WC"); d.dir = bot_dodge_dir(c, s, &op->r); bc_push_at(q, d, now); bot_dodge_used(s, now); } }   // whiff cancel: dodge out of a whiffed avoid / absorb
    if (s->ab_pending && now - s->ab_t > 2.0) s->ab_pending = 0;
    if (s->fsm == BS_DEFENCE && now - s->t_state > c->kx[KX_DEF_HOLD_S] && !bo_attacking(op)) s->fsm = BS_NEUTRAL;
    // stamina stop: below stamina_stop of the guard gauge the bot blocks instead of pressing; resumes above stamina_resume
    { float mg = bo_v(me, BO_MAXGUARD), g = bo_v(me, BO_GUARD); if (mg > 0 && g >= 0) { float fr = g / mg;
        if (!s->tired && fr < c->stamina_stop && now >= s->tired_block) { s->tired = 1; s->tired_since = now; s->string_on = 0; s->press_until = 0; }
        else if (s->tired && (fr > c->stamina_resume || now - s->tired_since > c->kx[KX_TIRED_MAX_S])) { s->tired = 0; if (fr <= c->stamina_resume) s->tired_block = now + c->kx[KX_TIRED_CD_S]; } } }   // released by the time cap with stamina still low: no re-entry for tired_cd_s (learned)
    // OPENING: the opponent is stunned (nothing available to it: hit stun, guard break, knock-down recovery) and we are free: attack at once, even when tired
    // (PUNISH covers only an attack's recovery). Gated 0.3 s so one attack is queued at a time.
    if (c->tech[BT_OP] && op->stunned && !bo_attacking(me) && !s->guard_up && bc_pending(q, BC_ATTACK) == 0 && o->dist < c->range_uu + c->kx[KX_OP_RANGE_EXTRA] && c->tech[BT_PN] && now >= s->t_next_open && bc_rand(&s->rng) < c->kx[KX_OP_RATE]) {
        s->t_next_open = now + 0.3; bot_relock(q, s, now); { BotCmd a = bot_cmd(BC_ATTACK, now, "PN"); a.input = bc_rand(&s->rng) < c->kx[KX_OP_ALT_RATE]; bot_push_target(q, a, now, s, c); } bot_note(s, BT_PN, 0); s->fsm = BS_PUNISH; s->t_state = now; s->chain = 0; s->string_on = 1;
        return s->fsm;
    }
    int punish_now = !bo_attacking(me) && !s->guard_up && bc_pending(q, BC_ATTACK) == 0 && o->dist < c->range_uu + c->kx[KX_PUNISH_RANGE_EXTRA] && c->tech[BT_PN] &&
        ((bo_phase(op) == BO_PH_RECOVERY && (!op->r.ok || (float)op->r.release - op->frame >= c->punish_min_f)) || (s->fsm == BS_DEFENCE && now < s->punish_until && !bo_attacking(op)));
    if (punish_now && s->punish_seq != op->seq) { s->punish_seq = op->seq; s->punish_roll = bc_rand(&s->rng) < c->kx[KX_PUNISH_RATE] && (!s->tired || bc_rand(&s->rng) < c->kx[KX_TIRED_PUNISH]); }   // one learned decision per opponent attack
    if (s->tired && !bo_attacking(me) && !(punish_now && s->punish_roll)) {                                                  // only defence while tired (the DEFENCE block above still runs), but not a frozen stance: keep the guard up near the opponent
        if (c->tech[BT_TG] && !s->guard_up && bc_pending(q, -1) == 0 && o->dist < c->range_uu + 60) {
            if (bc_rand(&s->rng) < c->tired_retreat) { BotCmd m = bot_cmd(BC_MOVE, now, "BO"); m.dir = 0; m.secs = c->backoff_s; bc_push_at(q, m, now); }   // step out of range: no guard, so the gauge regenerates at the full rate
            else if (c->tech[BT_GD]) { bot_push_target(q, bot_cmd(BC_GUARD, now, "GD"), now, s, c); s->guard_up = 1; s->guard_until = now + c->tired_guard_s; } }
        return s->fsm; }
    // PUNISH: the opponent is in recovery (or its attack was answered) and we are free
    if (punish_now && s->punish_roll) {
        bot_relock(q, s, now); { BotCmd a = bot_cmd(BC_ATTACK, now, "PN"); a.input = bc_rand(&s->rng) < c->kx[KX_PUNISH_ALT_RATE]; bot_push_target(q, a, now, s, c); } bot_note(s, BT_PN, 0); s->fsm = BS_PUNISH; s->t_state = now; s->chain = 0; s->string_on = 1;
        return s->fsm;
    }
    // OFFENCE: own string running: plan exactly ONE follow-up per own attack (no mashing)
    if (bo_attacking(me) && s->string_on && s->gold_seq != me->seq) {
        s->gold_seq = me->seq;
        if (s->chain < c->chain_max || (now < s->press_until && s->chain < c->chain_max + (int)c->kx[KX_PRESS_CHAIN_EXTRA])) {
            double r = bc_rand(&s->rng);
            int alt = bc_rand(&s->rng) < c->kx[KX_CHAIN_ALT_RATE];
            if (c->tech[BT_U2] && r < c->feint_rate) { BotCmd a = bot_cmd(BC_ATTACK, now, "U2"); a.feint = 1; a.gold = 1; a.input = alt; s->feint_fu = 1; s->feint_fu_until = now + 2.0;                  // delayed feint: off the gold timing,
                a.offset_f = c->delay_min_f + (float)bc_rand(&s->rng) * (c->delay_max_f - c->delay_min_f); bc_push(q, a, &c->skill, &s->rng, 0); bot_note(s, BT_U2, 0); }   // so a buffered punish misses
            else if (bc_rand(&s->rng) < c->delay_rate) { BotCmd a = bot_cmd(BC_ATTACK, now, "DP"); a.gold = 1; a.input = alt;                                 // delay pressing: off-timed plain link
                a.offset_f = c->delay_min_f + (float)bc_rand(&s->rng) * (c->delay_max_f - c->delay_min_f); bc_push(q, a, &c->skill, &s->rng, 0); }
            else if (c->tech[BT_U3] && bc_rand(&s->rng) < c->gold_rate) { BotCmd g = bot_cmd(BC_ATTACK, now, "U3"); g.gold = 1; g.input = alt; g.offset_f = c->gold_offset_f; bc_push(q, g, &c->skill, &s->rng, 0); bot_note(s, BT_U3, 0); }
            else { BotCmd a = bot_cmd(BC_ATTACK, now, "PL"); a.gold = 1; a.input = alt; a.offset_f = c->kx[KX_PLAIN_OFFSET_F]; bc_push(q, a, &c->skill, &s->rng, 0); }   // a plain link (pressed after the gold window)
            s->fsm = BS_OFFENCE;
        } else { s->string_on = 0;
            float mg = bo_v(me, BO_MAXGUARD), g = bo_v(me, BO_GUARD); int low = c->backoff_stamina > 0 && mg > 0 && g >= 0 && g / mg < c->backoff_stamina;
            if (low || bc_rand(&s->rng) < c->backoff_rate) { BotCmd m = bot_cmd(BC_MOVE, now, "BO"); m.dir = 0; m.secs = c->backoff_s; bc_push(q, m, &c->skill, &s->rng, 1); s->press_until = 0; }
            else if (c->tech[BT_U15] && bc_rand(&s->rng) < c->kx[KX_DODGE_AFTER_STRING]) { BotCmd d = bot_cmd(BC_DODGE, now, "DS"); d.dir = 0; bc_push(q, d, &c->skill, &s->rng, 1); bot_dodge_used(s, now); }   // dodge back out of the string
            else if (c->tech[BT_U4] && bc_rand(&s->rng) < c->kx[KX_STANCE_AFTER_STRING]) { int qq = bot_pick_quad(c, s, bo_stance(me)); if (qq >= 0) { BotCmd k = bot_cmd(BC_STANCE, now, "SR"); k.quad = qq; bc_push(q, k, &c->skill, &s->rng, 0); } } }   // stance change in the release after a hit   // the string is over: stay in (pressure) or step back (stamina)
        return s->fsm;
    }
    // after a feint: no idle pause. Once the feint has run (own attack over,
    // no attack command pending) a quick follow-up starter goes out at once if the opponent is in range and not about to hit us.
    if (s->feint_fu) { if (now > s->feint_fu_until || !c->tech[BT_FU]) s->feint_fu = 0;
        else if (!bo_attacking(me) && !s->guard_up && !s->tired && bc_pending(q, BC_ATTACK) == 0 && bo_phase(op) != BO_PH_STARTUP && o->dist <= c->range_uu + 40) {
            s->feint_fu = 0;
            if (bc_rand(&s->rng) < c->kx[KX_STANCE_AFTER_FEINT]) { int qq = bot_pick_quad(c, s, bo_stance(me)); if (qq >= 0) { BotCmd k = bot_cmd(BC_STANCE, now, "SF"); k.quad = qq; bc_push(q, k, &c->skill, &s->rng, 1); s->t_next_neutral = now + c->kx[KX_STANCE_WAIT_S]; return s->fsm; } }
            BotCmd a = bot_cmd(BC_ATTACK, now, "FU"); a.input = bc_rand(&s->rng) < c->kx[KX_FEINT_FU_ALT]; bc_push(q, a, &c->skill, &s->rng, 1); s->chain = 0; s->string_on = 1; s->t_next_neutral = now + c->kx[KX_STARTER_GAP_S]; bot_note(s, BT_U2, 1); return s->fsm; } }
    if (!bo_attacking(me) && s->fsm != BS_DEFENCE) { s->string_on = 0; if (s->fsm == BS_OFFENCE || s->fsm == BS_PUNISH) s->fsm = BS_NEUTRAL; }
    if (bo_attacking(me)) s->t_wait_start = 0;
    // WEAPON: sheathe below sheathe_dur when the opponent is not about to hit; draw when affordable (cost + reserve), calm (out of range or the opponent stunned) and the dice (draw_w) say so
    if (c->wpn_cost > 0 && me->has_weapon && !bo_attacking(me) && !s->guard_up && now >= s->t_next_draw && bc_pending(q, -1) == 0 && bo_phase(op) != BO_PH_STARTUP && bo_phase(op) != BO_PH_ACTIVE) {
        int calm = !bo_attacking(op) && (o->dist >= c->draw_dist || op->stunned);
        if (me->weapon_out && me->wdur >= 0 && me->wdur < c->sheathe_dur) { bc_push_at(q, bot_cmd(BC_DRAW, now, "WS"), now); s->t_next_draw = now + c->kx[KX_DRAW_GAP_S]; return s->fsm; }
        if (!me->weapon_out && me->wdur != 0 && bo_v(me, BO_SHARDS) >= c->wpn_cost + c->draw_reserve && calm && bc_rand(&s->rng) < c->draw_w) { bc_push_at(q, bot_cmd(BC_DRAW, now, "WD"), now); s->t_next_draw = now + c->kx[KX_DRAW_GAP_S]; return s->fsm; }
        s->t_next_draw = now + c->kx[KX_DRAW_TICK_S];
    }
    // POWERS: an opponent power (kind 2) on an opening / at range, a self buff (kind 4) out of range, space (kind 8); affordable after the reserve; learned rates
    if (!bo_attacking(me) && !s->guard_up && now >= s->t_next_power && bc_pending(q, -1) == 0) {
        s->t_next_power = now + 0.25;
        for (int i = 0; i < 2; i++) {
            int k = c->power_kind[i]; if (!(k & 14) || bo_v(me, BO_SHARDS) < c->power_cost[i] + c->power_reserve) continue;
            int use = 0;
            if ((k & 2) && (op->stunned || bo_phase(op) == BO_PH_RECOVERY || o->dist > c->range_uu) && bc_rand(&s->rng) < c->power_off_w) use = 2;
            else if ((k & 4) && !bo_attacking(op) && o->dist > c->range_uu && bc_rand(&s->rng) < c->power_buff_w) use = 4;
            else if ((k & 8) && o->dist < c->spacing_uu && bc_rand(&s->rng) < c->kx[KX_POWER_SPACE_W]) use = 8;   // Shockwave's main use is making space
            if (use) { BotCmd pc = bot_cmd(BC_POWER, now, use == 2 ? "PO" : use == 4 ? "PB" : "PS"); pc.slot = i; bc_push_at(q, pc, now); s->t_next_power = now + 1.0; return s->fsm; }
        }
    }
    // NEUTRAL
    if (s->fsm == BS_NEUTRAL && !bo_attacking(me) && !s->guard_up && now >= s->t_next_neutral && bc_pending(q, -1) == 0) {
        s->t_next_neutral = now + c->kx[KX_NEUTRAL_TICK_S];
        if (o->dist < c->spacing_uu && bc_rand(&s->rng) < c->kx[KX_STEP_OUT_RATE]) { BotCmd m = bot_cmd(BC_MOVE, now, "SO"); m.dir = 0; m.secs = c->kx[KX_MOVE_S]; bc_push_at(q, m, now); return s->fsm; }   // step out to the preferred spacing
        if (o->dist > c->range_uu && c->tech[BT_U15] && bc_rand(&s->rng) < c->kx[KX_DODGE_IN_RATE]) { float mg = bo_v(me, BO_MAXGUARD), g = bo_v(me, BO_GUARD); if (mg <= 0 || g / mg >= c->dodge_stamina_min) { BotCmd d = bot_cmd(BC_DODGE, now, "DI"); d.dir = 3; bc_push_at(q, d, now); bot_dodge_used(s, now); return s->fsm; } }   // dodge in
        if (bc_rand(&s->rng) < c->kx[KX_STRAFE_RATE]) { BotCmd m = bot_cmd(BC_MOVE, now, "SS"); m.dir = bc_rand(&s->rng) < 0.5 ? 1 : 2; m.secs = c->kx[KX_MOVE_S]; bc_push_at(q, m, now); return s->fsm; }   // strafe / circle
        if (c->tech[BT_U4] && bc_rand(&s->rng) < c->kx[KX_STANCE_IDLE_RATE]) { int qq = bot_pick_quad(c, s, bo_stance(me)); if (qq >= 0) { BotCmd k = bot_cmd(BC_STANCE, now, "SI"); k.quad = qq; bc_push(q, k, &c->skill, &s->rng, 1); s->t_next_neutral = now + c->kx[KX_STANCE_WAIT_S]; return s->fsm; } }
        if (o->dist > c->spacing_uu && (o->dist > c->approach_uu || now < s->press_until || bc_rand(&s->rng) < c->pressure)) { BotCmd m = bot_cmd(BC_MOVE, now, "MV"); m.dir = 3; m.secs = c->kx[KX_MOVE_S]; bc_push_at(q, m, now); if (o->dist > c->range_uu) return s->fsm; }
        if (o->dist <= c->range_uu && s->t_wait_start == 0) s->t_wait_start = now;
        double want_rate = now < s->press_until ? 1.0 : c->aggression * c->kx[KX_AGG_SCALE]; if (c->tech[BT_NF] && s->t_wait_start > 0 && now - s->t_wait_start > c->kx[KX_NF_WAIT_S] && (bo_v(op, BO_GUARDING) <= 0 || bc_rand(&s->rng) < c->kx[KX_NF_VS_GUARD])) want_rate = 1.0;   // stall breaker (NF): after nf_wait_s in range without an action the roll is certain              // inside a pressure window: attack at the first chance
        if (o->dist <= c->range_uu && bc_rand(&s->rng) < want_rate && (me->intr_open || !bo_attacking(me))) {
            int cur = bo_stance(me), want = -1, tech = BT_PN;
            if (c->tech[BT_U1] && op->locked_on && bo_v(op, BO_GUARDING) > 0 && bc_rand(&s->rng) < c->kx[KX_U1_RATE]) { want = bot_guard_hole_quad(bo_stance(op)); if (want >= 0) tech = BT_U1; }
            if (want < 0 && c->tech[BT_U4] && bc_rand(&s->rng) < c->stance_rate) { { float tw = c->qpref[0] + c->qpref[1] + c->qpref[2] + c->qpref[3]; double r = bc_rand(&s->rng) * (tw > 0 ? tw : 4.0f), acc = 0; want = 3; for (int qi = 0; qi < 4; qi++) { acc += tw > 0 ? c->qpref[qi] : 1.0f; if (r < acc) { want = qi; break; } } } tech = BT_U4; }   // weighted stance pick (all weights 1 = uniform draw)
            s->t_wait_start = 0;
            if (want >= 0 && want != cur) { BotCmd k = bot_cmd(BC_STANCE, now, BT_IDS[tech]); k.quad = want; bc_push(q, k, &c->skill, &s->rng, 1); bot_note(s, tech, 0); s->t_next_neutral = now + c->kx[KX_STANCE_WAIT_S]; return s->fsm; }
            snprintf(b, sizeof b, "%s", tech == BT_U1 ? "U1" : "ST");
            bot_relock(q, s, now); BotCmd a = bot_cmd(BC_ATTACK, now, b); if (c->alt_rate > 0 && bc_rand(&s->rng) < c->alt_rate) a.input = 1;
            if (c->tech[BT_U2] && bc_rand(&s->rng) < c->kx[KX_NEUTRAL_FEINT_RATE]) { a.feint = 1; s->feint_fu = 1; s->feint_fu_until = now + 2.0; }   // a feinted starter
            bc_push(q, a, &c->skill, &s->rng, 1); s->chain = 0; s->string_on = 1; s->t_next_neutral = now + c->kx[KX_STARTER_GAP_S];
        }
    }
    (void)dt; (void)prev; return s->fsm;
}
#endif

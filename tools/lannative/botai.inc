// botai.inc - the scripted bot's ADAPTER (the game side of botcore.h).
// Default OFF: nothing happens without `bot_ai = on` in the loader's config.txt. The decisions are pure (botcore.h + bot_cmd.h + botobs.h);
// this file only (1) reads the observation, (2) executes commands through the game's OWN orders, (3) logs.
//
// Which pawns: a pawn spawned from a "bots" content entry that has a "brain" object (bots.inc hook g_bt_spawn_cb). Its behaviour tree is stopped
// and the brain drives it; its opponent is the other brain bot if there is one (bot vs bot), else the local player.
//
// Action interface. Every action is gated by AFightingCharacter::IsAvailable(action) (vtable +0x900, the check the
// stock AI and the player's input stack use), so the bot can do nothing a player could not do at that moment:
//   attack   DelayedActionAttack + UAttackComponent::PrepareToLaunchAttack (= UAttackBTTask::LaunchAttack). Deck path (row ""): the row comes from the bot's own deck
//            via UComboManager::GetNextAttack(input, wanted quadrant) like a player's press; forced path (row id): m_bForceDebugPlay. Flags: perfect link (gold) only if
//            IsAvailable(Attack1Perfect) and UAttackComponent::CanPerfectLink(row); feint = m_bAttackCancel (the attack is cancelled at its m_iFrameCancel, the AI's feint).
//   gold     press emulation (bot_cmd.h bc_gold_resolve); the press offset vs the window opening is logged.
//   guard    AFightingCharacter::HandleGuard         dodge  AFightingCharacter::PlayDodgeOrder
//   ability  DelayedAction + AFightingCharacter::HandleSpecialAbilityDetection (= UParryBTTask::LaunchSpecial: parry / avoid / absorb; Drunken abilities are attack rows: bai_drunken)
//   stance   UFightingMovementComponent::LaunchChangeQuadrant(start None, end q) (= UQuadrantChangeBTTask), UpdateChangeQuadrant every tick
//   power    AFightingCharacter::LaunchItemUseOrder(dir) (= UUsePowerBTTask::LaunchPower; the archetype's m_ItemLeftUseCase / m_ItemRightUseCase item)
//   lock     the character's SetTarget UFunction          move   APawn::AddMovementInput towards / away from the opponent
// Logs: "bot: ..." lines (act / tech / link / def / match). bot_reads_inputs is NOT used: the bot sees only game state.
#include "botcore.h"
#include "botnn.h"
#include "botnn_net.h"

#define BAI_MAX 32                                                                 // up to 16 pairs at once
#define BAI_PRESETS 32
#define BAI_ROWS 64
typedef struct { char id[48], mod[64]; char *json; } BaiPreset;
typedef struct { char name[24]; double open_after; } BaiWin;                   // learned gold window opening per row (seconds after the attack start)
typedef struct {
    void *ch, *op; char id[48]; int used; BotCfg cfg; BotState st; BotQueue q; BotObs o;
    unsigned seq; double atk_t; char row[24];                                      // own attack (on_attack_start)
    int gold_prev; double gold_open_t; BotCmd hold; int holding; double hold_press;  // gold window edge, held press
    int move_on; double move_until; int move_dir; double pw_ready[2];   /* pw_ready: power cooldown end (the game never charges an NPC's power: the adapter does) */
    int wins, ko_lock; double hp_seen;
    double hit_chk; int gd_want, walk_slow, rush; double tog_t, fwd_t; /* gd_want: guard held while unavailable (stun), applied when allowed */ int pending_ab, pending_gd, pending_dg, pending_pw; double stance_t; int stance_want;
    int pair; long refused; double bt_check, tgt_check, tgt_log, def_t, dodge_t, ab_t; int ab_whiff_done; int bt_restarts; long ticks, ticks_range, attacks, hits_landed; char last_tech[8]; int feint_state; unsigned feint_base; int last_gold_cmd; float last_offset_f; double pend_t;
    BaiWin win[BAI_ROWS]; int nwin;
    int ch_e, ch_i, ch_q, lk_ab_ok; unsigned cx_seq, gd_seq, pr_seq; int feint_next, pr_n, cx_open; unsigned cx_land; double cx_end; double ch_until, lk_ab_t; float nn_pp[2][2]; double nn_pt;   // ch_*: own chain position (fallback continues it); lk_ab_*: ability re-press lock; cx_*: the landed feint (post-feint rules); cx_seq: attack already cancelled; gd_seq: attack whose gold link was dismissed; pr_seq / pr_n: presses per attack (anti-spam); feint_next: a late press feints the next attack; nn_pp / nn_pt: last poses (me, op) for the 2D speeds
    double nn_t, nn_buf_t; BotSide nn_hist[16]; float nn_ghist[16][6]; int nn_hi, nn_delay, nn_round, nn_buf; long nn_tick, nn_last_press, nn_acts[NNA_N];   // the neural-network bot (policy nn)
    int ally; double ally_t; void *ally_op;   // brain.ally = true: fights the nearest enemy near the player, never the player
    int want_sword, sword_in_hand, sdeck_n[4], bdeck_n[4]; char sdeck[4][BOT_DECK_MAX][16], salt[4][16], bdeck[4][BOT_DECK_MAX][16], balt[4][16]; int done, frozen, idle_fix, last_type; char last_ctech[8]; double idle_chk, frozen_until, last_cmd_t, stall_log, wp_t, wp_log, t_draw, t_sheathed, t_toggle; int draws, sheathes; float opp_guard_ema, dur_draw, ophp_draw;   // weapons layer: a preset with brain.weapon = sword + deck_sword draws / sheathes the sword by policy (bai_weapon_policy)
} BaiBot;
typedef struct { void *ch; unsigned seq; char row[24]; double t; } BaiAtk;
static const AbsApi *BAI;
static int g_bai_on, g_bai_verbose = 1, g_bai_match_rounds, g_bai_round, g_bai_draws, g_bai_hooked; static double g_bai_round_t, g_bai_last_report, g_bai_round_max = 60;
typedef struct { int round, draws, wins[2], mw[2], sp_ok; double round_t; void *a, *b; float sp[2][3], sr[2]; } BaiPair;   // one match per pair (bots mode: pair = adoption order / 2); mw: round wins in the running first-to-N match (shards carry over inside it); sp: spawn spots at match start; sr: health share at round start (heal-on-kill curve)
static BaiPair g_bai_pairs[BAI_MAX / 2]; static int g_bai_adopt_n;
#define BAI_ATK_N (BAI_MAX + 8)                                                            // one slot per bot (+ spare): a bot without a slot has its attacks unrecorded, so nobody defends against them
static BaiBot g_bai[BAI_MAX]; static BaiPreset g_bai_pre[BAI_PRESETS]; static int g_bai_npre; static BaiAtk g_bai_atk[BAI_ATK_N];
static double g_bai_t0;
static double bai_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart - g_bai_t0; }

// ----- native functions (symbols.bin names; fallback = this build's RVA)
typedef char (*BaiIsAvail_t)(void *ch, uint8_t action, char predict, char ignore_reject, uint8_t ctx);
static BaiIsAvail_t g_bai_isavail; static uintptr_t g_bai_isavail_va; static int g_bai_fn_state;
static double bai_cfg_num(const char *key, double dflt);
static void *g_bai_canperfect, *g_bai_nextattack, *g_bai_dact, *g_bai_hsad, *g_bai_abinput, *g_bai_itemuse, *g_bai_lcq, *g_bai_ucq, *g_bai_lca, *g_bai_scs; static int g_bai_lca_ok;   // lca / scs: mid-attack feint natives
static int bai_fns(void) {
    if (g_bai_fn_state) return g_bai_fn_state > 0;
    g_bai_isavail_va = (uintptr_t)(g_base + se_rva_sym("AFightingCharacter::IsAvailable", 0x339470u));
    g_bai_canperfect = g_base + se_rva_sym("UAttackComponent::CanPerfectLink", 0x2EEAC0u);
    g_bai_nextattack = g_base + 0x2F8D60u;                                         // UComboManager::GetNextAttack(this, FName *ret (hidden), EQuadrantTypes *out_quadrant (may be NULL), InputAction, EQuadrantTypes, bool rush, ECardinalPoints) - overloaded name: RVA
    g_bai_dact = g_base + se_rva_sym("DelayedAction::DelayedAction", 0x2E3010u);
    g_bai_hsad = g_base + se_rva_sym("AFightingCharacter::HandleSpecialAbilityDetection", 0x364B80u);
    g_bai_abinput = g_base + se_rva_sym("UDefenseComponent::GetSpecialAbilityInputAction", 0x334BC0u);
    g_bai_itemuse = g_base + se_rva_sym("AFightingCharacter::LaunchItemUseOrder", 0x365D90u);
    g_bai_lca = g_base + se_rva_sym("UAttackComponent::LaunchCancelAttack", 0x302090u); g_bai_scs = g_base + se_rva_sym("UAttackComponent::SetCancelAttackStatus", 0x30C310u);
    { static const uint8_t pro[16] = { 0x44, 0x88, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55 };   // expected prologue bytes
      __try { g_bai_lca_ok = !memcmp(g_bai_lca, pro, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { g_bai_lca_ok = 0; }
      logf_("bot: native LaunchCancelAttack %p prologue %s, SetCancelAttackStatus %p (mid-attack feints %s)", g_bai_lca, g_bai_lca_ok ? "OK" : "DIFFERS", g_bai_scs, g_bai_lca_ok ? "on" : "OFF"); }
    g_bai_lcq = g_base + se_rva_sym("UFightingMovementComponent::LaunchChangeQuadrant", 0x43F4A0u);
    g_bai_ucq = g_base + se_rva_sym("UFightingMovementComponent::UpdateChangeQuadrant", 0x450B70u);
    static const uint8_t nxt[6] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 };          // GetNextAttack prologue (mov [rsp+8], rbx; ...) checked below, logged either way
    int ok = tk_fn_ok(TKF_PREPARE) && tk_fn_ok(TKF_DAACTOR) && tk_fn_ok(TKF_DACTOR);
    logf_("bot: natives IsAvailable %p CanPerfectLink %p GetNextAttack %p (prologue %02X %02X %02X %s) DelayedAction %p HandleSpecialAbilityDetection %p GetSpecialAbilityInputAction %p LaunchItemUseOrder %p LaunchChangeQuadrant %p UpdateChangeQuadrant %p testkit natives %d",
          (void *)g_bai_isavail_va, g_bai_canperfect, g_bai_nextattack, ((uint8_t *)g_bai_nextattack)[0], ((uint8_t *)g_bai_nextattack)[1], ((uint8_t *)g_bai_nextattack)[2], memcmp(g_bai_nextattack, nxt, 5) ? "(differs: deck path off)" : "ok",
          g_bai_dact, g_bai_hsad, g_bai_abinput, g_bai_itemuse, g_bai_lcq, g_bai_ucq, ok);
    if (memcmp(g_bai_nextattack, nxt, 5)) g_bai_nextattack = NULL;
    g_bai_fn_state = ok ? 1 : -1; return ok;
}
// IsAvailable through the character's vtable slot +0x900, used only when that slot IS AFightingCharacter::IsAvailable (checked per call: cheap)
static int bai_avail(void *ch, int action) {
    if (!tk_ptr_ok(ch)) return 0; int r = 0;
    __try { void **vt = *(void ***)ch; void *f = vt[0x900 / 8]; if ((uintptr_t)f != g_bai_isavail_va) { static int n; if (n++ < 3) logf_("bot: IsAvailable slot is %p, expected %p - availability unknown (treated as available)", f, (void *)g_bai_isavail_va); return 1; }
            r = ((BaiIsAvail_t)f)(ch, (uint8_t)action, 1, 0, 0) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}
// ABaseCharacter::IsOrderRunning(EOrderType, EOrderAccessMode) 0x33AA40; EOrderType Attack = 1; mode 2 assumed RunningOrPending (logged per mode in the first attacks)
static int bai_order_running(void *ch, int type, int mode) { int r = -1; __try { r = ((char (*)(void *, uint8_t, uint8_t))(g_base + 0x33AA40u))(ch, (uint8_t)type, (uint8_t)mode) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; } return r; }
static int g_bai_mode_bots;                                                           // config bot_ai_mode = bots: bot-vs-bot mode (never the player); default: every bot fights the player
static char g_bai_vs_npc[64];                                                       // config bot_ai_vs_npc: archetype path part of a STOCK NPC the bot fights (baseline matches)
typedef struct { void *self, *found; } BaiFind;
static void bai_find_cb(void *o, void *u) { BaiFind *f = (BaiFind *)u; if (f->found || o == f->self || !BAI->is_a(BAI, o, "FightingCharacter")) return; void *ar = BAI->npc_archetype(BAI, o); char ap[300] = "";
    if (ar && BAI->object_path(BAI, ar, ap, sizeof ap) && strstr(ap, g_bai_vs_npc)) f->found = o; }
static void *g_bai_npc_cache;                                                         // cleared by bai_on_destroyed (never use a destroyed pawn)
static void *bai_find_npc(void *self) {   // a miss is rescanned once per second, not every tick (a full object scan every tick costs too much frame time)
    void *cache = g_bai_npc_cache; static double t, t0, warned; double now = bai_now(); if (t > 0 && now - t < (cache ? 2.0 : 1.0) && (!cache || tk_ptr_ok(cache))) return cache;
    t = now; if (t0 == 0) t0 = now; BaiFind f = { self, NULL }; BAI->for_each_object(BAI, bai_find_cb, &f); g_bai_npc_cache = f.found;
    if (!f.found && now - t0 > 10.0 && !warned) { warned = 1; logf_("bot: WARNING baseline mode: no NPC archetype path contains '%s' after 10 s (check bot_ai_vs_npc; a shell may have rewritten /Game/...)", g_bai_vs_npc); }
    return f.found; }
static BaiBot *bai_of(void *ch) { for (int i = 0; i < BAI_MAX; i++) if (g_bai[i].used && g_bai[i].ch == ch) return &g_bai[i]; return NULL; }
static int bai_is_ally_bot(void *ch) { BaiBot *b = bai_of(ch); return b && b->ally; }
static BaiAtk *bai_atk_of(void *ch, int add) { for (int i = 0; i < BAI_ATK_N; i++) if (g_bai_atk[i].ch == ch) return &g_bai_atk[i]; if (!add) return NULL; for (int i = 0; i < BAI_ATK_N; i++) if (!g_bai_atk[i].ch) { memset(&g_bai_atk[i], 0, sizeof g_bai_atk[i]); g_bai_atk[i].ch = ch; return &g_bai_atk[i]; } { static int warned; if (!warned++) logf_("bot: WARNING the attack table is full (%d slots): some pawns' attacks are not tracked", BAI_ATK_N); } return NULL; }
static const char *bai_who(void *ch) { BaiBot *b = bai_of(ch); return b ? b->id : ch && ch == cd_local_pawn() ? "player" : "npc"; }

// ----- attack row data
static int bai_row(const char *name, BotRow *r) {
    memset(r, 0, sizeof *r); if (!name || !name[0]) return 0; snprintf(r->name, sizeof r->name, "%s", name);
    __try {
        uint8_t *p = api_p_table_row(BAI, CT_ATTACKS, name); if (!p) return 0;
        r->startup = *(int *)(p + 0x110); if (r->startup <= 0) r->startup = *(int *)(p + 0x10C); r->cancel = *(int *)(p + 0x114); r->strike = *(int *)(p + 0x118);
        r->perfect = *(int *)(p + 0x134); r->release = *(int *)(p + 0x138); r->irelease = *(int *)(p + 0x13C); r->start_q = p[0xD0]; r->end_q = p[0xD1]; r->motion = p[0x144]; { int og = p[0x148]; r->origin = og == 0 ? 0 : og == 2 ? 1 : 2; }   /* EAttackOrigin in memory: Left 0, Middle 1, Right 2  -> BotRow order: left 0, right 1, middle 2 */
        r->range = *(float *)(p + 0x158); r->adv_hit = *(int *)(p + 0x1C0); r->adv_guard = *(int *)(p + 0x1C4);
        // m_iFrameStrike1 / m_iFrameRelease are small numbers (row 004: strike 2) while the hit lands at the startup frame:
        // they count from the end of the build-up. Absolute frames for the bot: hit = startup, end of the move = startup + max(release, interruptible release).
        { int rel = r->release > r->irelease ? r->release : r->irelease; r->strike = r->startup; if (rel <= 0 || rel > 120) rel = 15; r->release = r->startup + rel; }
        r->ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r->ok = 0; }
    return r->ok;
}
static double bai_win_get(BaiBot *b, const char *row) { for (int i = 0; i < b->nwin; i++) if (!strcmp(b->win[i].name, row)) return b->win[i].open_after; return -1; }
static void bai_win_put(BaiBot *b, const char *row, double v) {
    for (int i = 0; i < b->nwin; i++) if (!strcmp(b->win[i].name, row)) { b->win[i].open_after = bc_learn(b->win[i].open_after, v); return; }
    if (b->nwin < BAI_ROWS) { snprintf(b->win[b->nwin].name, sizeof b->win[b->nwin].name, "%s", row); b->win[b->nwin++].open_after = v; }
}

// ----- observation
static void *bai_target_of(void *ch) { void *t = NULL; __try { void *f = api_p_find_function(BAI, ch, "GetTarget"); if (f) api_p_call_params(BAI, ch, f, NULL, &t); } __except (EXCEPTION_EXECUTE_HANDLER) { t = NULL; } return se_valid_ptr(t) ? t : NULL; }
static int bai_ability_action(void *ch) { int r = 99; __try { void *dc = se_comp(BAI, ch, "m_DefenseComponent"); if (dc) r = ((uint8_t (*)(void *))g_bai_abinput)(dc); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 99; } return r; }
static void *bai_weapon_actor(void *ch); static float bai_weapon_dur(void *ch);   // defined with the weapon actions below
static int bai_avail(void *ch, int action);
static void bai_side(void *ch, void *other, BotSide *s, double now) {
    memset(s, 0, sizeof *s); for (int i = 0; i < IR_SIDE_N; i++) s->s.v[i] = -1; if (!tk_ptr_ok(ch)) return;
    AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; if (!api_p_char_info(BAI, ch, &ci)) return;
    s->s.v[0] = ci.health; s->s.v[1] = ci.max_health; s->s.v[2] = ci.guard; s->s.v[3] = ci.max_guard; s->s.v[4] = (float)ci.stance; s->s.v[5] = (float)ci.style;
    s->s.v[6] = (float)ci.attacking; s->s.v[7] = (float)ci.guarding; s->s.v[8] = (float)ci.is_down;
    void *ac = se_comp(BAI, ch, "m_AttackComponent"); void *f = ac ? api_p_find_function(BAI, ac, "BPF_GetShards") : NULL; float sh = -1; if (f && api_p_call_params(BAI, ac, f, NULL, &sh)) s->s.v[9] = sh;
    // attacking: BPF_IsPlayingAttack (char_info) is 0 for the player's forced attacks and stays 1 for a driven bot long after its attack, so the attack
    // state comes from the last attack start + the row's frame data: running until max(release, interruptible release) (fallback strike + 15 F)
    BaiAtk *k = bai_atk_of(ch, 0); s->s.v[6] = 0;
    if (k && k->row[0]) { s->seq = k->seq; s->atk_t = k->t; float fr = (float)((now - k->t) * 30.0); BotRow r; bai_row(k->row, &r);
        int end = r.ok ? r.release : 27;
        if (fr <= end && bai_order_running(ch, 1, 2) != 0) { s->frame = fr; s->s.v[10] = fr; snprintf(s->row, sizeof s->row, "%s", k->row); s->r = r; s->s.v[6] = 1; } }   // a cancelled / interrupted attack ends early (no Attack order left)
    // windows: PerfectAttackOnly allows Attack1Perfect but not Attack1; GenericAll allows Attack1 but not the perfect actions; no layer allows both
    int g19 = bai_avail(ch, 19), g0 = bai_avail(ch, 0);
    if (s->s.v[6] > 0) { s->gold_open = g19 && !g0; s->intr_open = g0 && !g19; } else { s->gold_open = 0; s->intr_open = g0; }
    int ab = bai_ability_action(ch); s->ability_ok = ab < 99 && bai_avail(ch, ab);
    s->stunned = s->s.v[6] <= 0 && s->s.v[7] <= 0 && !g0 && !g19 && !bai_avail(ch, 2) && !bai_avail(ch, 5);
    if (!s->stunned && s->s.v[6] <= 0 && (bai_order_running(ch, 5, 2) == 1 || bai_order_running(ch, 4, 2) == 1 || bai_order_running(ch, 11, 2) == 1)) s->stunned = 1;   // EOrderType Hitted 5 (hit / block / guard-break stun), Parried 4, Avoided 11: the availability check does not apply to NPC pawns   // not while guarding (Guard is unavailable WHILE guarding: a blocking pawn would read as stunned)
    s->locked_on = other && bai_target_of(ch) == other; s->valid = 1;
    s->weapon_out = bai_weapon_actor(ch) != NULL; s->wdur = bai_weapon_dur(ch); s->has_weapon = s->weapon_out || s->wdur >= 0;   /* has_weapon approximated by a readable durability (an equipped weapon keeps its gauge) [unverified] */
}
static float bai_dist(void *a_, void *b_) { float a[3], b[3]; if (!api_p_test_get_location(BAI, a_, a) || !api_p_test_get_location(BAI, b_, b)) return 9999; return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])); }

// ----- actions
// ----- weapons: draw / sheathe (AFightingCharacter::LaunchDrawSheatheOrder toggles; a draw costs shards), weapon in hand, weapon health
static void *bai_weapon_actor(void *ch) { void *w = NULL; __try { void *f = api_p_find_function(BAI, ch, "BPF_GetFirstHandledWeaponActor"); if (f) api_p_call_params(BAI, ch, f, NULL, &w); } __except (EXCEPTION_EXECUTE_HANDLER) { w = NULL; } return se_valid_ptr(w) ? w : NULL; }
static int bai_draw_toggle(void *ch) { static uint32_t rva; if (!rva) rva = api_p_find_symbol(BAI, "AFightingCharacter::LaunchDrawSheatheOrder"); int r = -1; if (!rva) return -1;
    __try { r = ((char (*)(void *, char, char, char))(g_base + rva))(ch, 1, 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { r = -2; } return r; }
static float bai_weapon_dur(void *ch) { float *p = (float *)api_p_prop_ptr(BAI, ch, "m_fHandledWeaponDurability"); return p && tk_ptr_ok(p) ? *p : -1.f; }   // 1.0 = full
static void bai_set_shards(void *ch, float v) { void **ap = (void **)api_p_prop_ptr(BAI, ch, "m_AttackComponent"); void *ac = ap ? *ap : NULL, *f = ac ? api_p_find_function(BAI, ac, "BPF_SetShards") : NULL; void *args[1] = { &v }; if (f) api_p_call_params(BAI, ac, f, args, NULL); }
static int g_bai_hp_trace;                                                          // config bot_ai_hp_trace = 1: one health / guard line per bot every 0.5 s (damage vs regen analysis)
static void bai_freeze(BaiBot *b, double now, double secs) {                                   // round settle (in-flight attacks and hit reactions must not leak into the next round): invincible, no commands, empty queue
    b->frozen = 1; b->frozen_until = now + secs; if (b->o.me.s.v[BO_MAXHEALTH] > 0) b->o.me.s.v[BO_HEALTH] = b->o.me.s.v[BO_MAXHEALTH]; /* the cached health must not retrigger the round end */ bc_clear(&b->q); b->holding = 0; b->move_on = 0; api_p_test_set_invincible(BAI, b->ch, 1); }
// round reset: health / ghost / down / guard (api_p_test_heal), shards = bot_ai_start_shards (default 0), weapon durability 1.0, both pawns
// invincible and idle for 1 s (in-flight attacks and reactions settle), then FACED OFF at 250 uu on the pair's own axis, facing each other.
static float g_bai_reset_dist = 250.f;
static void bai_face_off(BaiBot *a, BaiBot *c) {
    float pa[3], pc[3]; if (!api_p_test_get_location(BAI, a->ch, pa) || !api_p_test_get_location(BAI, c->ch, pc)) return;
    float dx = pc[0] - pa[0], dy = pc[1] - pa[1], d = sqrtf(dx * dx + dy * dy); if (d < 1.f) { dx = 1.f; dy = 0.f; d = 1.f; } dx /= d; dy /= d;
    float mx = (pa[0] + pc[0]) * 0.5f, my = (pa[1] + pc[1]) * 0.5f, h = g_bai_reset_dist * 0.5f, ya = atan2f(dy, dx) * 57.29578f, yc = ya + 180.f;
    float na[3] = { mx - dx * h, my - dy * h, pa[2] }, nc[3] = { mx + dx * h, my + dy * h, pc[2] };
    int r1 = api_p_test_teleport(BAI, a->ch, na, &ya), r2 = api_p_test_teleport(BAI, c->ch, nc, &yc);
    static int nlog; if (nlog++ < 6) logf_("bot: round reset: %s and %s faced off at %.0f uu (was %.0f), teleport %d/%d", a->id, c->id, g_bai_reset_dist, d, r1, r2);
    for (int k = 0; k < 2; k++) { BaiBot *b = k ? c : a; float *dur = (float *)api_p_prop_ptr(BAI, b->ch, "m_fHandledWeaponDurability"); if (dur && tk_ptr_ok(dur)) *dur = 1.0f; }
}
// 1v1 round flow (as the game's own 1v1): only the LOSER respawns fresh, at the spawn spot farthest from the winner, and the fight goes on at once
// (no face-off: the winner may rush the respawn or wait); the winner keeps its health / stamina and gets the heal-on-kill 150 x HealthRefillCoeff(winner start share - loser start share)
// (3.0 at -0.8, 1.0 at 0, 0 at +0.8) [BP_TPSCharacter]. Weapon durability is not touched (an inventory item: it carries over).
static float bai_heal_coef(float x) { x = x < -0.8f ? -0.8f : x > 0.8f ? 0.8f : x; return x < 0 ? 1.f - 2.5f * x : 1.f - 1.25f * x; }
static void bai_add_health(void *ch, float add) { void *hc = tk_comp(BAI, ch, "m_HealthComponent"); if (!hc) return; float mx = se_prop_f(BAI, hc, "m_fMaxHealth") + se_prop_f(BAI, hc, "m_fBonusMaxHealth"); float *hp = (float *)api_p_prop_ptr(BAI, hc, "m_fHealth"); if (hp && tk_ptr_ok(hp) && mx > 0) *hp = *hp + add > mx ? mx : *hp + add; }
static void bai_respawn_far(void *loser, void *winner, float sp[2][3], int sp_ok) {
    float pw[3], pl[3]; if (!api_p_test_get_location(BAI, winner, pw) || !api_p_test_get_location(BAI, loser, pl)) return;
    float *s = NULL; if (sp_ok) { float d0 = (sp[0][0] - pw[0]) * (sp[0][0] - pw[0]) + (sp[0][1] - pw[1]) * (sp[0][1] - pw[1]), d1 = (sp[1][0] - pw[0]) * (sp[1][0] - pw[0]) + (sp[1][1] - pw[1]) * (sp[1][1] - pw[1]); s = d0 > d1 ? sp[0] : sp[1]; }
    float to[3] = { s ? s[0] : pl[0], s ? s[1] : pl[1], s ? s[2] : pl[2] }, yaw = atan2f(pw[1] - to[1], pw[0] - to[0]) * 57.29578f;
    int r = api_p_test_teleport(BAI, loser, to, &yaw); static int nlog; if (nlog++ < 6) logf_("bot: round flow: loser respawned at the far spawn (%.0f uu from the winner), teleport %d", sqrtf((to[0] - pw[0]) * (to[0] - pw[0]) + (to[1] - pw[1]) * (to[1] - pw[1])), r);
}
static int g_bai_match_to = 3;   // first to 3 rounds like a player 1v1; shards carry over between its rounds and start at bot_ai_start_shards (config bot_ai_match_to, 0 = reset every round)
static int g_bai_start_shards = 0;   // config bot_ai_start_shards: shards a bot gets at the start of each round (tests); -1 = leave them
static void bai_swap_deck(BaiBot *b, int sword) {                                    // the brain's own deck rows (fallback / feint) follow the weapon in hand; the game's combo manager already switches decks with the weapon
    for (int e = 0; e < 4; e++) { if (sword) { b->cfg.deck_n[e] = b->sdeck_n[e]; memcpy(b->cfg.deck[e], b->sdeck[e], sizeof b->sdeck[e]); memcpy(b->cfg.alt[e], b->salt[e], sizeof b->salt[e]); }
        else { b->cfg.deck_n[e] = b->bdeck_n[e]; memcpy(b->cfg.deck[e], b->bdeck[e], sizeof b->bdeck[e]); memcpy(b->cfg.alt[e], b->balt[e], sizeof b->balt[e]); } } }
// Weapon policy (weapon health, shard management, when to use it). Measured: close contact costs ~0.13 durability per 5 s, shards cap at 5, the game draws the sword itself on the first sword attack.
//  SHEATHE  tier 1 (< 35 % durability): at a SAFE moment = own attack idle and (opponent knocked down, or opponent idle and >= 250 uu away, or opponent in the recovery of a long attack);
//           tier 2 (< 15 %): as soon as the bot is idle and the opponent is not in the startup / active frames of an attack (a broken weapon cannot be redrawn).
//  DRAW     the sword deck is the option when: weapon healthy (>= 60 %, or unknown), shards cover the draw (>= 3), the opponent is not turtling (guard share EMA < 0.5: a guarded sword just wears down),
//           calm (neutral, >= 400 uu); a CAPPED shard bar (>= 4.5) is a sink: draw at >= 250 uu (unspent shards are wasted at 5).
//  Logged: durability / shards every 5 s (also sheathed: the heal rate), sheathe duration, and the sword's efficiency per draw = opponent health taken per 0.1 durability lost.
static void bai_weapon_policy(BaiBot *b, double now) {
    if (!b->want_sword || now - b->wp_t < 0.25) return; double dt = now - b->wp_t; b->wp_t = now; (void)dt;
    int in_hand = bai_weapon_actor(b->ch) != NULL; float dur = bai_weapon_dur(b->ch), sh = b->o.me.s.v[9], ophp = b->o.op.s.v[BO_HEALTH];
    int me_atk = b->o.me.s.v[BO_ATTACKING] > 0, op_atk = b->o.op.s.v[BO_ATTACKING] > 0, op_down = b->o.op.s.v[BO_DOWN] > 0, ph = bo_phase(&b->o.op);
    b->opp_guard_ema += 0.05f * ((b->o.op.s.v[BO_GUARDING] > 0 ? 1.f : 0.f) - b->opp_guard_ema);                          // share of time the opponent keeps its guard up
    if (in_hand != b->sword_in_hand) { b->sword_in_hand = in_hand; bai_swap_deck(b, in_hand);
        if (in_hand) { b->dur_draw = dur; b->ophp_draw = b->o.op.valid && ophp > 0 ? ophp : -1.f; b->t_draw = now; }
        else { float lost = b->dur_draw - dur, took = b->ophp_draw - ophp; b->t_sheathed = now;
            logf_("bot: %s: sword used %.0f s: durability %.2f -> %.2f, opponent health %.0f -> %.0f (%.0f health per 0.1 durability)", b->id, now - b->t_draw, b->dur_draw, dur, b->ophp_draw, ophp, lost > 0.001f && b->ophp_draw > 0 ? took / (lost * 10.f) : 0.f); }
        logf_("bot: %s: weapon %s (durability %.2f, shards %.1f, opponent guard share %.2f) - brain deck rows now the %s deck", b->id, in_hand ? "DRAWN" : "SHEATHED", dur, sh, b->opp_guard_ema, in_hand ? "sword" : "bare-hands"); }
    static double lastdur_log[BAI_MAX]; int bi = (int)(b - g_bai); if (now - lastdur_log[bi] > 5.0) { lastdur_log[bi] = now; logf_("bot: %s: weapon %s: durability %.2f, shards %.1f, distance %.0f uu, opponent guard share %.2f", b->id, in_hand ? "in hand" : "sheathed", dur, sh, b->o.dist, b->opp_guard_ema); }
    int act = 0, r = 0; const char *why = "";
    if (b->cfg.wpn_cost > 0) return;   // the brain decides draw / sheathe (BC_DRAW, knobs draw_w / draw_reserve / sheathe_dur); this fixed policy is the fallback for presets without them
    int safe = !me_atk && (op_down || (!op_atk && b->o.dist >= 250.f) || (ph == BO_PH_RECOVERY && bo_frames_to_hit(&b->o.op) < -20.f));
    int idle_ok = !me_atk && ph != BO_PH_STARTUP && ph != BO_PH_ACTIVE;
    int healthy = dur < 0 || dur >= 0.6f, capped = sh >= 4.5f;
    if (now - b->wp_log > 3.0) {
        if (in_hand && dur >= 0 && dur < 0.15f && idle_ok) { act = 2; why = "sheathe (tier 2): below 15 % durability, bot idle and opponent not mid-strike"; }
        else if (in_hand && dur >= 0 && dur < 0.35f && (safe || (!me_atk && !op_atk && b->st.fsm == BS_NEUTRAL))) { act = 2; why = "sheathe (tier 1): below 35 % durability at a quiet moment (knock-down / idle opponent / neutral gap)"; }
        else if (!in_hand && healthy && sh >= 3.f && b->opp_guard_ema < 0.5f && !me_atk && !op_atk && ((b->st.fsm == BS_NEUTRAL && b->o.dist >= 400.f) || (capped && (b->o.dist >= 250.f || safe)))) { act = 1; why = capped ? "draw: shards capped (spend them), weapon healthy, opponent not turtling" : "draw: calm, shards cover it, weapon healthy, opponent not turtling"; } }
    if (act) { b->wp_log = now; r = bai_draw_toggle(b->ch); if (act == 1) b->draws++; else b->sheathes++;
        logf_("bot: %s: %s -> LaunchDrawSheatheOrder %d (durability %.2f, shards %.1f, distance %.0f uu)", b->id, why, r, dur, sh, b->o.dist); }
}
static int bai_set_target(void *ch, void *t) { int ok = 0; __try { void *f = api_p_find_function(BAI, ch, "SetTarget"); void *args[1] = { &t }; ok = f && api_p_call_params(BAI, ch, f, args, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; } return ok; }
// guard only when available (IsAvailable action 2); otherwise held (gd_want) and applied when the game allows it
static int bai_guard(BaiBot *b, int on) { if (!on) { b->gd_want = 0; return api_p_test_guard(BAI, b->ch, 0); } if (bai_avail(b->ch, 2)) { b->gd_want = 0; return api_p_test_guard(BAI, b->ch, 1); } b->gd_want = 1; return 0; }
// a deck row launched from stance q: its natural start, or mirrored into q when q is the left / right mirror of that start (FL<->FR, BR<->BL: the deck
// editor's mirrored placement); -1 = it cannot start from q
static int bai_row_quad(const BotRow *r, int q) { return r->start_q == q || (r->start_q >= 0 && r->start_q <= 3 && (r->start_q ^ 1) == q) ? q : -1; }
// Mid-attack feint: cancel the bot's RUNNING attack like a player's guard press in the cancel window. UAttackComponent::LaunchCancelAttack
// (the AI's launch-time feint runs the same pair; the cancel is scheduled AT the row's m_iFrameCancel) checks nothing itself, so
// the gates are the game's: the row can be cancelled, the window is still open (frame < m_iFrameCancel - 3, OrderAttack m_bCancelWindowOpened), once per
// attack, not while hit, stamina > 0. Cost: OrderCancel::OnStart takes the flat 10 stamina itself. Returns 1 = scheduled.
static int bai_cancel(BaiBot *b) {
    const BotSide *me = &b->o.me; if (!g_bai_lca_ok || !bo_attacking(me) || !me->r.ok || me->r.cancel < 0) return 0;
    if (me->frame >= (float)(me->r.cancel - 3) || me->stunned || (b->cx_seq && b->cx_seq == me->seq)) return 0;
    if (me->s.v[BO_MAXGUARD] > 0 && me->s.v[BO_GUARD] <= 0.f) return 0;
    int ok = 0;
    __try { uint8_t *ac = se_comp(BAI, b->ch, "m_AttackComponent"); if (!ac) return 0; uint8_t id = ac[0x630], out[2] = { 0, 0 };   // m_uiCurrentAttackOrderIDPending
            ((void *(*)(void *, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t))g_bai_lca)(ac, out, id, 1, 0, 0, 0);
            if (out[0]) { ((void (*)(void *, uint8_t, uint8_t))g_bai_scs)(ac, id, 1); ok = 1; b->cx_seq = me->seq; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bot: %s: cancel EXCEPTION 0x%08lX", b->id, GetExceptionCode()); ok = 0; }
    if (g_bai_verbose > 1 || ok) logf_("bot: %s: mid-attack feint %s: row %s frame %.1f (cancel frame %d)", b->id, ok ? "scheduled" : "refused by the game", me->row, me->frame, me->r.cancel);
    return ok;
}
static long g_gold_no_spam, g_gold_no_dism, g_gold_try;   // gold-link gate counters (logged at the match end)
static void bai_press(BaiBot *b) { if (b->o.me.s.v[BO_ATTACKING] > 0) { if (b->pr_seq != b->o.me.seq) { b->pr_seq = b->o.me.seq; b->pr_n = 0; } b->pr_n++; } }   // one real attack press during an own attack (anti-spam count; buffered retries are not presses)
static int bai_is_rushing(BaiBot *b, float *spent);
// returns 1 launched; *perf_out = the perfect flag actually used; row_out = the row
static int bai_attack(BaiBot *b, const BotCmd *c, int want_perfect, int *perf_out, char *row_out) {
    void *ch = b->ch; *perf_out = 0; row_out[0] = 0; int ok = 0;
    if (b->ally && b->op) { void *pl = cd_local_pawn(); float m[3], p[3], o[3];   // ally friendly fire: no swing while the player is close and in front (a sweep at the enemy would hit the player in between)
        if (pl && api_p_test_get_location(BAI, ch, m) && api_p_test_get_location(BAI, pl, p) && api_p_test_get_location(BAI, b->op, o)) {
            float px = p[0] - m[0], py = p[1] - m[1], ox = o[0] - m[0], oy = o[1] - m[1], pd = sqrtf(px * px + py * py), od = sqrtf(ox * ox + oy * oy);
            if (pd < 260.f && pd < od + 60.f && (pd < 1.f || od < 1.f || (px * ox + py * oy) / (pd * od) > 0.34f)) { static double fl; static int fn; fn++; if (b->o.t - fl > 2.0) { fl = b->o.t; logf_("bot: %s: attack held - the player is in the swing (player %.0f uu, enemy %.0f uu, #%d)", b->id, pd, od, fn); } return 0; } } }
    if (b->o.me.s.v[BO_MAXGUARD] > 0 && b->o.me.s.v[BO_GUARD] <= 0.f) { if (g_bai_verbose > 1) logf_("bot: %s: attack refused: no stamina", b->id); return 0; }   // no attack on an empty guard gauge (the game checks it for players; the NPC order path does not)
    __try {
        uint8_t *ac = se_comp(BAI, ch, "m_AttackComponent"); if (!ac) return 0;
        AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; api_p_char_info(BAI, ch, &ci);
        uint64_t name = 0; int quad = ci.stance >= 0 && ci.stance <= 3 ? ci.stance : 0, force = 0, input = c->input ? 1 : 0;
        if (ci.stance < 0 || ci.stance > 3) { uint8_t *wq = api_p_prop_ptr(BAI, ac, "m_eWantedQuadrant"); if (wq && *wq <= 3) quad = *wq; }   // fallback when the stance read is out of range
        if (c->row[0]) {                                                                  // forced row (any table row: guard-hole openers, tests)
            BotRow r; if (!bai_row(c->row, &r)) { logf_("bot: %s: attack row %s is not in the attacks table", b->id, c->row); return 0; }
            quad = tk_quadrant_pick(r.start_q, quad); wchar_t w[40]; u8_to_w(c->row, w, 40); FNameRaw fn = fname_of(w); name = *(uint64_t *)&fn; force = 1;
        } else {                                                                          // the bot's own deck: what a press of Attack1 / Attack2 would play now
            void **cmp = api_p_prop_ptr(BAI, ac, "m_ComboManager"); void *cm = cmp && se_valid_ptr(*cmp) ? *cmp : NULL;
            if (!cm || !g_bai_nextattack) { logf_("bot: %s: no combo manager / GetNextAttack - deck attack refused", b->id); return 0; }
            { float sp = 0; if (bai_is_rushing(b, &sp) && sp > 0.5f) {   // a press after > 0.5 s of rushing = the rush attack (GetPotentialNextNode param 4 -> GetRushAttackNode) [code: CanPerformRushAttack]
                ((void *(*)(void *, uint64_t *, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t))g_bai_nextattack)(cm, &name, NULL, (uint8_t)input, (uint8_t)quad, 1, 5);
                { static int nr; char tmp[24] = ""; if (name) fname_text(*(FNameRaw *)&name, tmp, 24); if (nr++ < 20) logf_("bot: %s: RUSH-ATTACK after %.2f s rushing -> %s", b->id, sp, name ? tmp : "(none: deck attack)"); } } }
            if (!name) ((void *(*)(void *, uint64_t *, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t))g_bai_nextattack)(cm, &name, NULL, (uint8_t)input, (uint8_t)quad, 0, 5);
            { static int na; if (input && na < 25) { char tmp[24] = ""; if (name) fname_text(*(FNameRaw *)&name, tmp, 24); na++; logf_("bot: %s: alternate starter: GetNextAttack(input 1, quadrant %d) -> %s", b->id, quad, name ? tmp : "(nothing: falls back to the main string)"); } }
            if (!name && input) { input = 0; ((void *(*)(void *, uint64_t *, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t))g_bai_nextattack)(cm, &name, NULL, 0, (uint8_t)quad, 0, 5); }
            if (!name && !input && b->ch_e >= 0 && bai_now() < b->ch_until && b->ch_i + 1 < b->cfg.deck_n[b->ch_e]) {   // continue the bot's own chain first (the root fallback below alone would repeat one row)
                BotRow r2; int q2 = b->ch_q >= 0 && b->ch_q <= 3 ? b->ch_q : quad;   // the stance the previous row ends in (the game may still report its start stance during the gold window)
                if (bai_row(b->cfg.deck[b->ch_e][b->ch_i + 1], &r2) && bai_row_quad(&r2, q2) >= 0) { wchar_t w2[40]; u8_to_w(r2.name, w2, 40); FNameRaw f2 = fname_of(w2); name = *(uint64_t *)&f2; force = 1; quad = q2; } }
            if (!name) {   // no next node from the combo manager: restart from the deck's root row of this stance (forced), like a player's fresh press
                for (int pass = 0; pass < 1 && !name; pass++)   /* only pass 0 runs: this stance's root, like a player's fresh press (passes 1 / 2, any root / any row, are disabled) */ for (int e = 0; e < 4 && !name; e++) for (int i2 = 0; i2 < b->cfg.deck_n[e] && !name; i2++) { BotRow r2; if (!bai_row(b->cfg.deck[e][i2], &r2)) continue;
                    if (pass == 0 && !(i2 == 0 && e == quad && bai_row_quad(&r2, quad) >= 0)) continue;
                    if (pass == 1 && !(i2 == 0 && bai_row_quad(&r2, quad) >= 0)) continue;
                    wchar_t w2[40]; u8_to_w(r2.name, w2, 40); FNameRaw f2 = fname_of(w2); name = *(uint64_t *)&f2; force = 1; if (pass == 2) quad = tk_quadrant_pick(r2.start_q, quad); }
                static int nfb; if (name && nfb++ < 20) logf_("bot: %s: no next node from the combo manager (input %d) - deck root fallback (forced)", b->id, input);
                if (!name) { if (g_bai_verbose > 1) logf_("bot: %s: the deck has no next attack (input %d quadrant %d)", b->id, input, quad); return 0; } }
        }
        fname_text(*(FNameRaw *)&name, row_out, 24);
        int act = input ? 1 : 0, gp = bai_avail(ch, input ? 20 : 19), pp = bai_avail(ch, act), busy = b->o.me.s.v[BO_ATTACKING] > 0;
        int gold_avail = busy ? gp && !pp : 0, plain_avail = busy ? pp && !gp : pp;      // during an own attack: only inside its gold / interruptible window (no layer = mid-move: a player cannot link there)
        int perfect = 0;
        if (busy && want_perfect && (b->pr_n >= 2 || b->gd_seq == b->o.me.seq)) { if (b->gd_seq == b->o.me.seq) g_gold_no_dism++; else g_gold_no_spam++; want_perfect = 0; }
        if (busy && want_perfect && gold_avail) g_gold_try++;   // a 2nd press or a dismissed link = no gold (like a player's IsPerfectLinkValid)
        if (want_perfect && gold_avail) { uint8_t *row = api_p_table_row(BAI, CT_ATTACKS, row_out); perfect = row && ((char (*)(void *, void *))g_bai_canperfect)(ac, row) != 0; }
        if (!perfect && !plain_avail && c->slot != 74) { /* slot 74 = a Drunken ability launch (bai_drunken): its own availability (IsAvailable DrunkenAttack) was checked by bai_ability, plain Attack1 availability is not required */ if (g_bai_verbose > 1) logf_("bot: %s: attack %s refused: not available (gold %d plain %d)", b->id, row_out, gold_avail, plain_avail); return 0; }
        int nofeint = 0;   /* a row that cannot be cancelled is thrown without the feint (no deck-row swap decided by the adapter) */
        if (c->feint) { BotRow r; if (bai_row(row_out, &r) && r.cancel < 0) { nofeint = 1; static int nl2; if (nl2++ < 20) logf_("bot: %s: feint dropped: row %s cannot be cancelled (m_iFrameCancel %d)", b->id, row_out, r.cancel); }
            if (0) {
                int found = 0; for (int pass = 0; pass < 2 && !found; pass++) for (int q2 = 0; q2 < 4 && !found; q2++) for (int i2 = 0; i2 < b->cfg.deck_n[q2] && !found; i2++) { BotRow r2; if (bai_row(b->cfg.deck[q2][i2], &r2) && r2.cancel >= 0 && (pass || r2.start_q == quad)) {
                    snprintf(row_out, 24, "%s", r2.name); wchar_t w2[40]; u8_to_w(row_out, w2, 40); FNameRaw f2 = fname_of(w2); name = *(uint64_t *)&f2; force = 1; found = 1; } }
                if (!found) { logf_("bot: %s: feint refused: row %s cannot be cancelled (m_iFrameCancel %d) and no deck row of stance %d can", b->id, row_out, r.cancel, quad); return 0; }
                logf_("bot: %s: feint: row %s cannot be cancelled, using deck row %s", b->id, r.name, row_out); } }
        { char ap[300]; int la = se_row_anim_ensure(BAI, CT_ATTACKS, row_out, 1, ap, sizeof ap); if (la == 1 && g_bai_verbose > 1) logf_("bot: %s: loaded the animation of row %s", b->id, row_out); }
        if (tk_ptr_ok(b->op)) { api_weak_set((WeakPtr *)(ac + TK_OFF_ATK_TARGET), b->op); api_weak_set((WeakPtr *)(ac + TK_OFF_ATK_NEXT), b->op); }
        uint8_t *us = api_p_prop_ptr(BAI, ac, "m_bPerfectLinkUsesShards"); uint8_t consume = us ? *us : 0;
        uint8_t tf[0x60], daa[TK_DAA_SIZE + 0x20]; memset(tf, 0, sizeof tf); memset(daa, 0, sizeof daa);
        ((void *(*)(void *, uint8_t, void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint64_t, uint8_t, uint8_t))(g_base + g_tkf[TKF_DAACTOR].rva))(daa, 0, tf, (uint8_t)input, (uint8_t)quad, (uint8_t)perfect, (uint8_t)(c->feint && !nofeint ? 1 : 0), consume, name, 0, 5);
        if (daa[0x70] != (uint8_t)input || daa[0x71] != (uint8_t)quad || daa[0x74] != (uint8_t)perfect || daa[0x75] != (uint8_t)(c->feint && !nofeint ? 1 : 0) || daa[0x81] != 5) { logf_("bot: %s: DelayedActionAttack layout differs - not launched", b->id); return 0; }
        daa[TK_DAA_FORCE] = (uint8_t)force;
        if (force) daa[0x82] = 1;   // m_bIsSpecialAttack: with force the game never mirrors; with this byte it mirrors when quadrant (+0x71) = the opposite of the row's start (PrepareToLaunchAttack 0x140308FC0)
        ((void (*)(void *, void *))(g_base + g_tkf[TKF_PREPARE].rva))(ac, daa);
        *perf_out = perfect; ok = 1;
        { int fe = -1, fi = -1; for (int e = 0; e < 4 && fe < 0; e++) for (int i2 = 0; i2 < b->cfg.deck_n[e]; i2++) if (!strcmp(b->cfg.deck[e][i2], row_out)) { fe = e; fi = i2; break; }   // chain position of what was launched
          BotRow rl; int okr = bai_row(row_out, &rl); double len = okr ? (rl.release > 0 ? rl.release : 30) / 30.0 : 1.0; b->ch_e = input ? -1 : fe; b->ch_i = fi; b->ch_until = bai_now() + len + 0.35;
          b->ch_q = !okr || rl.end_q < 0 || rl.end_q > 3 ? -1 : quad == rl.start_q ? rl.end_q : rl.end_q ^ 1; }   // end stance; a mirrored launch ends mirrored
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bot: %s: attack EXCEPTION 0x%08lX", b->id, GetExceptionCode()); ok = 0; }
    return ok;
}
// ----- Drunken style abilities (Stagger drunken avoids, Faejin JKD parries / avoids). They are attack ROWS launched by the Drunken input (UParryBTTask::LaunchCustomSpecial: IsAvailable(DrunkenAttack 74) ->
// combo node by stance -> DelayedActionAttack). The adapter launches the same row by name through the forced-row path of bai_attack (the dodge / safety window comes from the row's data).
// Table: Stagger [stance][West, North, South], Faejin [FL / BL source stance][neutral stopper, vs a left-origin strike, vs any other].
// the right-hand stances (FR, BR) use the left rows placed mirrored (the quadrant pick of bai_attack).
/* DrunkenDB / JeetDB [stance FL, FR, BR, BL][North, East, South, West] (the mirror follows from the stance in bai_attack) */
static const char *const BAI_STAG_ROWS[4][4] = { { "183", "185", "189", "186" }, { "183", "186", "189", "185" }, { "184", "187", "190", "188" }, { "184", "188", "190", "187" } };
static const char *const BAI_FAE_ROWS[4][4] = { { "220", "223", "221", "222" }, { "220", "222", "221", "223" }, { NULL, "226", "225", "227" }, { NULL, "227", "225", "226" } };
static int bai_drunken(BaiBot *b, int var) {   /* network: var 0..3 = North / East / South / West (the stick, like a player); scripted brain: its variant 0..3 */
    int style = (int)b->o.me.s.v[BO_STYLE], q = bo_stance(&b->o.me); if (q < 0 || (style != 4 && style != 5)) return 0;
    const char *row; int variant = 0;
    if (b->cfg.policy == BP_NN) variant = var >= 0 && var <= 3 ? var : 0;
    else if (style == 4) { static const int m4[4] = { 3, 3, 0, 2 }; variant = m4[var >= 0 && var <= 3 ? var : 0]; }   // Stagger: 0 / 1 West (side avoid), 2 North, 3 South
    else if (var >= 1 && var <= 3) { static const int m5[3] = { 0, 3, 1 }; variant = m5[var - 1]; }                // Faejin: 1 North (stopper), 2 West (parry L), 3 East (parry R)
    else { BotSide *os = &b->o.op; variant = q >= 2 ? 2 : 0; if (bo_attacking(os) && os->r.ok && os->r.origin <= 1) variant = os->r.origin == 0 ? 3 : 1; }   // from the strike it SEES
    row = style == 4 ? BAI_STAG_ROWS[q][variant] : BAI_FAE_ROWS[q][variant];
    if (!row) { if (g_bai_verbose > 1) logf_("bot: %s: no drunken row (style %d stance %d direction %d)", b->id, style, q, variant); return 0; }
    BotCmd c; memset(&c, 0, sizeof c); c.type = BC_ATTACK; c.slot = 74; snprintf(c.row, sizeof c.row, "%s", row); int perf = 0; char ro[24] = "";
    int ok = bai_attack(b, &c, 0, &perf, ro); static int nlog; if (nlog++ < 30) logf_("bot: %s: drunken ability (style %d stance %d variant %d) row %s -> %d", b->id, style, q, variant, row, ok); return ok;
}
static int bai_ability(BaiBot *b, int var) {
    int ok = 0;
    __try {
        void *dc = se_comp(BAI, b->ch, "m_DefenseComponent"); if (!dc) return 0; int act = ((uint8_t (*)(void *))g_bai_abinput)(dc);
        if (act >= 99) { logf_("bot: %s: no style ability", b->id); return 0; }
        if (bai_target_of(b->ch) != b->op) { if (g_bai_verbose > 1) logf_("bot: %s: ability refused: not locked on", b->id); return 0; }   // APSpecialAbilityDetection needs a lock target (Drunken too: bai_drunken is called below)
        if (b->o.me.s.v[BO_MAXGUARD] > 0 && b->o.me.s.v[BO_GUARD] <= 0.f) return 0;   // no stamina: no ability
        { double lk = b->cfg.ability_kind == BA_PARRY ? 22 : b->cfg.ability_kind == BA_AVOID ? 19 : b->cfg.ability_kind == BA_ABSORB ? 20 : 0;   // re-press lock: the clip's own re-press frame (game data:
          if (lk > 0 && !b->lk_ab_ok && bai_now() - b->lk_ab_t < lk / 30.0) return 0; }   //  parry F22, avoid F19, absorb F20), earlier only after a successful one
        if (!bai_avail(b->ch, act)) { if (g_bai_verbose > 1) logf_("bot: %s: ability (action %d) not available now", b->id, act); return 0; }
        if (b->cfg.ability_kind == BA_DRUNKEN) return bai_drunken(b, var);                                      // Stagger / Faejin: the ability is an attack row
        int8_t ty = 0; BaiAtk *k = bai_atk_of(b->op, 0); void *oac = b->op ? se_comp(BAI, b->op, "m_AttackComponent") : NULL; uint8_t *orow = k && k->row[0] ? api_p_table_row(BAI, CT_ATTACKS, k->row) : NULL;
        // the direction is the BOT's choice like a player's stick (var 0..3), never GetAdaptedSpecial (= always right)
        // parry: EQuadrantTypes, Left = FL / BL, Right = FR / BR by the bot's front / back stance; avoid: EAvoidType Duck 0, Jump 1, LeftSide 2, RightSide 3; absorb 0
        (void)oac; (void)orow;
        if (var == 0 && b->cfg.policy != BP_NN && bo_attacking(&b->o.op) && b->o.op.r.ok) {   // the scripted brain picks the stick like a player, from the strike it SEES (perceived row)
            if (b->cfg.ability_kind == BA_PARRY) var = b->o.op.r.origin == 1 ? 1 : 0;   // right origin -> Right parry, else Left
            else if (b->cfg.ability_kind == BA_AVOID) var = b->o.op.r.motion == 1 ? 0 : 2;   // horizontal -> Duck, thrust / vertical -> Side (BotRow has no hit height: low sweeps are misread; needs m_eHitTarget1)
        }
        { int bk = bo_stance(&b->o.me) >= 2; if (b->cfg.ability_kind == BA_PARRY) ty = (int8_t)(var == 1 ? (bk ? 2 : 1) : (bk ? 3 : 0)); else if (b->cfg.ability_kind == BA_AVOID) ty = (int8_t)(var >= 0 && var <= 3 ? var : 0); else ty = 0; }
        uint8_t *prm = api_p_prop_ptr(BAI, dc, "m_uiCurrentSpecialAbilityEnumParam"); if (!prm) prm = (uint8_t *)dc + 0x1B9;   /* not a UPROPERTY: PDB offset */ if (prm) *prm = (uint8_t)(ty >= 0 ? ty : 0);
        if (g_bai_verbose > 1) logf_("bot: %s: ability type %d vs row %s (param byte now %d)", b->id, ty, k ? k->row : "-", prm ? *prm : -1);
        uint8_t tf[0x60], obj[0xA0]; memset(tf, 0, sizeof tf); memset(obj, 0, sizeof obj);
        ((void *(*)(void *, uint8_t, void *))g_bai_dact)(obj, 0, tf);
        ((void (*)(void *, void *))g_bai_hsad)(b->ch, obj); ok = 1; b->lk_ab_t = bai_now(); b->lk_ab_ok = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bot: %s: ability EXCEPTION 0x%08lX", b->id, GetExceptionCode()); ok = 0; }
    return ok;
}
static int bai_power(BaiBot *b, int slot, double now) {
    if (!bai_avail(b->ch, slot ? 44 : 45)) { if (g_bai_verbose > 1) logf_("bot: %s: power slot %d not available now", b->id, slot); return 0; }
    // an NPC's LaunchItemUseOrder is never charged by the game (shards and cooldown are spent on the player's inventory path only), so the adapter
    // charges the item's cost (brain knob power_cost<slot>) and enforces its cooldown (1.4 s + power_cd<slot>, default 1.5 s) itself
    float cost = b->cfg.power_cost[slot & 1], sh = b->o.me.s.v[BO_SHARDS];
    if (now < b->pw_ready[slot & 1]) { if (g_bai_verbose > 1) logf_("bot: %s: power slot %d on cooldown (%.2f s left)", b->id, slot, b->pw_ready[slot & 1] - now); return 0; }
    if (cost > 0 && sh >= 0 && sh + 1e-3f < cost) { if (g_bai_verbose > 1) logf_("bot: %s: power slot %d refused: %.2f shards, costs %.0f", b->id, slot, sh, cost); return 0; }
    int ok = 0; __try { ((void (*)(void *, uint8_t))g_bai_itemuse)(b->ch, (uint8_t)(slot ? 1 : 0)); ok = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bot: %s: power EXCEPTION 0x%08lX", b->id, GetExceptionCode()); }
    if (ok) { if (cost > 0 && sh >= 0) { bai_set_shards(b->ch, sh - cost); b->o.me.s.v[BO_SHARDS] = sh - cost; } b->pw_ready[slot & 1] = now + 1.4 + (b->cfg.power_cd[slot & 1] > 0 ? b->cfg.power_cd[slot & 1] : 1.5); }
    return ok;
}
static int bai_bt_running(void *ch) {                                          // the stock behaviour tree of a driven bot must stay stopped (BrainComponent::IsRunning)
    int r = -1; __try { void *ctl = se_comp(BAI, ch, "Controller"); void **mp = ctl ? api_p_prop_ptr(BAI, ctl, "m_BehaviorComp") : NULL; void *br = mp && se_valid_ptr(*mp) ? *mp : NULL;
        void *f = br ? api_p_find_function(BAI, br, "IsRunning") : NULL; uint8_t v = 0; if (f && api_p_call_params(BAI, br, f, NULL, &v)) r = v != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
static void *bai_movecomp(void *ch) { void *mc = se_comp(BAI, ch, "CharacterMovement"); return mc && api_p_is_a(BAI, mc, "FightingMovementComponent") ? mc : NULL; }
static int bai_stance(BaiBot *b, int q) {
    if (bai_target_of(b->ch) != b->op) return 0;   // APQuadrantChangeDetection runs only with a lock target
    void *mc = bai_movecomp(b->ch); if (!mc || q < 0 || q > 3) return 0; AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; api_p_char_info(BAI, b->ch, &ci);
    if ((b->o.me.s.v[BO_ATTACKING] > 0 && !b->o.me.intr_open) || ci.stance == q) {   /* a stance change is legal in the interruptible release; the live effect of LaunchChangeQuadrant there is unverified */ if (g_bai_verbose > 1) logf_("bot: %s: stance %d refused (attacking %.0f, stance %d)", b->id, q, b->o.me.s.v[BO_ATTACKING], ci.stance); return ci.stance == q; }
    int ok = 0; __try { uint8_t st = 5, en = (uint8_t)q; ((void (*)(void *, uint8_t *, uint8_t *, uint8_t, uint8_t, uint8_t))g_bai_lcq)(mc, &st, &en, 1, 0, 0); ok = 1; }   // param 1 = 1: no SwitchIdle order (it causes idle pauses and refused attacks)
    __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bot: %s: stance EXCEPTION 0x%08lX", b->id, GetExceptionCode()); }
    return ok;
}
// with the stock tree stopped the movement component stays in move status Exploration with MaxWalkSpeed 10000, so every movement input would run at
// ~10,000 uu/s. The fight tree normally switches a pawn to Combat (lock-move walking speed). The bot sets Combat itself: BPF_SetMoveStatus(value), read back as text; the value that
// reads 'Combat' is kept (bottrial.inc: 0 = Exploration). Returns 1 when the pawn is in Combat.
static int g_bai_combat_ms = 1;
static int bai_combat_move(BaiBot *b) {
    void *mc = se_comp(BAI, b->ch, "CharacterMovement"); if (!mc) return 0; char st[48] = ""; BAI->get_property_text(BAI, mc, "m_eMoveStatus", st, sizeof st); if (strstr(st, "Combat")) return 1;
    void *f = api_p_find_function(BAI, mc, "BPF_SetMoveStatus"); if (!f) return 0; static int tries, nlog;
    for (int k = 0; k < 4; k++) { uint8_t v = (uint8_t)g_bai_combat_ms; void *a1[1] = { &v }; api_p_call_params(BAI, mc, f, a1, NULL); st[0] = 0; BAI->get_property_text(BAI, mc, "m_eMoveStatus", st, sizeof st);
        if (strstr(st, "Combat")) { if (nlog++ < 6) logf_("bot: %s: move status set to Combat (value %d)", b->id, g_bai_combat_ms); return 1; }
        if (tries++ < 6) logf_("bot: %s: BPF_SetMoveStatus(%d) reads back '%s' - trying the next value", b->id, g_bai_combat_ms, st); g_bai_combat_ms = (g_bai_combat_ms + 1) % 4; if (g_bai_combat_ms == 0) g_bai_combat_ms = 1; }
    return 0;
}
static int bai_explore_move(BaiBot *b) {   // unlocked: the game's Exploration move status (faces the run direction, run speed) like a player without a lock target
    void *mc = se_comp(BAI, b->ch, "CharacterMovement"); if (!mc) return 0; char st[48] = ""; BAI->get_property_text(BAI, mc, "m_eMoveStatus", st, sizeof st); if (strstr(st, "Exploration")) return 1;
    void *f = api_p_find_function(BAI, mc, "BPF_SetMoveStatus"); if (!f) return 0; static int ev = -1, nlog;
    for (int k = 0; k < 4; k++) { uint8_t v = (uint8_t)(ev >= 0 ? ev : k); void *a1[1] = { &v }; api_p_call_params(BAI, mc, f, a1, NULL); st[0] = 0; BAI->get_property_text(BAI, mc, "m_eMoveStatus", st, sizeof st);
        if (strstr(st, "Exploration")) { ev = v; if (nlog++ < 6) logf_("bot: %s: move status set to Exploration (value %d)", b->id, v); return 1; } if (ev >= 0) { ev = -1; } }
    if (nlog++ < 6) logf_("bot: %s: no BPF_SetMoveStatus value gives Exploration (reads '%s')", b->id, st); return 0;
}
/* UFightingMovementComponent: m_bIsRushing +0xB1D, m_fRushCoolDownTimer +0xB20, m_fTimeSpentRushing +0xB2C; SetIsRushing @ 0x44b510
   (refuses while the cooldown runs; stamina drain + the 3 s burn-out cooldown are the game's own UpdateRush) */
static int bai_is_rushing(BaiBot *b, float *spent) { uint8_t *mc = (uint8_t *)bai_movecomp(b->ch); int r = 0; float t = 0; __try { if (mc) { r = mc[0xB1D] != 0; t = *(float *)(mc + 0xB2C); } } __except (EXCEPTION_EXECUTE_HANDLER) { } if (spent) *spent = t; return r; }
static void bai_set_rushing(BaiBot *b, int on) { uint8_t *mc = (uint8_t *)bai_movecomp(b->ch); if (!mc || (mc[0xB1D] != 0) == (on != 0)) return;
    __try { ((void (*)(void *, uint8_t))(g_base + se_rva_sym("UFightingMovementComponent::SetIsRushing", 0x44b510u)))(mc, (uint8_t)(on != 0)); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    static int nlog; if (nlog++ < 20) logf_("bot: %s: rush %s -> m_bIsRushing %d (cooldown %.2f)", b->id, on ? "on" : "off", mc[0xB1D], *(float *)(mc + 0xB20)); }
static void bai_move(BaiBot *b, int dir, double now) {
    static void *fn; if (!fn) fn = api_p_find_function(BAI, b->ch, "AddMovementInput"); if (!fn) return;
    { static double ct[BAI_MAX]; int ci = (int)(b - g_bai); if (ci >= 0 && ci < BAI_MAX && now - ct[ci] > 0.25) { ct[ci] = now; if (b->st.unlocked) bai_explore_move(b); else bai_combat_move(b); } }   // Combat move status before moving; unlocked = the game's own Exploration movement (faces the run direction), not Combat strafing at run speed
    if (bo_attacking(&b->o.me) || b->o.me.stunned || (b->atk_t > 0 && now - b->atk_t < 0.4)) {   /* atk_t = the attack-started event: the per-tick attacking read lags it */ static int nl2; if (nl2++ < 20) logf_("bot: %s: MOVE-BLOCKED: own attack / stun (the game's IsAvailable does not gate NPC attacks)", b->id); return; }   // movement only in Idle / Guard
    if (!bai_avail(b->ch, b->st.unlocked ? 10 : 11)) { static int nlog; if (nlog++ < 20) logf_("bot: %s: MOVE-BLOCKED: the game allows no %s now (attack / hit / dodge / ability)", b->id, b->st.unlocked ? "Move" : "LockMove"); return; }   // no movement input during actions: a player's stick does nothing there (InputAction Move 10 / LockMove 11)
    float a[3], o[3]; if (!api_p_test_get_location(BAI, b->ch, a) || !api_p_test_get_location(BAI, b->op, o)) return;
    // safety: stop the move when the last step was impossible for a walk (> 1,500 uu/s; a walk is ~3 uu per frame) and log it
    { static float mp[BAI_MAX][3]; static double mt[BAI_MAX]; static int nlog; int bi = (int)(b - g_bai); if (bi >= 0 && bi < BAI_MAX) { double dt = now - mt[bi]; float dx0 = a[0] - mp[bi][0], dy0 = a[1] - mp[bi][1], sp = dt > 0 && dt < 0.2 ? sqrtf(dx0 * dx0 + dy0 * dy0) / (float)dt : 0.f;
        mp[bi][0] = a[0]; mp[bi][1] = a[1]; mp[bi][2] = a[2]; mt[bi] = now;
        { static double vt[BAI_MAX]; if (now - vt[bi] > 0.25) { vt[bi] = now; void *mc = se_comp(BAI, b->ch, "CharacterMovement"); char v[96] = "?", mst[48] = "?", mm[48] = "?", mw[32] = "?"; uint8_t ss = 0xFF;
            if (mc) { void *f = api_p_find_function(BAI, mc, "BPF_GetCurrentSpeedState"); if (f) api_p_call_params(BAI, mc, f, NULL, &ss); BAI->get_property_text(BAI, mc, "Velocity", v, sizeof v); BAI->get_property_text(BAI, mc, "m_eMoveStatus", mst, sizeof mst); BAI->get_property_text(BAI, mc, "MovementMode", mm, sizeof mm); BAI->get_property_text(BAI, mc, "MaxWalkSpeed", mw, sizeof mw); }
            logf_("bot: MOVE-PROBE %s: dir %d, %.0f uu/s measured, velocity %s, move status %s, mode %s, max walk %s, speed state %d, distance %.0f", b->id, dir, sp, v, mst, mm, mw, ss, b->o.dist); } }
        if (sp > 1500.f) { b->move_on = 0; if (nlog++ < 40) logf_("bot: MOVE-GUARD %s: %.0f uu/s while moving (dir %d, distance %.0f) - move stopped", b->id, sp, dir, b->o.dist); return; } } }
    float dx = o[0] - a[0], dy = o[1] - a[1], d = sqrtf(dx * dx + dy * dy); if (d < 1) return; dx /= d; dy /= d;
    static const int dv[8][2] = { { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };   // dodge vectors: back, left, right, toward, FL, FR, BL, BR
    int di = dir == 3 ? 3 : dir >= 0 && dir < 8 ? dir : 3; float tx = (float)dv[di][0], ty = (float)dv[di][1], tn = sqrtf(tx * tx + ty * ty); tx /= tn; ty /= tn;
    float v[3] = { tx * dx + ty * -dy, tx * dy + ty * dx, 0 };   // toward = (dx, dy), left = (-dy, dx)
    // an outside movement input is scaled by MaxWalkSpeed (10000 on these pawns). A walking input is analog (speed = MaxWalkSpeed x input size),
    // so the input size is set to the wanted speed / MaxWalkSpeed (walk speeds measured on the player, below)
    void *mc = se_comp(BAI, b->ch, "CharacterMovement"); float mws = mc ? se_prop_f(BAI, mc, "MaxWalkSpeed") : 0.f; float want = tx < -0.1f ? 1.f / sqrtf((tx / 160.f) * (tx / 160.f) + (ty / 200.f) * (ty / 200.f)) : 200.f; if (b->walk_slow) want *= 0.5f;
    { int fwd = di == 3 || di == 4 || di == 5; if (!b->st.unlocked) b->rush = 0; if (fwd) b->fwd_t = now; bai_set_rushing(b, b->rush && (fwd || now - b->fwd_t < 0.3));   /* the game keeps the rush wish 0.3 s without full forward stick: no on / off flicker */   /* rush: unlocked, forward stick only (drains only moving forward) */
      if (b->st.unlocked) want = b->rush && fwd && bai_is_rushing(b, NULL) ? 610.f : 430.f; }   /* unlocked: jog 430, rush 610; the game's ellipse: forward / side 200, back 160; half stick = walk */   /* locked walk forward / sideways 200, back 160 uu/s */
    float scale = mws > want ? want / mws : 1.f; uint8_t force = 1; void *args[3] = { v, &scale, &force }; __try { api_p_call_params(BAI, b->ch, fn, args, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static int bai_dodge(BaiBot *b, int dir) {
    float a[3], o[3]; if (!api_p_test_get_location(BAI, b->ch, a) || !api_p_test_get_location(BAI, b->op, o)) return 0;
    float ax = a[0] - o[0], ay = a[1] - o[1], d = sqrtf(ax * ax + ay * ay); if (d < 1) return 0; ax /= d; ay /= d;
    static const int dv[8][2] = { { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };   // dodge vector [toward, left]
    int di = dir >= 0 && dir < 8 ? dir : 0; float tx = (float)dv[di][0], ty = (float)dv[di][1], tn = sqrtf(tx * tx + ty * ty); tx /= tn; ty /= tn;
    float v[3] = { -tx * ax + ty * ay, -tx * ay - ty * ax, 0 };   // away = (ax, ay): toward = -away, left = (ay, -ax)   // 1 = LEFT of the line to the opponent, 2 = right (= bai_move)
    if (!bai_avail(b->ch, 5)) { if (g_bai_verbose > 1) logf_("bot: %s: dodge not available now", b->id); return 0; }
    if (b->o.me.s.v[BO_MAXGUARD] > 0 && b->o.me.s.v[BO_GUARD] <= 0.f) return 0;   // no dodge on an empty guard gauge
    return api_p_test_dodge(BAI, b->ch, v);
}


// ================================================================ neural-network bot (policy "nn")
// The network decides at 30 Hz (one combat frame). Observation: botnn.h from this adapter's BotObs, the opponent seen nn_delay decision ticks late (reaction ~100 ms,
// sampled per round) with its move hidden for reveal_f frames. Actions: the primitive actions of botnn_net.h (NNA_*), executed with the calls the scripted bot uses.
// A press is buffered 0.35 s like the player's input buffer (the game refuses an attack that is not available NOW); no new press within 2 ticks of the last (press-rate limit).
// Config: bot_ai_nn = <absolute path of policy.bin>. A preset uses it with brain.policy = nn.
static NnNet g_bai_nn; static int g_bai_nn_tried, g_bai_nn_midfeint; /* midfeint: config bot_ai_nn_mid_feint = 1 for a network trained with mid-attack feints */ static FILE *g_bai_nn_dump;   // bot_ai_nn_dump: per decision int32 tick, int32 action, float obs[n_obs], uint8 mask[NNA_N] (binary dump for offline comparison)
static void bai_nn_load(void) {
    if (g_bai_nn_tried) return; g_bai_nn_tried = 1; char v[260] = ""; wchar_t src[MAX_PATH];
    if (!setting_get("bot_ai_nn", L"", 0, v, sizeof v, src) || !v[0]) { logf_("bot: nn: no bot_ai_nn in the config - policy nn bots stay idle"); return; }
    g_bai_nn_midfeint = (int)bai_cfg_num("bot_ai_nn_mid_feint", 0) != 0 && g_bai_lca_ok; logf_("bot: nn: mid-attack feints %s", g_bai_nn_midfeint ? "offered" : "off");
    char dp[260] = ""; if (setting_get("bot_ai_nn_dump", L"", 0, dp, sizeof dp, src) && dp[0]) { g_bai_nn_dump = fopen(dp, "wb"); logf_("bot: nn: observation dump %s %s", dp, g_bai_nn_dump ? "open" : "NOT open"); }
    char why[300] = ""; if (nn_load(&g_bai_nn, v, why, sizeof why)) logf_("bot: nn: loaded %s (obs %d, hidden %d, actions %d)", v, g_bai_nn.n_obs, g_bai_nn.n_hid, g_bai_nn.n_act); else logf_("bot: nn: NOT loaded: %s", why);
}
static void bai_end_combo(BaiBot *b) {   // UComboManager::EndCombo @ 0x2f12c0 (a player's APCancel::OnDeactivation does this; the NPC path does not)
    b->ch_e = -1; __try { uint8_t *ac = se_comp(BAI, b->ch, "m_AttackComponent"); void **cmp = ac ? api_p_prop_ptr(BAI, ac, "m_ComboManager") : NULL;
        if (cmp && se_valid_ptr(*cmp)) ((void (*)(void *))(g_base + se_rva_sym("UComboManager::EndCombo", 0x2F12C0u)))(*cmp); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    static int nlog; if (nlog++ < 20) logf_("bot: %s: POST-FEINT combo ended", b->id); }
static int bai_nn_is_press(int a) { return a == NNA_ATK1 || a == NNA_ATK2 || a == NNA_ATK1_FEINT || a == NNA_ATK2_FEINT || (a >= NNA_DODGE_BACK && a <= NNA_STANCE_BL) || a == NNA_ABILITY || a == NNA_POWER0 || a == NNA_POWER1 || a == NNA_DRAW || a == NNA_LOCK_TOGGLE || (a >= NNA_ABILITY_V1 && a <= NNA_ABILITY_V3); }
// legal actions (the mask the policy was trained with): FEINT only with bot_ai_nn_mid_feint during an own attack (otherwise feints are decided at launch)
static void bai_nn_mask(BaiBot *b, double now, unsigned char *m) {
    const BotSide *me = &b->o.me; int atk = me->s.v[BO_ATTACKING] > 0, st = bo_stance(me), dodge = bai_avail(b->ch, 5);
    for (int i = 0; i < NNA_N; i++) m[i] = 1;
    m[NNA_FEINT] = (unsigned char)(g_bai_nn_midfeint && atk); m[NNA_GUARD_ON] = g_bai_nn_midfeint || !(atk && !me->intr_open);   // mid-feint variant of the mask
    for (int d = 0; d < 4; d++) m[NNA_DODGE_BACK + d] = m[NNA_DODGE_FL + d] = (unsigned char)dodge;
    { uint8_t *mc = (uint8_t *)bai_movecomp(b->ch); float cd = 0; __try { if (mc) cd = *(float *)(mc + 0xB20); } __except (EXCEPTION_EXECUTE_HANDLER) { }
      m[NNA_WALK_SLOW] = (unsigned char)(now - b->tog_t >= 0.33);   /* toggle cooldown 10 F */
      m[NNA_RUSH] = (unsigned char)(now - b->tog_t >= 0.33) && (unsigned char)(b->rush || (b->st.unlocked && cd <= 0.f && b->o.me.s.v[BO_GUARD] > 0.f)); }
    for (int q = 0; q < 4; q++) m[NNA_STANCE_FL + q] = q != st && (!atk || me->intr_open);
    m[NNA_ABILITY] = m[NNA_ABILITY_V1] = m[NNA_ABILITY_V2] = m[NNA_ABILITY_V3] = (unsigned char)me->ability_ok;
    for (int i = 0; i < 2; i++) m[NNA_POWER0 + i] = b->cfg.power_kind[i] && now >= b->pw_ready[i] && me->s.v[BO_SHARDS] + 1e-3f >= b->cfg.power_cost[i];
    m[NNA_DRAW] = b->cfg.wpn_cost > 0 && me->has_weapon;
}
static int bai_nn_try_attack(BaiBot *b, int input, int feint) {
    BotCmd c; memset(&c, 0, sizeof c); c.type = BC_ATTACK; c.input = input; c.feint = feint || b->feint_next; snprintf(c.tech, sizeof c.tech, "NN"); /* feint_next: the latch throws the next attack as an instant feint */ int perf = 0; char row[24] = "";
    int ok = bai_attack(b, &c, b->o.me.gold_open, &perf, row); if (ok) { b->attacks++; b->feint_next = 0; } return ok;
}
static void bai_nn_exec(BaiBot *b, int a, double now) {
    switch (a) {
    case NNA_ATK1: case NNA_ATK2: case NNA_ATK1_FEINT: case NNA_ATK2_FEINT: {
        int in = a == NNA_ATK2 || a == NNA_ATK2_FEINT, fe = a == NNA_ATK1_FEINT || a == NNA_ATK2_FEINT; bai_press(b);
        if (!bai_nn_try_attack(b, in, fe)) { b->nn_buf = 1 + in + 2 * fe; b->nn_buf_t = now; } else b->nn_buf = 0; break; }
    case NNA_FEINT: if (!bai_cancel(b) && bo_attacking(&b->o.me) && !b->o.me.intr_open) b->feint_next = 1; break;   // in the cancel window: cancel now; in the unfinishable part: the latch
    case NNA_GUARD_ON: if (g_bai_nn_midfeint && bo_attacking(&b->o.me) && !b->o.me.intr_open) { b->feint_next = 1; break; } bai_guard(b, 1); break;   // guard in the unfinishable part = the latch
    case NNA_GUARD_OFF: bai_guard(b, 0); break;
    case NNA_DODGE_BACK: case NNA_DODGE_LEFT: case NNA_DODGE_RIGHT: case NNA_DODGE_FWD: if (bai_dodge(b, a - NNA_DODGE_BACK)) b->dodge_t = now; break;   // network 0 back 1 left 2 right 3 forward = bai_dodge 0 away 1 / 2 side 3 toward
    case NNA_STANCE_FL: case NNA_STANCE_FR: case NNA_STANCE_BR: case NNA_STANCE_BL: bai_stance(b, a - NNA_STANCE_FL); break;
    case NNA_ABILITY: bai_ability(b, 0); break;
    case NNA_ABILITY_V1: case NNA_ABILITY_V2: case NNA_ABILITY_V3: bai_ability(b, a - NNA_ABILITY_V1 + 1); break;
    case NNA_MOVE_FWD: b->move_on = 1; b->move_dir = 3; b->move_until = now + 60; break;
    case NNA_MOVE_BACK: b->move_on = 1; b->move_dir = 0; b->move_until = now + 60; break;
    case NNA_MOVE_STOP: b->move_on = 0; break;
    case NNA_STRAFE: b->move_on = 1; b->move_dir = 1; b->move_until = now + 60; break;     // 2D: walk left of the line until another move / stop (side_dir +1)
    case NNA_STRAFE_R: b->move_on = 1; b->move_dir = 2; b->move_until = now + 60; break;   // 2D: walk right
    case NNA_MOVE_FL: case NNA_MOVE_FR: case NNA_MOVE_BL: case NNA_MOVE_BR: b->move_on = 1; b->move_dir = 4 + (a - NNA_MOVE_FL); b->move_until = now + 60; break;   // diagonals (bai_move dir 4..7)
    case NNA_WALK_SLOW: b->walk_slow = !b->walk_slow; b->tog_t = now; break;
    case NNA_RUSH: b->tog_t = now; b->rush = !b->rush && b->st.unlocked; if (!b->rush) bai_set_rushing(b, 0); break;   // toggle; bai_move applies it
    case NNA_DODGE_FL: case NNA_DODGE_FR: case NNA_DODGE_BL: case NNA_DODGE_BR: if (bai_dodge(b, 4 + (a - NNA_DODGE_FL))) b->dodge_t = now; break;
    case NNA_LOCK_TOGGLE: if (bai_target_of(b->ch) == b->op) { bai_set_target(b->ch, NULL); b->st.unlocked = 1; } else { bai_set_target(b->ch, b->op); b->st.unlocked = 0; } break;
    case NNA_POWER0: case NNA_POWER1: if (bai_power(b, a - NNA_POWER0, now)) b->pending_pw = 1; break;
    case NNA_DRAW: bai_draw_toggle(b->ch); break;
    default: break;
    }
}
// 2D pose: world x / y (uu) + yaw (radians, atan2 convention) from the root component (same read as inputrec's ir_pose)
static int bai_pose(void *ch, float *x, float *y, float *yaw) {
    float l[3]; if (!ch || !api_p_test_get_location(BAI, ch, l)) return 0;
    __try { void *root = *(void **)((uint8_t *)ch + OFF_ACTOR_ROOTCOMP); const float *r = (const float *)((uint8_t *)root + OFF_ROOT_C2W_POS - 16);
            float yw = atan2f(2.f * (r[3] * r[2] + r[0] * r[1]), 1.f - 2.f * (r[1] * r[1] + r[2] * r[2])); if (!isfinite(yw)) return 0; *x = l[0]; *y = l[1]; *yaw = yw; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static float bai_wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
// Locked on, a player's character turns toward its target (back stance: its back); the stock NPC tree does that for the pawn and is stopped,
// so the adapter turns the bot itself (300 deg/s).
static void bai_face(BaiBot *b, double now) {
    static double ft[BAI_MAX]; int i = (int)(b - g_bai); if (i < 0 || i >= BAI_MAX) return; double dt = now - ft[i]; ft[i] = now; if (dt <= 0 || dt > 0.2) return;
    if (b->st.unlocked || bo_attacking(&b->o.me) || b->o.me.stunned || bai_target_of(b->ch) != b->op) return;
    float mx, my, myaw, ox, oy, oyaw; if (!bai_pose(b->ch, &mx, &my, &myaw) || !bai_pose(b->op, &ox, &oy, &oyaw)) return;
    int q = bo_stance(&b->o.me); float want = atan2f(oy - my, ox - mx) + ((q == 2 || q == 3) ? 3.14159265f : 0.f);
    float err = bai_wrap_pi(want - myaw), step = (float)(300.0 * 3.14159265 / 180.0 * dt); if (fabsf(err) < 0.01f) return; if (err > step) err = step; if (err < -step) err = -step;
    static void *fn; if (!fn) fn = api_p_find_function(BAI, b->ch, "K2_SetActorRotation"); if (!fn) { static int nl3; if (nl3++ < 3) logf_("bot: K2_SetActorRotation not found - bots do not turn when locked"); return; }
    float rot[3] = { 0.f, (myaw + err) * 57.2957795f, 0.f }; uint8_t tp = 0, ret = 0; void *args[2] = { rot, &tp }; __try { api_p_call_params(BAI, b->ch, fn, args, &ret); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
// NnExtra.geo layout: botnn.h; edge = 0 (unknown)
static void bai_nn_geo(BaiBot *b, double now, float *g) {
    float mx, my, myaw, ox, oy, oyaw; memset(g, 0, 10 * sizeof *g); if (!bai_pose(b->ch, &mx, &my, &myaw) || !bai_pose(b->op, &ox, &oy, &oyaw)) return;
    { int qm = bo_stance(&b->o.me), qo = bo_stance(&b->o.op); if (qm == 2 || qm == 3) myaw += 3.14159265f; if (qo == 2 || qo == 3) oyaw += 3.14159265f; }   // a back stance faces AWAY from the target; the observation's facing = the fighting direction
    float dx = ox - mx, dy = oy - my, d = sqrtf(dx * dx + dy * dy); if (d < 1e-3f) d = 1e-3f; float ux = dx / d, uy = dy / d;
    float a = bai_wrap_pi(myaw - atan2f(dy, dx)), c = bai_wrap_pi(oyaw - atan2f(-dy, -dx));
    double dt = now - b->nn_pt; float vm[2] = { 0, 0 }, vo[2] = { 0, 0 };
    if (b->nn_pt > 0 && dt > 0.005 && dt < 0.2) { vm[0] = (float)((mx - b->nn_pp[0][0]) / dt); vm[1] = (float)((my - b->nn_pp[0][1]) / dt); vo[0] = (float)((ox - b->nn_pp[1][0]) / dt); vo[1] = (float)((oy - b->nn_pp[1][1]) / dt); }
    b->nn_pp[0][0] = mx; b->nn_pp[0][1] = my; b->nn_pp[1][0] = ox; b->nn_pp[1][1] = oy; b->nn_pt = now;
    float vx = vo[0] - vm[0], vy = vo[1] - vm[1];
    g[0] = sinf(a); g[1] = cosf(a); g[2] = sinf(c); g[3] = cosf(c); g[4] = -(vx * ux + vy * uy) / 300.f; g[5] = (vx * -uy + vy * ux) / 300.f; g[6] = sqrtf(vm[0] * vm[0] + vm[1] * vm[1]) / 400.f; g[7] = 0.f; g[8] = (float)(b->walk_slow != 0); g[9] = (float)(b->rush != 0);   /* obs v5 geometry */
}
static void bai_nn_step(BaiBot *b, double now) {
    if (!g_bai_nn.ok) return;
    if (b->nn_buf && now - b->nn_buf_t <= 0.35) { int in = (b->nn_buf - 1) & 1, fe = (b->nn_buf - 1) >> 1; if (bai_nn_try_attack(b, in, fe)) b->nn_buf = 0; } else b->nn_buf = 0;   // press buffer
    if (now - b->nn_t < 1.0 / 30.0) return;
    b->nn_t = now; b->nn_tick++;
    BaiPair *P = b->pair >= 0 && b->pair < BAI_MAX / 2 ? &g_bai_pairs[b->pair] : NULL; int rnd = P ? P->round : 0, newr = rnd != b->nn_round || b->nn_tick == 1;
    if (newr) {                                            // a new round: reaction delay for this round, history = the current opponent
        b->nn_round = rnd; double g = 0; for (int k = 0; k < 12; k++) g += bc_rand(&b->st.rng); g -= 6.0;   // ~normal(0, 1)
        double ms = 100.0 + 25.0 * g; if (ms < 83.0) ms = 83.0; b->nn_delay = (int)(ms / 1000.0 * 30.0 + 0.5); if (b->nn_delay > 15) b->nn_delay = 15;
        for (int k = 0; k < 16; k++) b->nn_hist[k] = b->o.op; b->nn_hi = 0;
    }
    NnExtra xm, xo; memset(&xm, 0, sizeof xm); memset(&xo, 0, sizeof xo);
    bai_nn_geo(b, now, xm.geo);   // obs v5: 2D geometry (own side); geo[0..5] seen with the reaction delay like the opponent
    if (newr) for (int k = 0; k < 16; k++) memcpy(b->nn_ghist[k], xm.geo, sizeof b->nn_ghist[k]);
    b->nn_hi = (b->nn_hi + 1) & 15; b->nn_hist[b->nn_hi] = b->o.op; memcpy(b->nn_ghist[b->nn_hi], xm.geo, sizeof b->nn_ghist[0]);
    memcpy(xm.geo, b->nn_ghist[(b->nn_hi + 16 - b->nn_delay) & 15], sizeof b->nn_ghist[0]);
    BotSide op = b->nn_hist[(b->nn_hi + 16 - b->nn_delay) & 15]; bo_perceive(&op, b->cfg.reveal_f > 0 ? b->cfg.reveal_f : 2);
    for (int i = 0; i < 2; i++) { xm.power_kind[i] = b->cfg.power_kind[i]; xm.power_cost[i] = b->cfg.power_cost[i]; xm.power_cd_left[i] = b->pw_ready[i] > now ? (float)(b->pw_ready[i] - now) : 0.f; }
    xm.wpn_cost = b->cfg.wpn_cost;
    if (P) { int me0 = P->a == b->ch; xm.rounds_won = (float)P->mw[me0 ? 0 : 1]; xo.rounds_won = (float)P->mw[me0 ? 1 : 0]; }
    float obs[NN_OBS], lg[64]; unsigned char m[NNA_N];
    BotSide me = b->o.me; if (b->cfg.wpn_cost <= 0) { me.has_weapon = 0; me.weapon_out = 0; me.wdur = 0; }   // a bare-hands bot: the archetype's unused weapon item is not part of its kit
    float rt = (float)(P ? now - P->round_t : 0); if (g_trs.on && g_trs.bot == b->ch && g_trs.phase == TP_FIGHT) rt = (float)tr_since();   // sparring (bot trial): the duel's own round start
    nn_obs(&me, &op, b->o.dist, &xm, &xo, rt, obs);
    bai_nn_mask(b, now, m);
    if (b->cx_seq && b->cx_seq != b->cx_land && !bo_attacking(&b->o.me)) { b->cx_land = b->cx_seq; b->cx_end = now; b->cx_open = 1; }   // the feinted attack is gone: the cancel runs
    if (b->cx_open && now - b->cx_end <= 0.1) for (int i = 0; i < NNA_N; i++) if (i != NNA_WAIT) m[i] = 0;   // GenericNone for 0.1 s (CancelDB m_iFrameInterruptibleRelease 3 F)
    nn_forward(&g_bai_nn, obs, lg); int a = nn_sample(lg, m, NNA_N, &b->st.rng);
    if (a < 0) a = NNA_WAIT;
    if (bai_nn_is_press(a)) { if (b->nn_tick - b->nn_last_press < 2 && b->nn_last_press > 0) a = NNA_WAIT; else b->nn_last_press = b->nn_tick; }
    if (a >= 0 && a < NNA_N) b->nn_acts[a]++;
    if (g_bai_nn_dump) { int hd[2] = { (int)b->nn_tick, a }; fwrite(hd, 4, 2, g_bai_nn_dump); fwrite(obs, 4, NN_OBS, g_bai_nn_dump); fwrite(m, 1, NNA_N, g_bai_nn_dump); if (b->nn_tick % 300 == 0) fflush(g_bai_nn_dump); }
    if (b->cx_open) { int atk = a == NNA_ATK1 || a == NNA_ATK2 || a == NNA_ATK1_FEINT || a == NNA_ATK2_FEINT || (b->cfg.ability_kind == BA_DRUNKEN && (a == NNA_ABILITY || (a >= NNA_ABILITY_V1 && a <= NNA_ABILITY_V3)));
        if (atk) b->cx_open = 0; else if (bai_nn_is_press(a) || now - b->cx_end > 1.0) { b->cx_open = 0; bai_end_combo(b); } }   // EndCombo unless an attack follows inside the 1.0 s cancel
    if (a != NNA_WAIT) { bai_nn_exec(b, a, now); b->last_cmd_t = now; b->last_type = BC_ATTACK; snprintf(b->last_ctech, sizeof b->last_ctech, "NN%d", a); }
    if (b->nn_tick % 900 == 0) { char h[400]; int k = 0; for (int i = 0; i < NNA_N && k < 380; i++) if (b->nn_acts[i]) k += snprintf(h + k, sizeof h - k, " %d:%ld", i, b->nn_acts[i]);
        logf_("bot: nn %s: %ld decisions, delay %d ticks, actions%s", b->id, b->nn_tick, b->nn_delay, h); }
}
// ----- one command
static void bai_exec(BaiBot *b, const BotCmd *c, double now) {
    int ok = 0, tech = bot_tech_id(c->tech); char row[24] = "", extra[96] = "";
    switch (c->type) {
    case BC_ATTACK:
        bai_press(b);   /* anti-spam count (one command = one press) */
        if (c->gold) { b->hold = *c; b->holding = 1; b->hold_press = -1; return; }       // gold / link press: resolved by bai_hold on the next ticks
        { int perf = 0; unsigned before = b->seq; ok = bai_attack(b, c, 0, &perf, row);
          if (!ok) { b->refused++; if (b->refused % 25 == 1) { AbsCharInfo di; memset(&di, 0, sizeof di); di.size = sizeof di; api_p_char_info(BAI, b->ch, &di); void *tg = bai_target_of(b->ch);
                logf_("bot: refused %s attack #%ld: guarding %d attacking(char %d, bot %.0f) stance %d down %d | avail Attack1 %d Attack2 %d Perfect %d Guard %d Dodge %d | target %s, order Attack running %d, tree running %d",
                      b->id, b->refused, di.guarding, di.attacking, b->o.me.s.v[BO_ATTACKING], di.stance, di.is_down, bai_avail(b->ch, 0), bai_avail(b->ch, 1), bai_avail(b->ch, 19), bai_avail(b->ch, 2), bai_avail(b->ch, 5),
                      tg == b->op ? "opponent" : tg ? "OTHER" : "NONE", bai_order_running(b->ch, 1, 2), bai_bt_running(b->ch)); } }
          if (!ok && c->secs < 3) {                                                          // an attack is refused while the guard is still up (a follow-up right after
              AbsCharInfo gi; memset(&gi, 0, sizeof gi); gi.size = sizeof gi; api_p_char_info(BAI, b->ch, &gi);   // the unguard, neutral starters while blocking): drop the guard
              if (gi.guarding > 0 || b->st.guard_up) { api_p_test_guard(BAI, b->ch, 0); b->st.guard_up = 0; }      // and retry up to 3 times, 0.1 s apart
              BotCmd r2 = *c; r2.secs += 1; bc_push_at(&b->q, r2, now + 0.1); if (g_bai_verbose > 1) logf_("bot: %s attack retried (%d, guarding %d)", b->id, (int)r2.secs, gi.guarding); return; } snprintf(extra, sizeof extra, "row %s input %d%s", row[0] ? row : "-", c->input, c->feint ? " FEINT" : ""); if (ok && c->feint) { b->feint_state = 1; b->feint_base = before; } }
        break;
    case BC_GUARD: ok = bai_guard(b, 1); if (!ok && b->gd_want) ok = 1; break;   /* held until available (bai_guard) */
    case BC_UNGUARD: ok = bai_guard(b, 0); break;
    case BC_ABILITY: ok = bai_ability(b, c->dir); if (ok) { b->pending_ab = 1; b->ab_t = now; b->ab_whiff_done = 0; } break;
    case BC_DODGE: ok = bai_dodge(b, c->dir); snprintf(extra, sizeof extra, "dir %d", c->dir); if (ok) { b->pending_dg = 1; b->dodge_t = now; } break;
    case BC_STANCE: ok = bai_stance(b, c->quad); snprintf(extra, sizeof extra, "to %d", c->quad); if (ok) { b->stance_t = now; b->stance_want = c->quad; } break;
    case BC_POWER: ok = bai_power(b, c->slot, now); snprintf(extra, sizeof extra, "slot %d", c->slot); if (ok) b->pending_pw = 1; break;
    case BC_DRAW: ok = bai_draw_toggle(b->ch) > 0; break;
    case BC_LOCK: ok = bai_set_target(b->ch, b->op); break;
    case BC_UNLOCK: ok = bai_set_target(b->ch, NULL); break;
    case BC_MOVE: b->move_on = 1; b->move_dir = c->dir; b->move_until = now + c->secs; ok = 1; break;
    default: break;
    }
    if (c->type == BC_GUARD && ok) b->pending_gd = 1;
    if (ok && (c->type == BC_GUARD || c->type == BC_ABILITY || c->type == BC_DODGE || c->type == BC_POWER)) b->pend_t = now;
    if (c->type != BC_MOVE || g_bai_verbose > 1)
        logf_("bot: act %s %s %s -> %d (seen %.3f, due %.3f, late %.0f ms, tech %s, pair %d)", b->id, BC_NAMES[c->type], extra, ok, c->seen, c->due, (now - c->due) * 1000.0, c->tech[0] ? c->tech : "-", b->pair);
        if (c->type == BC_ATTACK && !strcmp(c->tech, "PN")) { BotSide *os = &b->o.op; float end = os->r.ok ? (float)os->r.release : -1.f, iend = os->r.ok && os->r.irelease > 0 && os->r.irelease <= 120 ? (float)(os->r.startup + os->r.irelease) : -1.f;   // punish measurement: what the opponent had left when the punish started
            logf_("bot: PN launch %s: opponent %s phase %d frame %.0f, to move end %.0f F, to its interruptible release %.0f F, distance %.0f uu, opp stunned %d", b->id, os->row[0] ? os->row : "-", bo_phase(os), os->frame, end >= 0 ? end - os->frame : -1.f, iend >= 0 ? iend - os->frame : -1.f, b->o.dist, os->stunned); }
    if (ok && c->type == BC_LOCK) { void *t = bai_target_of(b->ch); logf_("bot: proof lock %s: target is %s", b->id, t == b->op ? "the opponent (PASS)" : "NOT the opponent (FAIL)"); }
    if (ok && c->type == BC_UNLOCK) { void *t = bai_target_of(b->ch); logf_("bot: proof unlock %s: target %s", b->id, t ? "still set (FAIL)" : "cleared (PASS)"); }
    if (tech >= 0 && !ok) logf_("bot: tech %s %s fail (not executed)", c->tech, b->id);
}
// held gold / link press: decide the press time, then resolve it with the buffer rules
static void bai_hold(BaiBot *b, double now) {
    if (!b->holding) return;
    BotSide *me = &b->o.me;
    if (b->hold_press < 0) {
        double learned = me->row[0] ? bai_win_get(b, me->row) : -1, pt = bc_gold_press_time(b->atk_t, learned, b->hold.offset_f);
        if (pt >= 0) b->hold_press = pt;
        else if (me->gold_open && b->gold_open_t > 0) b->hold_press = b->gold_open_t + (b->hold.offset_f > 0 ? b->hold.offset_f : 0) * BC_FRAME;   // window not learned yet: press when it is seen
        else if (!bo_attacking(me) && now - b->hold.due > 1.0) { b->holding = 0; logf_("bot: link %s: no own attack to link from - press dropped", b->id); return; }
        else return;
    }
    int r = bc_gold_resolve(b->hold_press, now, me->gold_open, me->intr_open);
    if (r == BC_LINK_WAIT) return;
    b->holding = 0;
    double off_f = b->gold_open_t > 0 ? (b->hold_press - b->gold_open_t) * 30.0 : 99;
    if (r == BC_LINK_LOST) { logf_("bot: link %s lost row - press %+.1f F vs the window (no window within the buffer)", b->id, off_f); bot_note(&b->st, bot_tech_id(b->hold.tech), 0); return; }
    int perf = 0; char row[24] = ""; BotCmd c = b->hold; c.gold = 0;
    int ok = bai_attack(b, &c, r == BC_LINK_GOLD, &perf, row);
    b->last_gold_cmd = perf; b->last_offset_f = (float)off_f;
    logf_("bot: link %s %s row %s press %+.1f F vs the window opening (planned %+.1f F, resolve %s, launched %d, tech %s)", b->id, perf ? "gold" : "plain", row[0] ? row : "-", off_f, b->hold.offset_f, r == BC_LINK_GOLD ? "GOLD" : "PLAIN", ok, b->hold.tech);
    if (ok && perf && !strcmp(b->hold.tech, "U3")) bot_note(&b->st, BT_U3, 1);
}

// ----- events
static void bai_on_attack(void *ch, int id, const char *attack, void *u) {
    (void)id; (void)u; if (!g_bai_on) return; double now = bai_now();
    BaiAtk *k = bai_atk_of(ch, 1); if (!k) return;
    k->seq++; snprintf(k->row, sizeof k->row, "%s", attack ? attack : ""); k->t = now;
    if (ch == cd_local_pawn()) {   // the player's attack target slots at each attack start
        BaiBot *bt = NULL; for (int i = 0; i < BAI_MAX && !bt; i++) if (g_bai[i].used && g_bai[i].op == ch) bt = &g_bai[i];
        uint8_t *ac = bt ? se_comp(BAI, ch, "m_AttackComponent") : NULL;
        if (ac) { static const int off[4] = { 0x434, 0x43C, 0x444, 0x458 }; static const char *const nm[4] = { "target", "preview", "next", "designated" }; char s[160] = ""; int n = 0;
            __try { for (int q = 0; q < 4; q++) { void *o = api_weak_get((WeakPtr *)(ac + off[q])); n += snprintf(s + n, sizeof s - n, " %s %s", nm[q], !o ? "none" : o == bt->ch ? "BOT" : "other"); } } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(s, sizeof s, " (read failed)"); }
            int hit = -1; __try { void **aic = api_p_prop_ptr(BAI, bt->ch, "m_AIComponent"); if (aic && tk_ptr_ok(*aic)) hit = ((uint8_t *)*aic)[0x140]; } __except (EXCEPTION_EXECUTE_HANDLER) { hit = -1; }
            logf_("bot: PLAYER-ATTACK row %s, distance %.0f, bot can be hit %d, slots:%s", k->row, bt->o.dist, hit, s); } }
    if (g_bai_verbose > 1 && !bai_of(ch)) logf_("bot: seen attack %s row %s (seq %u)", bai_who(ch), k->row, k->seq);
    BaiBot *b = bai_of(ch); if (b && k->seq <= 3) logf_("bot: %s attack order running modes 0/1/2: %d %d %d (at its start)", b->id, bai_order_running(ch, 1, 0), bai_order_running(ch, 1, 1), bai_order_running(ch, 1, 2));
    if (b) { uint8_t ci = 0xFF; __try { void *ac = se_comp(BAI, ch, "m_AttackComponent"); void **cmp = ac ? api_p_prop_ptr(BAI, ac, "m_ComboManager") : NULL; if (cmp && se_valid_ptr(*cmp)) ci = *((uint8_t *)*cmp + 0x1C0); } __except (EXCEPTION_EXECUTE_HANDLER) { ci = 0xFF; }
             if (b->feint_state) logf_("bot: combo %s row %s index %d (after a feint of row %s)", b->id, attack ? attack : "", ci, b->row); else if (g_bai_verbose > 1) logf_("bot: combo %s row %s index %d", b->id, attack ? attack : "", ci); }
    if (b) { b->seq = k->seq; b->atk_t = now; b->attacks++; snprintf(b->row, sizeof b->row, "%s", k->row); if (g_bai_verbose) { BotRow rr; bai_row(k->row, &rr); logf_("bot: attack %s row %s (seq %u) pair %d dist %.0f range %.0f", b->id, k->row, k->seq, b->pair, b->o.dist, rr.ok ? rr.range : -1.f); }
             if (b->def_t > 0) { logf_("bot: metric %s style_move_to_attack %.1f F", b->id, (now - b->def_t) * 30.0); b->def_t = 0; } }
    else if (g_bai_verbose == 1) for (int i = 0; i < BAI_MAX; i++) if (g_bai[i].used && g_bai[i].op == ch) { logf_("bot: seen attack %s row %s (seq %u)", bai_who(ch), k->row, k->seq); break; }
}
static void bai_on_hit(const AbsHitInfo *h, void *u) {
    (void)u; if (!g_bai_on || !h) return;
    BaiBot *a = bai_of(h->attacker), *v = bai_of(h->victim);
    if (a && g_bai_mode_bots && h->victim == cd_local_pawn()) logf_("bot: WARNING %s hit the player in bots mode", a->id);
    if (a) { logf_("bot: hit %s -> %s row %s outcome %d damage %.0f link %s (victim health before %.0f / %.0f, guard %.0f) pair %d", a->id, bai_who(h->victim), h->attack, h->outcome, h->damage, h->perfect_link ? "gold" : "plain", a->o.op.s.v[BO_HEALTH], a->o.op.s.v[BO_MAXHEALTH], a->o.op.s.v[BO_GUARD], a->pair);
             if (h->outcome == ABS_HIT || h->outcome == ABS_HIT_GUARDED || h->outcome == ABS_HIT_GUARD_BROKEN) { if (h->outcome != ABS_HIT_GUARDED) a->hits_landed++; bot_on_landed(&a->cfg, &a->st, h->outcome == ABS_HIT_GUARDED, bai_now()); } }
    if (v) { logf_("bot: hit %s <- %s row %s outcome %d damage %.0f", v->id, bai_who(h->attacker), h->attack, h->outcome, h->damage);
             if (h->outcome == ABS_HIT && v->dodge_t > 0 && bai_now() - v->dodge_t < 0.6) logf_("bot: metric %s hit_while_dodging %.2f s after the dodge", v->id, bai_now() - v->dodge_t); }
}
static void bai_on_defense(const AbsDefenseInfo *d, void *u) {
    (void)u; if (!g_bai_on || !d) return;
    { BaiBot *at = bai_of(d->attacker); if (at && (d->type == ABS_DEF_DODGE || d->type == ABS_DEF_AVOID)) at->gd_seq = at->o.me.seq; }   // an avoid / dodge success dismisses the attacker's gold link
    BaiBot *b = bai_of(d->defender); if (!b) return;
    static const char *const tn[] = { "guard", "guard_break", "absorb", "parry", "deflect", "dodge", "avoid", "?" };   // ABS_DEF_*
    int t = d->type >= 0 && d->type < 7 ? d->type : 7;
    if (t == ABS_DEF_PARRY || t == ABS_DEF_DEFLECT || t == ABS_DEF_AVOID || t == ABS_DEF_ABSORB) b->lk_ab_ok = 1;   // a successful style ability frees the re-press lock
    logf_("bot: def %s %s vs %s row %s frame %.1f (startup %d) pair %d", b->id, tn[t], bai_who(d->attacker), d->attack, d->frame_in_attack, d->startup, b->pair);
    if (b->cfg.policy == BP_TECH) {                                                     // ability follow-up module (botcore.h bot_followup): continue at once, planned with the move
        int k = t == ABS_DEF_GUARD ? BF_GUARD : t == ABS_DEF_PARRY || t == ABS_DEF_DEFLECT ? BF_PARRY : t == ABS_DEF_AVOID ? BF_AVOID : t == ABS_DEF_ABSORB ? BF_ABSORB : t == ABS_DEF_DODGE ? BF_DODGE : -1;
        if (k >= 0) { double now = bai_now(); if (k != BF_GUARD) b->def_t = now; int qd = bot_on_defended(&b->cfg, &b->st, k, now, &b->q);
            logf_("bot: followup %s after %s -> %s (%s)", b->id, tn[t], qd ? "queued" : "none", bot_followup(k, b->cfg.ruleset).note); }
    }
    if ((t == ABS_DEF_ABSORB || t == ABS_DEF_PARRY || t == ABS_DEF_DEFLECT || t == ABS_DEF_AVOID) && b->pending_ab) { b->pending_ab = 0; bot_note(&b->st, BT_X9, 1); logf_("bot: proof ability %s: %s (PASS)", b->id, tn[t]); }
    if (t == ABS_DEF_DODGE && b->pending_dg) { b->pending_dg = 0; bot_note(&b->st, BT_U15, 1); logf_("bot: proof dodge %s: dodged (PASS)", b->id); }
    if (t == ABS_DEF_GUARD && b->pending_gd) { b->pending_gd = 0; bot_note(&b->st, BT_GD, 1); logf_("bot: proof guard %s: guarded (PASS)", b->id); }
    if ((t == ABS_DEF_PARRY || t == ABS_DEF_AVOID || t == ABS_DEF_DODGE) && b->pending_pw) { b->pending_pw = 0; bot_note(&b->st, BT_U12, 1); logf_("bot: proof shockwave %s: %s (power in the window)", b->id, tn[t]); }
}
static void bai_on_power(void *ch, void *item, void *actor, int dir, void *u) {
    (void)actor; (void)u; BaiBot *b = bai_of(ch); if (!b) return; char nm[96] = "?"; if (item) se_name_of(item, nm, sizeof nm);
    logf_("bot: proof power %s: %s used (dir %d) (PASS)", b->id, nm, dir);
}
static void bai_on_destroyed(void *ch, int reason, void *u) {
    (void)reason; (void)u; if (ch == g_bai_npc_cache) g_bai_npc_cache = NULL; BaiAtk *k = bai_atk_of(ch, 0); if (k) memset(k, 0, sizeof *k);
    for (int i = 0; i < BAI_MAX; i++) { if (g_bai[i].used && g_bai[i].ch == ch) { logf_("bot: %s: pawn gone - brain released", g_bai[i].id); g_bai[i].used = 0; } else if (g_bai[i].used && g_bai[i].op == ch) g_bai[i].op = NULL; }
}

// ----- presets (bots entries with a "brain" object)
static BaiPreset *bai_preset(const char *id) { for (int i = 0; i < g_bai_npre; i++) if (!strcmp(g_bai_pre[i].id, id)) return &g_bai_pre[i]; return NULL; }
static void bai_entry(const char *mod, const char *id, const char *json) {
    JDoc d; if (jdoc_parse(&d, json) != 0) { jdoc_free(&d); return; } int br = jfind(&d, 0, "brain"); jdoc_free(&d);
    if (br < 0) return;
    BaiPreset *p = bai_preset(id); if (!p && g_bai_npre < BAI_PRESETS) p = &g_bai_pre[g_bai_npre++]; if (!p) { logf_("bot: preset table full - %s ignored", id); return; }
    free(p->json); p->json = _strdup(json); snprintf(p->id, sizeof p->id, "%s", id); snprintf(p->mod, sizeof p->mod, "%s", mod);
    logf_("bot: preset %s/%s has a brain (%s)", mod, id, g_bai_on ? "will be driven when spawned" : "bot_ai is off: stays a stock AI");
}
static int bai_load_cfg(BaiBot *b, const BaiPreset *p) {
    BotCfg *c = &b->cfg; bot_cfg_init(c); JDoc d; char why[200]; int bad = 0;
    if (jdoc_parse(&d, p->json) != 0) { jdoc_free(&d); return 0; }
    int br = jfind(&d, 0, "brain"); if (br < 0 || d.n[br].t != J_OBJ) { jdoc_free(&d); return 0; }
    int df = jfind(&d, br, "difficulty"); if (df >= 0 && d.n[df].str) bot_cfg_set(c, "difficulty", d.n[df].str);     // difficulty first: its defaults, then the explicit knobs
    for (int k = d.n[br].first; k >= 0; k = d.n[k].next) {
        const char *key = d.n[k].key; if (!key || !strcmp(key, "difficulty")) continue;
        if (!strcmp(key, "ally")) { b->ally = d.n[k].str && (!_stricmp(d.n[k].str, "true") || !strcmp(d.n[k].str, "1")); continue; }   // ally mode (bai_ally_op)
        if (!strcmp(key, "weapon")) { b->want_sword = d.n[k].str && !strcmp(d.n[k].str, "sword"); continue; }      // weapons layer: this brain draws / sheathes a sword (bai_weapon_policy)
        if (d.n[k].t == J_OBJ && (!strcmp(key, "techs") || !strcmp(key, "skill") || !strcmp(key, "knobs"))) {
            for (int t = d.n[k].first; t >= 0; t = d.n[t].next) { char kk[64]; snprintf(kk, sizeof kk, "%s%s", !strcmp(key, "techs") ? "tech." : "", d.n[t].key ? d.n[t].key : "");
                if (!d.n[t].str || !bot_cfg_set(c, kk, d.n[t].str)) { if (!strcmp(key, "techs") && d.n[t].str && !strcmp(d.n[t].str, "avoid")) { char k2[64]; snprintf(k2, sizeof k2, "tech.%s", d.n[t].key); bot_cfg_set(c, k2, "off"); continue; }
                    logf_("bot: %s: brain.%s.%s = %s not understood", p->id, key, d.n[t].key ? d.n[t].key : "?", d.n[t].str ? d.n[t].str : "(object)"); bad++; } }
            continue;
        }
        if (d.n[k].t == J_ARR && !strcmp(key, "proof")) { char lst[200] = ""; for (int t = d.n[k].first; t >= 0; t = d.n[t].next) if (d.n[t].str) { strncat(lst, d.n[t].str, sizeof lst - strlen(lst) - 2); strcat(lst, ","); } bot_cfg_set(c, "proof", lst); continue; }
        if (!d.n[k].str) continue;
        if (!bot_cfg_set(c, key, d.n[k].str) && strcmp(key, "label") && strcmp(key, "note") && strcmp(key, "powers")) { logf_("bot: %s: brain.%s = %s not understood", p->id, key, d.n[k].str); bad++; }
    }
    // the brain's rows per ENGINE stance must match what the game plays: bots.inc writes a "mirror" entry (default) FR row -> FrontLeft field, FL -> FrontRight, BR -> BackLeft, BL -> BackRight
    // (save-deck convention); "mirror": false (decks in engine stances) writes FR -> FrontRight. bot_decks order FR FL BR BL -> EQuadrantTypes FL 0 FR 1 BR 2 BL 3.
    static const int map_plain[4] = { 1, 0, 2, 3 }, map_mirror[4] = { 0, 1, 3, 2 }; const int *map = (cs_str(&d, 0, "mirror") ? cs_true(&d, 0, "mirror") : 1) ? map_mirror : map_plain;
    int dk = jfind(&d, 0, "deck"); if (dk >= 0) { BtDeck k; char err[200] = "";
        if (bt_read_deck(&d, dk, &k, 0, err, sizeof err)) for (int q = 0; q < 4; q++) { int e = map[q]; c->deck_n[e] = k.n[q] < BOT_DECK_MAX ? k.n[q] : BOT_DECK_MAX; for (int i = 0; i < c->deck_n[e]; i++) snprintf(c->deck[e][i], 16, "%s", k.rows[q][i]); snprintf(c->alt[e], 16, "%s", k.alts[q]); } }
    { memcpy(b->bdeck, c->deck, sizeof b->bdeck); memcpy(b->bdeck_n, c->deck_n, sizeof b->bdeck_n); memcpy(b->balt, c->alt, sizeof b->balt);   // the bare-hands rows the brain started with
      int sk = jfind(&d, 0, "deck_sword"); if (b->want_sword && sk >= 0) { BtDeck k; char err[200] = "";   /* same stance map as the bare-hands deck (mirror flag) */
        if (bt_read_deck(&d, sk, &k, 1, err, sizeof err)) for (int q = 0; q < 4; q++) { int e = map[q]; b->sdeck_n[e] = k.n[q] < BOT_DECK_MAX ? k.n[q] : BOT_DECK_MAX; for (int i = 0; i < b->sdeck_n[e]; i++) snprintf(b->sdeck[e][i], 16, "%s", k.rows[q][i]); snprintf(b->salt[e], 16, "%s", k.alts[q]); }
        else { logf_("bot: %s: deck_sword unreadable (%s) - no weapon policy", b->id, err); b->want_sword = 0; } }
      else if (b->want_sword) { logf_("bot: %s: brain.weapon = sword but the preset has no deck_sword - no weapon policy", b->id); b->want_sword = 0; } }
    jdoc_free(&d);
    if (!bot_cfg_check(c, why, sizeof why)) { logf_("bot: %s: preset refused: %s - the bot stays a stock AI", p->id, why); return 0; }
    if (bad) logf_("bot: %s: %d brain key(s) ignored", p->id, bad);
    return 1;
}
// pairing by PLACE, not by adoption order (one failed or late adoption would mis-pair every later pair): a bot joins the nearest bot that is still alone within BAI_PAIR_UU
// (a pair spawns 300-800 uu apart; sequential mode spawns 160 uu apart), else it opens a new pair slot.
#define BAI_PAIR_UU 1100.f
static int bai_pair_for(BaiBot *self) {
    float p[3]; int cnt[BAI_MAX / 2]; memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < BAI_MAX; i++) if (g_bai[i].used && &g_bai[i] != self && g_bai[i].pair >= 0 && g_bai[i].pair < BAI_MAX / 2) cnt[g_bai[i].pair]++;
    if (api_p_test_get_location(BAI, self->ch, p)) { int best = -1; float bd = BAI_PAIR_UU;
        for (int i = 0; i < BAI_MAX; i++) { BaiBot *o = &g_bai[i]; if (!o->used || o == self || cnt[o->pair] != 1) continue; float q[3]; if (!api_p_test_get_location(BAI, o->ch, q)) continue;
            float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1])); if (d < bd) { bd = d; best = o->pair; } }
        if (best >= 0) return best; }
    for (int k = 0; k < BAI_MAX / 2; k++) if (cnt[k] == 0) return k;
    return 0;
}
// ally mode: the opponent is the nearest living enemy character within 1000 uu (3D) of the PLAYER (kept while within 1500 uu) (not the player, not another ally brain, not the practice dummy);
// none = no opponent (the bot waits). Rescanned every 0.5 s over UWorld::PawnList (a few dozen pawns),
// never for_each_object (a full object scan is a 430-560 ms frame).
static int bai_is_ally_bot(void *ch);
static void *bai_ally_op(BaiBot *b, double now) {
    { AbsCharInfo me; memset(&me, 0, sizeof me); me.size = sizeof me; if (!BAI->char_info(BAI, b->ch, &me) || me.is_dead || me.health <= 0) return b->ally_op = NULL; }   // a dead ally (2.5 s until removal) has no opponent, so it stops pulling enemies onto itself
    if (b->ally_op && !tk_ptr_ok(b->ally_op)) b->ally_op = NULL;
    if (b->ally_op) { AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; if (!BAI->char_info(BAI, b->ally_op, &ci) || ci.is_dead || ci.health <= 0) b->ally_op = NULL; }
    if (now - b->ally_t < 0.5) return b->ally_op; b->ally_t = now;
    void *pl = cd_local_pawn(), *w = world(), *best = NULL; float at[3], bd = 1000.f * 1000.f; int n = 0, seen = 0;   // pick radius 1000 uu (1500 pulls in far idle world NPCs)
    if (!pl || !w || !api_p_test_get_location(BAI, pl, at)) return b->ally_op = NULL;
    if (b->ally_op) { float l[3]; if (api_p_test_get_location(BAI, b->ally_op, l)) { float dx = l[0] - at[0], dy = l[1] - at[1], dz = l[2] - at[2]; if (dx * dx + dy * dy + dz * dz < 1500.f * 1500.f) return b->ally_op; } }   // sticky: keep a living opponent within 1500 uu of the player (nearest-to-player alone would flip it mid-fight)
    __try { WeakPtr *list = *(WeakPtr **)((uint8_t *)w + OFF_W_PAWNLIST); n = *(int *)((uint8_t *)w + OFF_W_PAWNLIST + 8);
        if (!list || n < 0 || n > 512) { static int warned; if (!warned++) logf_("bot: %s: WARNING implausible PawnList (%p, %d) - no ally opponent", b->id, (void *)list, n); n = 0; }
        for (int i = 0; i < n; i++) { void *o = api_weak_get(&list[i]); if (!o || o == b->ch || o == pl || !BAI->is_a(BAI, o, "FightingCharacter") || bai_is_ally_bot(o)) continue;
            char nm[100] = ""; BAI->object_name(BAI, o, nm, sizeof nm); if (strstr(nm, "Training")) continue;
            AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; if (!BAI->char_info(BAI, o, &ci) || ci.is_player || ci.is_dead || ci.health <= 0) continue;
            { void *ot = bai_target_of(o); if (ot != pl && ot != b->ch) continue; }   // only enemies already going for the player (or this ally): an idle world NPC pulled in would turn on the player once the ally fell
            float l[3]; if (!api_p_test_get_location(BAI, o, l)) continue; seen++;
            float dx = l[0] - at[0], dy = l[1] - at[1], dz = l[2] - at[2], d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; best = o; } } }
    __except (EXCEPTION_EXECUTE_HANDLER) { static int fl; if (!fl++) logf_("bot: %s: WARNING PawnList walk faulted - no ally opponent", b->id); best = NULL; }
    if (best != b->ally_op) logf_("bot: %s ally scan: %d pawn(s) listed, %d candidate(s), nearest %p at %.0f uu of the player", b->id, n, seen, best, best ? sqrtf(bd) : -1.f);
    return b->ally_op = best; }
static void bai_adopt(void *ch, const char *mod, const char *id) {
    (void)mod; if (!g_bai_on || bai_of(ch)) return; BaiPreset *p = bai_preset(id); if (!p) return;
    BaiBot *b = NULL; for (int i = 0; i < BAI_MAX; i++) if (!g_bai[i].used) { b = &g_bai[i]; break; }
    if (!b) { logf_("bot: %s: %d brains already run - this one stays a stock AI", id, BAI_MAX); return; }
    memset(b, 0, sizeof *b); b->ch = ch; snprintf(b->id, sizeof b->id, "%s", id);
    if (!bai_load_cfg(b, p)) return;
    int ab = bai_ability_action(ch); b->cfg.ability_kind = ab == 7 ? BA_PARRY : ab == 8 ? BA_AVOID : ab == 9 ? BA_ABSORB : ab == 74 || ab == 75 ? BA_DRUNKEN : BA_NONE;
    if (b->cfg.ability_kind == BA_DRUNKEN) b->cfg.drunken_driven = 1;                                       // the adapter launches the Drunken rows: X9 may press them
    if (b->cfg.seed == 12345) b->cfg.seed = 12345 + (uint64_t)(b - g_bai) * 7919;
    bot_state_init(&b->st, &b->cfg); b->pair = -1; b->used = 1; b->pair = bai_pair_for(b); g_bai_adopt_n++;
    if (g_bai_start_shards >= 0) bai_set_shards(ch, (float)g_bai_start_shards);
    { void *hc = se_comp(BAI, ch, "m_HealthComponent"); int was = -1, now_ = -1; if (hc) { api_p_get_bool(BAI, hc, "m_bCanRecoverHealth", &was); api_p_set_bool(BAI, hc, "m_bCanRecoverHealth", 0); api_p_get_bool(BAI, hc, "m_bCanRecoverHealth", &now_); }   // with the behaviour tree stopped the pawn stays in danger state 0 = 30 HP/s regen DURING combat (m_RecoveryRateByDangerState[0]); no regen in a duel
      logf_("bot: %s: health regeneration off (m_bCanRecoverHealth %d -> %d; the stopped behaviour tree left the pawn in danger state 0 = 30 HP/s)", id, was, now_); }
    { void *ac = se_comp(BAI, ch, "m_AttackComponent"); float *rr = ac ? (float *)api_p_prop_ptr(BAI, ac, "m_fShardsRecoveryRate") : NULL; float was = rr && tk_ptr_ok(rr) ? *rr : -1.f; if (rr && tk_ptr_ok(rr)) *rr = 0.f;   /* NPCs refill shards over time (AI RefillShardsOvertime), players do not: off */
      logf_("bot: %s: passive shard refill off (m_fShardsRecoveryRate %.3f -> 0)", id, was); }
    int ai = api_p_test_ai(BAI, ch, 0), hit = api_p_test_hittable(BAI, ch, 1);
    logf_("bot: ADOPT %s (%p): ally %d, policy %d, ruleset %s, difficulty %s, react %.0f+-%.0f ms (floor %.0f), exec error %.1f F, gold miss %.2f, aggression %.2f, ability %d (action %d), power slot %d; stock behaviour tree stopped -> %d, hittable -> %d",
          id, ch, b->ally, b->cfg.policy, BR_NAMES[b->cfg.ruleset], BD_NAMES[b->cfg.difficulty], b->cfg.skill.react_mean_ms, b->cfg.skill.react_sd_ms, b->cfg.skill.react_floor_ms, b->cfg.skill.exec_err_f, b->cfg.skill.gold_miss, b->cfg.aggression, b->cfg.ability_kind, ab, b->cfg.power_slot, ai, hit);
    { void *pl = cd_local_pawn(); char a1[24] = "?", a2[24] = "?", b1[24] = "?", b2[24] = "?";   // unlocked-attack targeting needs matching replication flags
      BAI->get_property_text(BAI, ch, "bReplicates", a1, sizeof a1); BAI->get_property_text(BAI, ch, "m_bSyncFromServer", a2, sizeof a2);
      if (pl) { BAI->get_property_text(BAI, pl, "bReplicates", b1, sizeof b1); BAI->get_property_text(BAI, pl, "m_bSyncFromServer", b2, sizeof b2); }
      logf_("bot: %s targeting flags: bot bReplicates %s m_bSyncFromServer %s | player bReplicates %s m_bSyncFromServer %s", id, a1, a2, b1, b2); }
    char tl[160] = ""; for (int i = 0; i < BT_N; i++) if (b->cfg.tech[i]) { strcat(tl, BT_IDS[i]); strcat(tl, " "); } logf_("bot: %s techs on: %s", id, tl);
}
static void bai_spawned(void *ch, const char *mod, const char *id) { if (g_bai_on && api_p_is_game_thread(BAI)) bai_adopt(ch, mod, id); }

// ----- match (bot vs bot / bot vs player): a round is lost below 25 % health
static void bai_report(const char *why) {
    logf_("bot: gold gate (%s): tried %ld, refused after a 2nd press %ld, refused after an avoid / dodge %ld", why, g_gold_try, g_gold_no_spam, g_gold_no_dism);
    for (int i = 0; i < BAI_MAX; i++) { BaiBot *b = &g_bai[i]; if (!b->used) continue; char t[300] = ""; for (int k = 0; k < BT_N; k++) { char x[32]; snprintf(x, sizeof x, "%s %d/%d ", BT_IDS[k], b->st.oks[k], b->st.tries[k]); strcat(t, x); }
        logf_("bot: techs %s (%s) [%s] wins %d | ok/try %s| queue dropped %lld", b->id, why, BR_NAMES[b->cfg.ruleset], b->wins, t, b->q.dropped);
        logf_("bot: metric %s range %ld/%ld ticks (%.0f %% within %.0f uu), attacks %ld, hits landed %ld", b->id, b->ticks_range, b->ticks, b->ticks ? 100.0 * b->ticks_range / b->ticks : 0.0, b->cfg.range_uu + 30, b->attacks, b->hits_landed); }
}
static void bai_match(double now) {
    if (g_bai_match_rounds <= 0 || !g_bai_mode_bots) return; BaiBot *x = NULL, *y = NULL;      // matches only in bots mode
    for (int i = 0; i < BAI_MAX; i++) if (g_bai[i].used) { if (!x) x = &g_bai[i]; else if (!y) y = &g_bai[i]; }
    if (!g_bai_vs_npc[0]) {                                                                   // several pairs at once: one match per pair
        for (int pi = 0; pi < BAI_MAX / 2; pi++) { BaiBot *a = NULL, *c = NULL; for (int i = 0; i < BAI_MAX; i++) if (g_bai[i].used && g_bai[i].pair == pi) { if (!a) a = &g_bai[i]; else if (!c) c = &g_bai[i]; }
            BaiPair *P = &g_bai_pairs[pi]; if (!a || !c) continue;
            if (a->frozen || c->frozen) continue;                                                 // settling between rounds: no round-end check on cached health
            if (a->ch != P->a || c->ch != P->b) { memset(P, 0, sizeof *P); P->a = a->ch; P->b = c->ch; }
            if (P->round >= g_bai_match_rounds) continue;
            if (P->round_t == 0) { P->round_t = now; P->sr[0] = P->sr[1] = 1.f; P->sp_ok = api_p_test_get_location(BAI, a->ch, P->sp[0]) && api_p_test_get_location(BAI, c->ch, P->sp[1]); logf_("bot: match start %s vs %s, %d rounds (pair %d)", a->id, c->id, g_bai_match_rounds, pi); }
            float ha = bo_hp_frac(&a->o.me), hc = bo_hp_frac(&c->o.me); int to = now - P->round_t > g_bai_round_max; if (!(ha < 0.25f || hc < 0.25f || to)) continue;
            int w = ha < 0.25f && hc >= 0.25f ? 1 : hc < 0.25f && ha >= 0.25f ? 0 : -1;   /* no round timer in the game: a time-out (safety net, bot_ai_round_s) is a draw */
            P->round++; if (w >= 0) P->wins[w]++; else P->draws++;
            logf_("bot: match round %d winner %s (%s hp %.2f, %s hp %.2f, %.1f s, pair %d)%s", P->round, w == 0 ? a->id : w == 1 ? c->id : "draw", a->id, ha, c->id, hc, now - P->round_t, pi, to ? " timeout" : "");
            if (w >= 0) P->mw[w]++; int fresh = w < 0 || g_bai_match_to <= 0 || P->mw[0] >= g_bai_match_to || P->mw[1] >= g_bai_match_to;
            if (fresh) {                                                                          // a draw or a decided match: both reset and face off (a new match)
                if (w >= 0) P->mw[0] = P->mw[1] = 0;
                api_p_test_heal(BAI, a->ch); api_p_test_heal(BAI, c->ch); bai_freeze(a, now, 1.0); bai_freeze(c, now, 1.0); bai_face_off(a, c); P->round_t = now + 1.0; P->sr[0] = P->sr[1] = 1.f;
                if (w >= 0 && g_bai_start_shards >= 0) { bai_set_shards(a->ch, (float)g_bai_start_shards); bai_set_shards(c->ch, (float)g_bai_start_shards); }
            } else {                                                                              // the 1v1 round flow: only the loser respawns
                BaiBot *wb = w == 0 ? a : c, *lb = w == 0 ? c : a; float add = 150.f * bai_heal_coef(P->sr[w] - P->sr[1 - w]);
                bai_add_health(wb->ch, add); api_p_test_heal(BAI, lb->ch); bai_freeze(lb, now, 1.0); bai_respawn_far(lb->ch, wb->ch, P->sp, P->sp_ok); P->round_t = now;
                float wf = bo_hp_frac(&wb->o.me) + add / (wb->o.me.s.v[BO_MAXHEALTH] > 0 ? wb->o.me.s.v[BO_MAXHEALTH] : 1.f); P->sr[w] = wf > 1.f ? 1.f : wf; P->sr[1 - w] = 1.f;
                logf_("bot: round flow: %s keeps its health + %.0f heal-on-kill, %s respawns fresh", wb->id, add, lb->id);
            }
            for (int k = 0; k < 2; k++) { BaiBot *b = k ? c : a; if (!fresh && b == (w == 0 ? a : c)) continue;   /* the winner keeps fighting as it was */ bc_rng_seed(&b->st.rng, b->cfg.seed * 31u + (uint64_t)P->round * 1009u + (uint64_t)k * 7u);   /* deterministic reseed: runs can be replayed */ bc_clear(&b->q); b->holding = 0; api_p_test_guard(BAI, b->ch, 0); b->st.fsm = BS_NEUTRAL; b->st.guard_up = 0; }
            if (P->round >= g_bai_match_rounds) { a->wins = P->wins[0]; c->wins = P->wins[1]; a->done = c->done = 1; api_p_test_heal(BAI, a->ch); api_p_test_heal(BAI, c->ch); api_p_test_set_invincible(BAI, a->ch, 1); api_p_test_set_invincible(BAI, c->ch, 1); logf_("bot: match done %s %d - %s %d, draws %d (pair %d)", a->id, P->wins[0], c->id, P->wins[1], P->draws, pi); bai_report("match end"); } }
        return;
    }
    static void *px, *py; if (x && y && (x->ch != px || y->ch != py)) { px = x->ch; py = y->ch; g_bai_round = 0; g_bai_draws = 0; g_bai_round_t = 0; x->wins = y->wins = 0; }   // a new pair: a new match
    static BaiBot stock; if (x && !y && g_bai_vs_npc[0] && x->op && x->op != cd_local_pawn()) { memset(&stock.o, 0, sizeof stock.o); stock.o.me = x->o.op; stock.ch = x->op; snprintf(stock.id, sizeof stock.id, "stock:%s", g_bai_vs_npc); stock.used = 0; y = &stock;
        if (y->ch != py) { py = y->ch; px = x->ch; g_bai_round = 0; g_bai_draws = 0; g_bai_round_t = 0; x->wins = stock.wins = 0; } }
    if (!x || !y || g_bai_round >= g_bai_match_rounds) return;
    if (x->frozen || (y != &stock && y->frozen)) return;
    static float nsp[2][3], nsr[2]; static int nsp_ok, nmw[2];
    if (g_bai_round_t == 0) { g_bai_round_t = now; nsr[0] = nsr[1] = 1.f; nmw[0] = nmw[1] = 0; nsp_ok = api_p_test_get_location(BAI, x->ch, nsp[0]) && api_p_test_get_location(BAI, y->ch, nsp[1]); logf_("bot: match start %s vs %s, %d rounds", x->id, y->id, g_bai_match_rounds); }
    if (now < g_bai_round_t) return;
    float hx = bo_hp_frac(&x->o.me), hy = bo_hp_frac(&y->o.me); int end = hx < 0.25f || hy < 0.25f || now - g_bai_round_t > g_bai_round_max;
    if (!end) return;
    BaiBot *w = hx < 0.25f && hy >= 0.25f ? y : hy < 0.25f && hx >= 0.25f ? x : NULL;
    g_bai_round++; if (w) w->wins++; else g_bai_draws++;
    logf_("bot: match round %d winner %s (%s hp %.2f, %s hp %.2f, %.1f s)%s", g_bai_round, w ? w->id : "draw", x->id, hx, y->id, hy, now - g_bai_round_t, now - g_bai_round_t > g_bai_round_max ? " timeout" : "");
    int wi = w == x ? 0 : w == y ? 1 : -1; if (wi >= 0) nmw[wi]++; int fresh = wi < 0 || g_bai_match_to <= 0 || nmw[0] >= g_bai_match_to || nmw[1] >= g_bai_match_to;
    if (fresh) { if (wi >= 0) nmw[0] = nmw[1] = 0; nsr[0] = nsr[1] = 1.f;
        api_p_test_heal(BAI, x->ch); api_p_test_heal(BAI, y->ch); bai_freeze(x, now, 1.0); if (y != &stock) bai_freeze(y, now, 1.0); if (y != &stock) bai_face_off(x, y); g_bai_round_t = now + 1.0; if (wi >= 0 && g_bai_start_shards >= 0) bai_set_shards(x->ch, (float)g_bai_start_shards);
    } else {                                                                                  // 1v1 round flow (see bai_respawn_far)
        BaiBot *lb = wi == 0 ? y : x; float add = 150.f * bai_heal_coef(nsr[wi] - nsr[1 - wi]), mh = w->o.me.s.v[BO_MAXHEALTH] > 0 ? w->o.me.s.v[BO_MAXHEALTH] : 1.f;
        bai_add_health(w->ch, add); api_p_test_heal(BAI, lb->ch); if (lb != &stock) bai_freeze(lb, now, 1.0); bai_respawn_far(lb->ch, w->ch, nsp, nsp_ok); g_bai_round_t = now + 0.5;   /* + 0.5: no round-end check on the cached (pre-heal) health */
        float wf = bo_hp_frac(&w->o.me) + add / mh; nsr[wi] = wf > 1.f ? 1.f : wf; nsr[1 - wi] = 1.f; logf_("bot: round flow: %s keeps its health + %.0f heal-on-kill, %s respawns fresh", w->id, add, lb->id);
    }
    for (BaiBot *b = x; b; b = b == x && y != &stock ? y : NULL) { if (!fresh && b == w) continue; bc_clear(&b->q); b->holding = 0; api_p_test_guard(BAI, b->ch, 0); b->st.fsm = BS_NEUTRAL; b->st.guard_up = 0; }
    if (g_bai_round >= g_bai_match_rounds) { x->done = 1; api_p_test_heal(BAI, x->ch); api_p_test_set_invincible(BAI, x->ch, 1); if (y != &stock) { y->done = 1; api_p_test_heal(BAI, y->ch); api_p_test_set_invincible(BAI, y->ch, 1); } logf_("bot: match done %s %d - %s %d, draws %d", x->id, x->wins, y->id, y->wins, g_bai_draws); bai_report("match end"); }
}

// ----- tick
// player clone: config bot_ai_clone = <preset>[,<preset>...]: every second / third LOCAL player's pawn (a real player character with
// its own save) is adopted by a brain, the n-th clone found by the n-th preset (the last preset repeats). Found by a scan once per second: a FightingCharacter
// with no NPC archetype, possessed by a PlayerController, that is not player 0's pawn. With bot_ai_mode = bots two clones fight each other (pairing by place).
static char g_bai_clone[160]; static int g_bai_clone_n;
typedef struct { void *self, *found; } BaiClone;
static void bai_clone_cb(void *o, void *u) { BaiClone *f = (BaiClone *)u; if (f->found || o == f->self || bai_of(o) || !BAI->is_a(BAI, o, "FightingCharacter") || BAI->npc_archetype(BAI, o)) return;
    char ap[300] = ""; if (BAI->object_path(BAI, o, ap, sizeof ap) && strstr(ap, "Default__")) return;
    void **cp = (void **)BAI->prop_ptr(BAI, o, "Controller"); void *pc = cp && se_valid_ptr(*cp) ? *cp : NULL;   // skip the title screen's StartMap pawn (no controller)
    if (!pc || !BAI->is_a(BAI, pc, "PlayerController")) return; f->found = o; }
static void bai_clone_preset(int n, char *out, size_t cap) {   // n-th entry of the comma list (the last one repeats)
    const char *p = g_bai_clone; for (int i = 0; i < n; i++) { const char *c = strchr(p, ','); if (!c) break; p = c + 1; }
    size_t k = 0; while (p[k] && p[k] != ',' && k + 1 < cap) { out[k] = p[k]; k++; } out[k] = 0; }
static void bai_clone_poll(double now) {
    static double t; if (!g_bai_clone[0] || now - t < 1.0) return; t = now;
    void *pl = cd_local_pawn(); if (!pl) return; BaiClone f = { pl, NULL }; BAI->for_each_object(BAI, bai_clone_cb, &f); if (!f.found) return;
    char id[64]; bai_clone_preset(g_bai_clone_n, id, sizeof id);
    logf_("bot: clone %d: a local player's pawn %p found -> brain %s", g_bai_clone_n + 1, f.found, id); bai_adopt(f.found, NULL, id);
    if (bai_of(f.found)) g_bai_clone_n++; else logf_("bot: clone: adoption FAILED (preset '%s' unknown or its config did not load)", id); }
static void bai_tick(float dt, void *u) {
    (void)u; if (!g_bai_on) return; double now = bai_now(); int n = 0;
    __try {
        bai_clone_poll(now);
        for (int i = 0; i < BAI_MAX; i++) {
            BaiBot *b = &g_bai[i]; if (!b->used) continue; n++;
            if (!tk_ptr_ok(b->ch)) { b->used = 0; continue; }
            if (b->frozen) { if (now < b->frozen_until) continue; b->frozen = 0; if (!b->done) api_p_test_set_invincible(BAI, b->ch, 0); }   // settle over: fight on
            if (b->done) continue;   // match over: healed, invincible, idle until the runner removes it (no post-match fight)
            if (g_trs.on && g_trs.bot == b->ch && (g_trs.ai_paused || g_trs.phase != TP_FIGHT)) { b->move_on = 0; continue; }   // no actions before the countdown ends / between rounds (the trial pauses the stock AI only)
            BaiBot *other = NULL; for (int j = 0; j < BAI_MAX; j++) if (j != i && g_bai[j].used && !g_bai[j].ally && !b->ally && (!g_bai_mode_bots || g_bai[j].pair == b->pair)) other = &g_bai[j];   // bots mode: only its own pair
            // opponent: mode "bots" never targets the player (no opponent = wait, e.g. between two spawns or removals, or before the baseline NPC is found);
            // default mode: the player only, even when other bots exist (dev-menu spawns). A new opponent clears the queued commands.
            void *op = b->ally ? bai_ally_op(b, now) : g_bai_mode_bots ? (other ? other->ch : g_bai_vs_npc[0] ? bai_find_npc(b->ch) : NULL) : cd_local_pawn();
            if (op != b->op) { b->op = op; bc_clear(&b->q); b->holding = 0; b->move_on = 0; if (b->st.guard_up) { api_p_test_guard(BAI, b->ch, 0); b->st.guard_up = 0; }
                int st = op ? bai_set_target(b->ch, op) : bai_set_target(b->ch, NULL);
                char onm[100] = ""; if (op) BAI->object_name(BAI, op, onm, sizeof onm);
                logf_("bot: %s opponent %s %s (lock -> %d)%s", b->id, !op ? "NONE (waiting)" : other && op == other->ch ? other->id : op == cd_local_pawn() ? "player" : "stock npc", onm, st, b->ally ? " [ally mode]" : ""); }
            if (now - b->idle_chk > 0.25) { b->idle_chk = now;                                      // bots can play the open-world idle fidget (scratching / hunching) mid-fight, mostly around feints, counters and being countered.
                void **aic = (void **)api_p_prop_ptr(BAI, b->ch, "m_AIComponent");                       // AIFightingComponent.m_uiIdleIndex (calm idle variant) vs m_uiAlertedIdle (the combat idle): with the behaviour tree stopped nothing keeps the alerted one
                if (aic && tk_ptr_ok(*aic)) { uint8_t *ix = (uint8_t *)api_p_prop_ptr(BAI, *aic, "m_uiIdleIndex"), *al = (uint8_t *)api_p_prop_ptr(BAI, *aic, "m_uiAlertedIdle"); uint8_t *ao = (uint8_t *)api_p_prop_ptr(BAI, *aic, "m_bAlertedOnce");
                    if (ix && al && *al != 255 && *ix != *al) { b->idle_fix++; if (b->idle_fix <= 12 || b->idle_fix % 50 == 0) logf_("bot: %s: IDLE INDEX %d != alerted idle %d (alerted-once %d, fsm %s, attacking %.0f) -> set to the alerted idle (#%d)", b->id, *ix, *al, ao ? *ao : -1, BS_NAMES[b->st.fsm], b->o.me.s.v[BO_ATTACKING], b->idle_fix); *ix = *al; } } }
            if (op && !b->st.unlocked && now - b->tgt_check > 0.25) {   /* the brain's unlocked guard is not re-locked here */ b->tgt_check = now; void *cur = bai_target_of(b->ch);                // re-assert the lock every 0.25 s (like the stock NPC baseline)
                if (cur != op) { int st = bai_set_target(b->ch, op); logf_("bot: %s target was %s - re-set to its opponent -> %d", b->id, cur == cd_local_pawn() ? "the player" : cur ? "another pawn" : "none", st); }
                if (b->ally && !bai_of(op) && bai_dist(b->ch, op) < 600.f) {                                    // ally mode: the engaged stock enemy fights the ally back (re-asserted every 0.25 s like the baseline NPC below)
                    void *nt = bai_target_of(op); if (nt != b->ch) { int ft = BAI->npc_set_target ? BAI->npc_set_target(BAI, op, b->ch) : -1, lt = bai_set_target(op, b->ch);
                        static double alog; if (now - alog > 2.0) { alog = now; logf_("bot: %s: enemy %p target was %s - set to the ally (force enemy %d, lock %d, dist %.0f)", b->id, op, nt == cd_local_pawn() ? "the PLAYER" : nt ? "another pawn" : "none", ft, lt, bai_dist(b->ch, op)); } } }
                if (g_bai_mode_bots && g_bai_vs_npc[0] && !other) {                                  // the stock baseline NPC keeps the bot as its enemy (ForceEnemy alone loses to the NPC's own target choice)
                    void *nt = bai_target_of(op), *pl = cd_local_pawn(); static double nlog; int fixed = 0;
                    if (nt != b->ch) { if (BAI->npc_set_target) BAI->npc_set_target(BAI, op, b->ch); fixed = bai_set_target(op, b->ch) ? 1 : -1;       // ForceEnemy + the NPC's own lock target (what its fight tree attacks)
                        if (now - nlog > 2.0) { nlog = now; logf_("bot: stock npc target was %s - set to %s -> %d", nt == pl ? "the PLAYER" : nt ? "another pawn" : "none", b->id, fixed); } }
                    static void *fsplit; if (fsplit != op && pl && BAI->npc_set_faction && BAI->faction_can_target) { fsplit = op;       // NPC and bot share a faction and the table has npc->bot 0, npc->player 1: split the NPC off (faction 3) so it can target the bot and not the player
                        unsigned char *fb = (unsigned char *)BAI->prop_ptr(BAI, b->ch, "m_eFaction"), *fp = (unsigned char *)BAI->prop_ptr(BAI, pl, "m_eFaction");
                        if (fb && fp) { int nf = BAI->npc_set_faction(BAI, op, 3); BAI->faction_can_target(BAI, 3, *fb, 1); BAI->faction_can_target(BAI, *fb, 3, 1); BAI->faction_can_target(BAI, 3, *fp, 0);
                            logf_("bot: stock npc split to faction %d (bot %d, player %d): npc->bot %d, npc->player %d", nf, *fb, *fp, BAI->faction_can_target(BAI, 3, *fb, -1), BAI->faction_can_target(BAI, 3, *fp, -1)); } }
                    static int flog; if (!flog && pl && BAI->prop_ptr && BAI->faction_can_target) { flog = 1; unsigned char *fn = (unsigned char *)BAI->prop_ptr(BAI, op, "m_eFaction"), *fp = (unsigned char *)BAI->prop_ptr(BAI, pl, "m_eFaction"), *fb = (unsigned char *)BAI->prop_ptr(BAI, b->ch, "m_eFaction");
                        if (fn && fp && fb) logf_("bot: factions npc %d player %d bot %d; table npc->player %d npc->bot %d", *fn, *fp, *fb, BAI->faction_can_target(BAI, *fn, *fp, -1), BAI->faction_can_target(BAI, *fn, *fb, -1)); } }
                if (g_bai_verbose && now - b->tgt_log > 10.0) { b->tgt_log = now; logf_("bot: target %s = %s", b->id, other && op == other->ch ? other->id : op == cd_local_pawn() ? "player" : "stock npc"); } }
            if (now - b->bt_check > 0.25) { b->bt_check = now; if (bai_bt_running(b->ch) == 1) { int s0 = api_p_test_ai(BAI, b->ch, 0), h0 = api_p_test_hittable(BAI, b->ch, 1); b->bt_restarts++;   // before the no-opponent skip: the stock tree can restart after adoption and after face_off / heal, so the check also runs while an ally waits
                if (b->bt_restarts <= 5) logf_("bot: %s: the stock behaviour tree was running again (restart %d, opponent %s) - stopped -> %d, hittable -> %d", b->id, b->bt_restarts, b->op ? "set" : "NONE", s0, h0); } }
            if (!b->op) continue;
            bai_weapon_policy(b, now);
            if (g_bai_hp_trace) { static int probed; if (!probed && BAI->dump_object) { probed = 1;                                // debug dump (bot_ai_hp_trace): health, movement, animation graph, character and AI components -> <loader folder>\objdump_*.txt
                void *hc = se_comp(BAI, b->ch, "m_HealthComponent"), **aic = (void **)api_p_prop_ptr(BAI, b->ch, "m_AIComponent");
                { void *mcd = bai_movecomp(b->ch); logf_("bot: idle hunt %s: dump_object movement component %d", b->id, mcd ? BAI->dump_object(BAI, mcd) : -1); }
                { void **ag = (void **)api_p_prop_ptr(BAI, b->ch, "m_AnimationGraph"); logf_("bot: idle hunt %s: dump_object animation graph %d", b->id, ag && tk_ptr_ok(*ag) ? BAI->dump_object(BAI, *ag) : -1); }
                logf_("bot: regen hunt %s: dump_object health component %d, character %d, AI component %d", b->id, hc ? BAI->dump_object(BAI, hc) : -1, BAI->dump_object(BAI, b->ch), aic && tk_ptr_ok(*aic) ? BAI->dump_object(BAI, *aic) : -1); } }
            if (g_bai_hp_trace) { static double hl[BAI_MAX]; int hi = (int)(b - g_bai); if (now - hl[hi] > 0.5) { hl[hi] = now; unsigned ord_mask = 0; for (int ot = 0; ot < 32; ot++) if (bai_order_running(b->ch, ot, 2) > 0) ord_mask |= 1u << ot;   // running order types (EOrderType: 1 Attack, 16 Emote ...)
                { void **ag = (void **)api_p_prop_ptr(BAI, b->ch, "m_AnimationGraph"); char ms[24] = "?", wf[8] = "?", fp[8] = "?", ii[8] = "?"; if (ag && tk_ptr_ok(*ag)) { BAI->get_property_text(BAI, *ag, "m_eMoveStatus", ms, sizeof ms); BAI->get_property_text(BAI, *ag, "m_bWantsFidget", wf, sizeof wf); BAI->get_property_text(BAI, *ag, "m_bFidgetAnimPlaying", fp, sizeof fp); BAI->get_property_text(BAI, *ag, "m_bInIdleState", ii, sizeof ii); }
                logf_("bot: hp %s %.1f / %.0f, guard %.0f / %.0f, attacking %.0f, stunned %d, down %.0f, shards %.1f, anim %s fidget-wanted %s fidget-playing %s idle-state %s, fsm %s, last cmd %.2f s ago (%s %s), orders 0x%08X, pair %d, ability-ok %d, dodge-avail %d", b->id, b->o.me.s.v[BO_HEALTH], b->o.me.s.v[BO_MAXHEALTH], b->o.me.s.v[BO_GUARD], b->o.me.s.v[BO_MAXGUARD], b->o.me.s.v[BO_ATTACKING], b->o.me.stunned, b->o.me.s.v[BO_DOWN], b->o.me.s.v[BO_SHARDS], ms, wf, fp, ii, BS_NAMES[b->st.fsm], now - b->last_cmd_t, BC_NAMES[b->last_type], b->last_ctech, ord_mask, b->pair, b->o.me.ability_ok, bai_avail(b->ch, 5)); } } }
            bai_side(b->ch, b->op, &b->o.me, now); bai_side(b->op, b->ch, &b->o.op, now); { float pd = b->o.dist; b->o.dist = bai_dist(b->ch, b->op); b->o.t = now;
                static float jd[BAI_MAX]; static double jt[BAI_MAX]; int ji = (int)(b - g_bai);       // log any distance jump a walk or dodge cannot make (> 250 uu in < 0.2 s)
                static float lp[BAI_MAX][3]; static double lpt[BAI_MAX]; float mp[3] = { 0, 0, 0 }, opn[3] = { 0, 0, 0 }; int hp_ = api_p_test_get_location(BAI, b->ch, mp), ho_ = api_p_test_get_location(BAI, b->op, opn);   /* positions (teleports / falling out of the map) */
                if (jt[ji] > 0 && now - jt[ji] < 0.2 && fabsf(b->o.dist - jd[ji]) > 250.f) logf_("bot: JUMP %s: distance to its opponent %.0f -> %.0f uu in %.3f s (fsm %s, attacking %.0f, opp attacking %.0f) | me %.0f %.0f %.0f (moved %.0f) op %.0f %.0f %.0f | last cmd %s %s", b->id, jd[ji], b->o.dist, now - jt[ji], BS_NAMES[b->st.fsm], b->o.me.s.v[BO_ATTACKING], b->o.op.s.v[BO_ATTACKING], mp[0], mp[1], mp[2], hp_ ? sqrtf((mp[0] - lp[ji][0]) * (mp[0] - lp[ji][0]) + (mp[1] - lp[ji][1]) * (mp[1] - lp[ji][1])) : -1.f, opn[0], opn[1], opn[2], b->last_type >= 0 && b->last_type < BC_TYPES ? BC_NAMES[b->last_type] : "-", b->last_ctech);
                if (hp_ && now - lpt[ji] > 1.0) { lpt[ji] = now; logf_("bot: pos %s %.0f %.0f %.0f | op %.0f %.0f %.0f | d %.0f", b->id, mp[0], mp[1], mp[2], opn[0], opn[1], opn[2], b->o.dist); }
                if (hp_) { lp[ji][0] = mp[0]; lp[ji][1] = mp[1]; lp[ji][2] = mp[2]; } (void)ho_;
                jd[ji] = b->o.dist; jt[ji] = now; (void)pd; }
            b->ticks++; if (b->o.dist <= b->cfg.range_uu + 30) b->ticks_range++;
            if (b->o.me.gold_open && !b->gold_prev) { b->gold_open_t = now; if (b->o.me.row[0] && b->atk_t > 0) { bai_win_put(b, b->o.me.row, now - b->atk_t); if (g_bai_verbose > 1) logf_("bot: %s gold window open row %s at %.1f F", b->id, b->o.me.row, (now - b->atk_t) * 30.0); } }
            b->gold_prev = b->o.me.gold_open;
            if (g_bai_verbose > 1) { static double tl; BotSide *os = &b->o.op;
                if (now - tl > 0.5 || (os->seq && os->seq != b->st.def_seq && os->s.v[BO_ATTACKING] > 0 && now - tl > 0.03)) { tl = now;
                    logf_("bot: obs %s fsm %s | me valid %d stance %.0f atk %.0f row %s f %.1f gold %d intr %d ab %d | op valid %d atk %.0f row %s seq %u f %.1f phase %d to-hit %.1f lock %d | d %.0f | q %d hold %d",
                          b->id, BS_NAMES[b->st.fsm], b->o.me.valid, b->o.me.s.v[BO_STANCE], b->o.me.s.v[BO_ATTACKING], b->o.me.row[0] ? b->o.me.row : "-", b->o.me.frame, b->o.me.gold_open, b->o.me.intr_open, b->o.me.ability_ok,
                          os->valid, os->s.v[BO_ATTACKING], os->row[0] ? os->row : "-", os->seq, os->frame, bo_phase(os), bo_frames_to_hit(os), os->locked_on, b->o.dist, b->q.n, b->holding); } }
            { int was = b->st.tired;
            { static const BotRow z; static double sf_t[BAI_MAX]; int bi = (int)(b - g_bai); if (bi >= 0 && bi < BAI_MAX && now - sf_t[bi] > 1.0) { sf_t[bi] = now; for (int e = 0; e < 4; e++) { BotRow r = z; b->cfg.starter_f[e] = b->cfg.deck_n[e] > 0 && bai_row(b->cfg.deck[e][0], &r) && r.ok ? (float)r.startup : 0.f; } } }   // the interrupt option needs each stance's starter speed (follows the deck in hand)
            if (!b->frozen && now - b->hit_chk > 0.5) { b->hit_chk = now; int cleared = 0; __try { void **aic = api_p_prop_ptr(BAI, b->ch, "m_AIComponent"); cleared = aic && tk_ptr_ok(*aic) && ((uint8_t *)*aic)[0x140] == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { cleared = 0; }
              if (cleared) { static int nlog; if (nlog++ < 30) logf_("bot: %s: m_bCanBeHitted was CLEARED - set again", b->id); api_p_test_hittable(BAI, b->ch, 1); } }   // stopping the stock tree clears UAIComponent m_bCanBeHitted (unlocked attacks then do not register): keep it set
            if (b->gd_want && bai_avail(b->ch, 2)) { b->gd_want = 0; api_p_test_guard(BAI, b->ch, 1); }   // the held guard comes up when the stun ends
            { static int sl = -1; if (sl < 0) sl = (int)bai_cfg_num("bot_ai_lab_stance", 0);   /* test mode: stance change timing + m_bAntiSpam */
              if (sl && b == &g_bai[0]) { static double t0, ts, ta, tg, td; static int want = -1, n; int q = bo_stance(&b->o.me);
                if (want < 0 && now - t0 > 3.0 && !bo_attacking(&b->o.me)) { want = q == 3 ? 0 : 3; t0 = now; ts = ta = tg = td = 0; logf_("bot: LAB-STANCE before the press: attack %d guard %d dodge %d (state attacking %.0f)", bai_avail(b->ch, 0), bai_avail(b->ch, 2), bai_avail(b->ch, 5), b->o.me.s.v[BO_ATTACKING]); int ok = bai_stance(b, want); if (!ok) want = -1; else n++;
                  if (n == 1) { uint8_t *ac = se_comp(BAI, b->ch, "m_AttackComponent"); void *pl = cd_local_pawn(); uint8_t *pac = pl ? se_comp(BAI, pl, "m_AttackComponent") : NULL; logf_("bot: LAB-STANCE m_bAntiSpam bot %d player %d", ac ? ac[0x344] : -1, pac ? pac[0x344] : -1); } }
                if (want >= 0) { double e = now - t0; if (!ts && q == want) ts = e; if (!ta && e > 0.02 && bai_avail(b->ch, 0)) ta = e; if (!tg && e > 0.02 && bai_avail(b->ch, 2)) tg = e; if (!td && e > 0.02 && bai_avail(b->ch, 5)) td = e;
                  if (e > 2.0) { logf_("bot: LAB-STANCE %d -> %d: stance shown %.3f s, attack %.3f, guard %.3f, dodge %.3f (0 = never within 2 s)", want == 3 ? 0 : 3, want, ts, ta, tg, td); want = -1; } }
                continue; } }
            { static int lab = -1; if (lab < 0) lab = (int)bai_cfg_num("bot_ai_lab_guardhole", 0);   /* test mode: the first bot guards in BackLeft, the second throws row 003 (horizontal, left origin) */
              if (lab) { int victim = b == &g_bai[0]; static double t_atk, t_g, t_h; if (victim) { if (bo_stance(&b->o.me) != 3) bai_stance(b, 3); if (now - t_g > 0.3) { t_g = now; api_p_test_guard(BAI, b->ch, 1); } if (now - t_h > 3) { t_h = now; api_p_test_heal(BAI, b->ch); } }
                else if (now - t_atk > 2.5 && !bo_attacking(&b->o.me)) { t_atk = now; BotCmd c; memset(&c, 0, sizeof c); c.type = BC_ATTACK; snprintf(c.row, sizeof c.row, "003"); int pf = 0; char rw[24] = ""; int ok = bai_attack(b, &c, 0, &pf, rw);
                  logf_("bot: LAB-C1 attack 003 launched %d; victim stance %d guarding %.0f locked %d", ok, bo_stance(&g_bai[0].o.me), g_bai[0].o.me.s.v[BO_GUARDING], bai_target_of(g_bai[0].ch) == g_bai[0].op); }
                continue; } }
            if (b->cfg.policy == BP_NN) { bai_nn_load(); bai_nn_step(b, now); } else bot_step(&b->cfg, &b->st, &b->o, now, dt, &b->q);
            { static int ct = -1; if (ct < 0) ct = (int)bai_cfg_num("bot_ai_cancel_test", 0); if (ct && bo_attacking(&b->o.me) && b->o.me.frame >= 2.f && b->cx_seq != b->o.me.seq) bai_cancel(b); }   /* test mode: mid-attack feint check (every attack cancelled 2 F in) */
              if (b->st.tired != was) logf_("bot: %s stamina %s (guard gauge %.0f / %.0f)", b->id, b->st.tired ? "STOP: blocks instead of pressing" : "ok again: presses", b->o.me.s.v[BO_GUARD], b->o.me.s.v[BO_MAXGUARD]); }
            BotCmd c; int guard = 0; while (guard++ < 8 && bc_pop_due(&b->q, now, &c)) { bai_exec(b, &c, now); b->last_cmd_t = now; b->last_type = c.type; snprintf(b->last_ctech, sizeof b->last_ctech, "%s", c.tech); }
            if (b->last_cmd_t == 0) b->last_cmd_t = now;                                                  // stall detector: in range, not stunned, no own attack, no command for 2.5 s
            if (now - b->last_cmd_t > 2.5 && now - b->stall_log > 3.0 && b->o.dist < 300.f && !b->o.me.stunned && b->o.me.s.v[BO_ATTACKING] <= 0) { b->stall_log = now;
                logf_("bot: STALL %s: no command for %.1f s at %.0f uu (fsm %s, tired %d, queue %d, holding %d, move_on %d, own guard %.0f / %.0f, stance %d, gold %d intr %d ability %d, opp attacking %.0f phase %d stunned %d, shards %.1f, weapon %d, guard_up %d, guard_until %+.1f s, next_neutral %+.1f s, queue head type %d due %+.1f s row %s tech %s, state age %.1f s, pair %d, lock me->opp %d opp->me %d)", b->id, now - b->last_cmd_t, b->o.dist, BS_NAMES[b->st.fsm], b->st.tired, b->q.n, b->holding, b->move_on, b->o.me.s.v[BO_GUARD], b->o.me.s.v[BO_MAXGUARD], bo_stance(&b->o.me), b->o.me.gold_open, b->o.me.intr_open, b->o.me.ability_ok, b->o.op.s.v[BO_ATTACKING], bo_phase(&b->o.op), b->o.op.stunned, b->o.me.s.v[BO_SHARDS], b->sword_in_hand, b->st.guard_up, b->st.guard_until - now, b->st.t_next_neutral - now, b->q.n ? b->q.q[0].type : -1, b->q.n ? b->q.q[0].due - now : 0.0, b->q.n ? b->q.q[0].row : "-", b->q.n ? b->q.q[0].tech : "-", now - b->st.t_state, b->pair, bai_target_of(b->ch) == b->op, b->op && bai_target_of(b->op) == b->ch);
                { void **ag = (void **)api_p_prop_ptr(BAI, b->ch, "m_AnimationGraph"); char ms[48] = "?", wf[16] = "?", fp[16] = "?", ii[16] = "?"; if (ag && tk_ptr_ok(*ag)) { BAI->get_property_text(BAI, *ag, "m_eMoveStatus", ms, sizeof ms); BAI->get_property_text(BAI, *ag, "m_bWantsFidget", wf, sizeof wf); BAI->get_property_text(BAI, *ag, "m_bFidgetAnimPlaying", fp, sizeof fp); BAI->get_property_text(BAI, *ag, "m_bInIdleState", ii, sizeof ii); }
                  logf_("bot: STALL anim %s: move status %s, wants fidget %s, fidget playing %s, in idle state %s", b->id, ms, wf, fp, ii); } }
            bai_hold(b, now);
            if (b->move_on) { if (now > b->move_until) b->move_on = 0; else bai_move(b, b->move_dir, now); }
            bai_face(b, now);
            void *mc = bai_movecomp(b->ch); if (mc) __try { ((void (*)(void *, float))g_bai_ucq)(mc, dt); } __except (EXCEPTION_EXECUTE_HANDLER) { }
            if (b->stance_t > 0 && now - b->stance_t > 0.8) { int q = bo_stance(&b->o.me); logf_("bot: proof stance %s: now %d, wanted %d (%s)", b->id, q, b->stance_want, q == b->stance_want ? "PASS" : "FAIL"); if (q == b->stance_want) bot_note(&b->st, bot_tech_id("U4"), 1); b->stance_t = 0; }
            if (b->feint_state == 1 && b->seq != b->feint_base) b->feint_state = 2;                       // the feinted attack started
            if (b->feint_state == 2 && b->o.me.s.v[BO_ATTACKING] <= 0) {                                   // ... and is over: a feint ends before its strike frame
                BotRow r; bai_row(b->row, &r); double fr = (now - b->atk_t) * 30.0; int pass = r.ok && fr < r.strike + 2;
                logf_("bot: proof feint %s: own attack %s ended at frame %.0f (strike frame %d, cancel frame %d) (%s)", b->id, b->row, fr, r.strike, r.cancel, pass ? "PASS" : "FAIL: it ran past its strike");
                if (pass) bot_note(&b->st, BT_U2, 1); b->feint_state = 0; }
            if (0 && b->cfg.whiff_cancel && b->pending_ab   /* the brain decides the whiff cancel now (kx wc_rate / wc_delay_s) */ && !b->ab_whiff_done && b->ab_t > 0 && now - b->ab_t > 0.35 && (b->cfg.ability_kind == BA_AVOID || b->cfg.ability_kind == BA_ABSORB) && bo_attacking(&b->o.op)) {
                b->ab_whiff_done = 1; int av = bai_avail(b->ch, 5), ok = av ? bai_dodge(b, 0) : 0; if (ok) b->dodge_t = now;
                logf_("bot: tech WC %s whiff-cancel: ability %.2f s ago, no defence event -> dodge available %d, launched %d", b->id, now - b->ab_t, av, ok); }
            if (b->pend_t > 0 && now - b->pend_t > 2.0) { if (b->pending_ab || b->pending_dg || b->pending_pw) logf_("bot: %s: no defence event within 2 s of the action (ability %d dodge %d power %d guard %d)", b->id, b->pending_ab, b->pending_dg, b->pending_pw, b->pending_gd);
                b->pending_ab = b->pending_dg = b->pending_pw = b->pending_gd = 0; b->pend_t = 0; }
        }
        if (n) bai_match(now);
        if (n && now - g_bai_last_report > 30) { g_bai_last_report = now; bai_report("every 30 s"); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { static int k; if (k++ < 5) logf_("bot: EXCEPTION 0x%08lX in the tick (skipped)", GetExceptionCode()); }
}

// ----- dev menu: one entry per brain preset (spawns it 350 uu in front of the player)
static void bai_menu_spawn(void *u) { BaiPreset *p = (BaiPreset *)u; void *n = api_p_npc_spawn(BAI, p->id, NULL, NULL, NULL); logf_("bot: menu: spawn %s -> %p", p->id, n); }
static void bai_menu_once(void) {
    static int done; if (done || !g_bai_npre) return; done = 1;
    for (int i = 0; i < g_bai_npre; i++) { char lb[80]; snprintf(lb, sizeof lb, "Bot: spawn %s", g_bai_pre[i].id); api_p_add_menu_item(BAI, lb, bai_menu_spawn, &g_bai_pre[i]); }
    logf_("bot: dev menu: %d spawn entries", g_bai_npre);
}
static void bai_tick_menu(float dt, void *u) { (void)dt; (void)u; if (g_bai_on && g_bai_npre) bai_menu_once(); }

static int bai_cfg_on(const char *key) { char v[32] = ""; wchar_t src[MAX_PATH]; return setting_get(key, L"", 0, v, sizeof v, src) && (!_stricmp(v, "on") || !_stricmp(v, "1") || !_stricmp(v, "true")); }
static double bai_cfg_num(const char *key, double dflt) { char v[32] = ""; wchar_t src[MAX_PATH]; return setting_get(key, L"", 0, v, sizeof v, src) && v[0] ? atof(v) : dflt; }
static void bai_init(const AbsApi *a) {
    BAI = a; g_bai_on = bai_cfg_on("bot_ai"); g_bt_entry_cb = bai_entry; g_bt_spawn_cb = bai_spawned;
    if (!g_bai_on) { logf_("bot: off (config bot_ai not set): brain presets stay stock AIs"); return; }
    g_bai_t0 = 0; g_bai_t0 = bai_now(); g_bai_verbose = (int)bai_cfg_num("bot_ai_log", 1); g_bai_match_rounds = (int)bai_cfg_num("bot_ai_match", 0); g_bai_match_to = (int)bai_cfg_num("bot_ai_match_to", 3); g_bai_round_max = bai_cfg_num("bot_ai_round_s", 60);
    { char v[32] = ""; wchar_t src[MAX_PATH]; g_bai_mode_bots = setting_get("bot_ai_mode", L"", 0, v, sizeof v, src) && !_stricmp(v, "bots"); logf_("bot: mode %s", g_bai_mode_bots ? "bots (bot vs bot / stock NPC; the player is never a target)" : "player (every brain bot fights the player)"); }
    { char v[16] = ""; wchar_t src[MAX_PATH]; if (setting_get("bot_ai_hp_trace", L"", 0, v, sizeof v, src) && v[0]) g_bai_hp_trace = atoi(v) != 0; }
    { char v[16] = ""; wchar_t src[MAX_PATH]; if (setting_get("bot_ai_start_shards", L"", 0, v, sizeof v, src) && v[0]) { g_bai_start_shards = atoi(v); logf_("bot: every bot starts each round with %d shards (config bot_ai_start_shards, tests only)", g_bai_start_shards); } }
    { char v[64] = ""; wchar_t src[MAX_PATH]; if (setting_get("bot_ai_vs_npc", L"", 0, v, sizeof v, src) && v[0]) { snprintf(g_bai_vs_npc, sizeof g_bai_vs_npc, "%s", v); logf_("bot: baseline mode: a brain bot fights the stock NPC whose archetype path contains '%s'", v); } }
    { wchar_t src[MAX_PATH]; if (setting_get("bot_ai_clone", L"", 0, g_bai_clone, sizeof g_bai_clone, src) && g_bai_clone[0]) logf_("bot: clone mode: a second local player's pawn gets brain %s", g_bai_clone); else g_bai_clone[0] = 0; }
    int fns = bai_fns();
    int ea = api_p_on_attack_start(a, bai_on_attack, NULL), eh = api_p_on_hit(a, bai_on_hit, NULL), ed = api_p_on_defense(a, bai_on_defense, NULL), ex = api_p_on_pawn_destroyed(a, bai_on_destroyed, NULL);
    int ep = api_p_on_power_used(a, bai_on_power, NULL), et = api_p_on_tick(a, bai_tick, NULL), em = api_p_on_tick(a, bai_tick_menu, NULL);
    logf_("bot: ON (natives %d) | events attack %d hit %d defense %d destroyed %d power %d tick %d menu %d | match rounds %d (round max %.0f s), log level %d. bot_reads_inputs is never used by the bot.",
          fns, ea, eh, ed, ex, ep, et, em, g_bai_match_rounds, g_bai_round_max, g_bai_verbose);
    if (!fns || et) { logf_("bot: natives or the tick missing - bot OFF"); g_bai_on = 0; }
}

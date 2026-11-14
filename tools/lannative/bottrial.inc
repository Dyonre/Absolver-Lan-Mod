// bottrial.inc - solo combat trials against bots (included at the end of bots.inc). API: docs\PLUGIN-API.md "Bot trials (v2.16)".
// Content kind "bot_trials" (Mods\<mod>\content\bot_trials\*.json): {id, name, map, bot | bots[] (a gauntlet), rounds_to_win, ai_level, ai_level_step, rewards (a ranks id),
//   spawn_distance, round_gap, round_seconds, end_delay, return, menu}. Started from the F2 Mods page ("Content: Bot trial: <name>") or bot_trial_start(id).
// Flow (one 0.25 s timer): solo_duel(1) + travel(map) -> the map's 1v1 game mode reaches InProgress -> m_iNbWinningRounds = rounds_to_win -> each round: npc_spawn(bot) at the
//   other team's player start like a second player (fallback: spawn_distance in front of the player) at the
//   stage's AI level, approach = RUN (MoveToActor; pathfinding only on a runtime navmesh) + LOCK-MOVE (SetTarget + movement input) -> the plugin counts rounds ITSELF from on_death (+ a dead-state poll):
//   player-like trials give the bot a player state, so the stock code scores the kills; otherwise (or if that fails) a player point is written into the player state's m_Scores
//   (BPF_GetTeamScore sums them): the stock team score, the HUD and the map script "score" events see a real scored point.
//   -> next round / next bot (gauntlet, AI level + ai_level_step per bot) -> trial_reward(pay) -> banner -> travel back. Pure logic: bottrial.h.
#include "bottrial.h"

typedef struct { char mod[MOD_NAME_MAX], id[CR_ID_MAX], name[96], map[200], ret[200], ranks[CR_ID_MAX]; char bots[BTR_BOTS_MAX][CR_ID_MAX]; int nbots, rounds, level, step, menu, menu_added, assist;
                 int player_like, block_respawn, intro; float dist, gap, round_s, end_delay; char player_intro[64]; } CTrial;   // player_intro: optional intro row of the local player for this trial
static CTrial g_trl[16]; static int g_trl_n;
static const char *const TR_KNOWN[] = { "id", "name", "map", "bot", "bots", "rounds_to_win", "ai_level", "ai_level_step", "rewards", "spawn_distance", "round_gap", "round_seconds", "end_delay", "return", "menu", "approach_assist",
                                        "player_like", "block_respawn", "intro", "player_intro", NULL };

static int tr_parse(const JDoc *d, CTrial *t, char *err, int cap) {
    memset(t, 0, sizeof *t); const char *v; int bl = jfind(d, 0, "bots");
    t->rounds = 3; t->level = -1; t->step = 0; t->menu = 1; t->dist = 350.f; t->gap = 3.f; t->round_s = 180.f; t->end_delay = 8.f;
    snprintf(t->ret, sizeof t->ret, "/Game/Maps/MainMap/MainMap_Main");
    if (!(v = cs_str(d, 0, "map")) || strncmp(v, "/Game/", 6) || strlen(v) >= sizeof t->map) { snprintf(err, cap, "needs \"map\": a duel map path like /Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum"); return 0; }
    snprintf(t->map, sizeof t->map, "%s", v);
    if ((v = cs_str(d, 0, "bot"))) snprintf(t->bots[t->nbots++], CR_ID_MAX, "%s", v);
    if (bl >= 0) {
        if (d->n[bl].t != J_ARR) { snprintf(err, cap, "\"bots\" must be an array of bot ids (a gauntlet, fought in this order)"); return 0; }
        for (int c = d->n[bl].first; c >= 0; c = d->n[c].next) { if (d->n[c].t != J_STR || !d->n[c].str[0] || strlen(d->n[c].str) >= CR_ID_MAX) { snprintf(err, cap, "\"bots\": ids of bots entries"); return 0; } if (t->nbots >= BTR_BOTS_MAX) { snprintf(err, cap, "at most %d bots", BTR_BOTS_MAX); return 0; } snprintf(t->bots[t->nbots++], CR_ID_MAX, "%s", d->n[c].str); }
    }
    if (!t->nbots) { snprintf(err, cap, "needs \"bot\" (a bots / npcs entry id) or \"bots\" [ids]"); return 0; }
    if ((v = cs_str(d, 0, "rounds_to_win"))) { t->rounds = atoi(v); if (t->rounds < 1 || t->rounds > 9) { snprintf(err, cap, "\"rounds_to_win\" 1..9"); return 0; } }
    if ((v = cs_str(d, 0, "ai_level"))) { t->level = atoi(v); if (t->level < 0 || t->level > 100) { snprintf(err, cap, "\"ai_level\" 0..100"); return 0; } }
    if ((v = cs_str(d, 0, "ai_level_step"))) { t->step = atoi(v); if (t->step < -50 || t->step > 50) { snprintf(err, cap, "\"ai_level_step\" -50..50 (added per bot of a gauntlet)"); return 0; } }
    if ((v = cs_str(d, 0, "spawn_distance"))) { t->dist = (float)atof(v); if (t->dist < 150 || t->dist > 2000) { snprintf(err, cap, "\"spawn_distance\" 150..2000 uu"); return 0; } }
    if ((v = cs_str(d, 0, "round_gap"))) { t->gap = (float)atof(v); if (t->gap < 0 || t->gap > 120) { snprintf(err, cap, "\"round_gap\" 0..120 s"); return 0; } }
    if ((v = cs_str(d, 0, "round_seconds"))) { t->round_s = (float)atof(v); if (t->round_s < 20 || t->round_s > 1800) { snprintf(err, cap, "\"round_seconds\" 20..1800"); return 0; } }
    if ((v = cs_str(d, 0, "end_delay"))) { t->end_delay = (float)atof(v); if (t->end_delay < 1 || t->end_delay > 60) { snprintf(err, cap, "\"end_delay\" 1..60 s"); return 0; } }
    if ((v = cs_str(d, 0, "return"))) { if (strncmp(v, "/Game/", 6) || strlen(v) >= sizeof t->ret) { snprintf(err, cap, "\"return\" must be a map path (/Game/...)"); return 0; } snprintf(t->ret, sizeof t->ret, "%s", v); }
    t->assist = 2;                                                                                   // approach_assist: 0 off, 1 on, 2 auto (auto = on from the spawn; duel maps have no navmesh)
    if ((v = cs_str(d, 0, "approach_assist"))) { t->assist = !_stricmp(v, "off") || !_stricmp(v, "false") ? 0 : !_stricmp(v, "on") || !_stricmp(v, "true") ? 1 : !_stricmp(v, "auto") ? 2 : -1; if (t->assist < 0) { snprintf(err, cap, "\"approach_assist\": auto, on or off"); return 0; } }
    t->player_like = cs_str(d, 0, "player_like") ? cs_true(d, 0, "player_like") : 1;               // the bot is a second player (player state on team 1, intro, stock score, stock-style respawn)
    t->block_respawn = cs_str(d, 0, "block_respawn") ? cs_true(d, 0, "block_respawn") : !t->player_like; // the "re-spawn 150 uu away when blocked" fallback (a teleport): off for player-like trials
    t->intro = cs_str(d, 0, "intro") ? cs_true(d, 0, "intro") : 1;
    if ((v = cs_str(d, 0, "player_intro"))) snprintf(t->player_intro, sizeof t->player_intro, "%s", v);   // optional intro of the player (the bot uses its own)
    if ((v = cs_str(d, 0, "rewards"))) snprintf(t->ranks, sizeof t->ranks, "%s", v);
    if (cs_str(d, 0, "menu")) t->menu = cs_true(d, 0, "menu");
    if ((v = cs_str(d, 0, "name"))) snprintf(t->name, sizeof t->name, "%s", v);
    return 1;
}
static int trials_validate(const char *json, char *err, int cap, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, cap, "%s", d.err); jdoc_free(&d); return 0; }
    static CTrial t; int ok = tr_parse(&d, &t, err, cap); const char *id = cs_str(&d, 0, "id");
    if (ok) cs_warn_unknown(&d, "bot_trials", id ? id : "?", TR_KNOWN);
    jdoc_free(&d); return ok;
}

// ================================================================ the running trial
enum { TP_IDLE, TP_TRAVEL, TP_WAIT_MATCH, TP_SPAWN, TP_FIGHT, TP_ROUND_END, TP_END, TP_DONE, TP_ABORTED, TP_PAYOUT, TP_BOT_DOWN, TP_PLAYER_DOWN };
static const char *const TP_NAME[] = { "idle", "travel", "wait_match", "spawn", "fight", "round_end", "end", "done", "aborted", "payout", "bot_down", "player_down" };
static int tr_arena_phase(int p) { return (p >= TP_SPAWN && p <= TP_END) || p == TP_BOT_DOWN || p == TP_PLAYER_DOWN; }
static struct { int on, ti, phase, prev_solo, timer, hooked, last_result, attacks, xp, points, level, logged_ip, paid, assist_on, assist_checked, assist_frames, ticked, near_spawn, stuck_n, tk_fixed; float spawn_loc[3], stuck_d; double stuck_t; BtrMatch m; void *world, *bot, *death_victim; double t_phase, t_start, t_log; char id[CR_ID_MAX], bot_name[96];
                 // player starts of the arena (scanned once per trial), approach timing, hit / interrupt bookkeeping, stock post-match watch
                 float st_loc[16][3]; int st_first[16], st_n, st_scanned, at_start, lock_set, run_calls, run_ok, reached, hit_pending, hit_was_attacking, hit_attacks_at, hits_interrupt, hits_through, hits_idle, pm_logged, brain_off, run_blocked, n_stops; float d_spawn, run_best; double t_run_best, t_stop, t_round; double t_spawn, t_run, hit_t; char hit_attack[32];
                 // block re-spawn at most once per round (respawned), later rounds avoid a blocked far start (far_blocked), the bot's own progress mark; the player fall guard
                 int respawned, far_blocked, still_logged, falls; float prog_loc[3]; double t_prog; TkFall fall;
                 // player-like: the bot's player state (one per trial, moved from pawn to pawn), the intro scenes, the down / respawn bookkeeping
                 void *ps, *bot_ctl, *scenes[4]; int n_scenes, scenes_scanned, early, intro_done, ai_paused, resp_ok, stock_logged, last_tb, rebuilds; float resp_loc[3], resp_yaw, death_respawn_s; double t_down;
                 int detours, detour_active, alert_resets, nav_stall, stall_replaced; unsigned wp_tried; float detour_to[3]; double detour_until, t_detour_end; } g_trs;
#define TR_LOCK_RANGE 300.f                                                                         // lock-move range (the AI's own combat strafe distance is ~200-300 uu)
#define TR_STOP 95.f                                                                                // attack range the assist closes to
static double tr_now(void) { return GetTickCount64() / 1000.0; }
static double tr_since(void) { return tr_now() - g_trs.t_phase; }
static void tr_go(int p) {
    if (g_trs.phase != p) logf_("bottrial: %s: phase %s -> %s", g_trs.id, TP_NAME[g_trs.phase], TP_NAME[p]); g_trs.phase = p; g_trs.t_phase = tr_now();
    if (p == TP_FIGHT) { g_trs.t_prog = tr_now(); float l[3]; if (g_trs.bot && tk_ptr_ok(g_trs.bot) && CA->test_get_location(CA, g_trs.bot, l)) memcpy(g_trs.prog_loc, l, sizeof l); }   // the stall / block clocks run from the start of the fight (the bot is paused through the intro)
}
static CTrial *tr_cur(void) { return g_trs.ti >= 0 && g_trs.ti < g_trl_n ? &g_trl[g_trs.ti] : NULL; }
static int tr_np_def(const char *bot) { for (int i = 0; i < g_np_n; i++) if (!strcmp(g_np[i].id, bot)) return i; return -1; }
static int tr_char(void *ch, AbsCharInfo *ci) { memset(ci, 0, sizeof *ci); ci->size = sizeof *ci; return tk_ptr_ok(ch) && CA->char_info(CA, ch, ci); }
static void tr_world_name(char *out, int cap) { out[0] = 0; void *w = CA->get_world(CA); if (w) CA->object_name(CA, w, out, cap); }

// the player's stock score: AAdversarialPlayerState::m_Scores (one FPlayerKill entry with the count) -> BPF_GetTeamScore -> HUD / map script "score" events
static void tr_write_score(int kills) {
    void *p = np_player(CA), **ps = p ? (void **)CA->prop_ptr(CA, p, "PlayerState") : NULL; char txt[96], back[160] = "?";
    if (!ps || !tk_ptr_ok(*ps) || !CA->is_a(CA, *ps, "AdversarialPlayerState")) { logf_("bottrial: no adversarial player state - the stock score is not written (HUD text only)"); return; }
    btr_scores_text(kills, txt, sizeof txt); int ok = CA->set_property_text(CA, *ps, "m_Scores", txt), direct = 0;
    // ImportText can leave the element at its defaults ("((m_PlayerState=None,m_uiNbKills=1))" reads back "(())"), but the engine allocated it: the count is written
    // straight into element 0 (FPlayerKill: +0 player state, +8 uint32 m_uiNbKills; TArray at the property: data, num, max)
    struct { uint8_t *data; int num, max; } *arr = CA->prop_ptr(CA, *ps, "m_Scores");
    if (arr && arr->data && arr->num >= 1 && *(uint32_t *)(arr->data + 8) != (uint32_t)(kills < 0 ? 0 : kills)) { *(uint32_t *)(arr->data + 8) = (uint32_t)(kills < 0 ? 0 : kills); direct = 1; }
    CA->get_property_text(CA, *ps, "m_Scores", back, sizeof back);
    AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi);
    logf_("bottrial: player m_Scores = %s -> import %d, direct count write %d (%d element(s)), reads back %s; stock team scores now %d / %d%s", txt, ok, direct, arr ? arr->num : -1, back, mi.team_score[0], mi.team_score[1],
          mi.team_score[0] == kills ? "" : "  <-- team score differs");
}
static void tr_hud(void) { char s[160]; btr_score_text(&g_trs.m, g_trs.bot_name, s, sizeof s); CA->hud_score(CA, s); }
static void tr_remove_bot(const char *why) {
    if (g_trs.bot && tk_ptr_ok(g_trs.bot)) { logf_("bottrial: removing the bot %p (%s) -> %d", g_trs.bot, why, CA->npc_remove(CA, g_trs.bot)); }
    g_trs.bot = NULL;
}
static void tr_finish_travel(void) {
    CTrial *t = tr_cur(); CA->solo_duel(CA, g_trs.prev_solo); CA->hud_score(CA, "");
    if (t) logf_("bottrial: %s: travel back to %s -> %d (solo duel restored to %d)", g_trs.id, t->ret, CA->travel(CA, t->ret), g_trs.prev_solo);
}
static void tr_abort(const char *why) {
    logf_("bottrial: %s: ABORTED: %s", g_trs.id, why); CA->show_banner(CA, "Trial aborted", 4.f);
    tr_remove_bot("abort"); int back = g_trs.phase >= TP_WAIT_MATCH; tr_go(TP_ABORTED); g_trs.on = 0; g_trs.m.done = 1;
    if (back) tr_finish_travel(); else { CA->solo_duel(CA, g_trs.prev_solo); CA->hud_score(CA, ""); }
}
static void tr_on_death(void *victim, void *killer, int id, const char *attack, void *u) {
    (void)killer; (void)id; (void)u; if (!g_trs.on || g_trs.phase != TP_FIGHT || !victim) return;
    if (victim == g_trs.bot || victim == np_player(CA)) { g_trs.death_victim = victim; logf_("bottrial: on_death: %s died (%s)", victim == g_trs.bot ? "the bot" : "the player", attack ? attack : "?"); }
}
static void tr_on_attack(void *ch, int id, const char *attack, void *u) { (void)id; (void)attack; (void)u; if (g_trs.on && ch && ch == g_trs.bot) g_trs.attacks++; }

// approach (no navmesh): the fight AI walks with MoveTo / UNavigationSystem::MoveAlongSurface (ULockMoveBTTask), so in a duel arena without a navmesh it gets its
// target and the attack ticket but never moves (it stays at ~300 uu). AddMovementInput alone does not move a bot 245-360 uu away either. bottrial.h btr_approach:
//   RUN (> TR_LOCK_RANGE): AIController.MoveToActor(player): path-less, or pathfinding when the scene's runtime navmesh is ready, re-issued every
//     0.5 s from tr_tick because the behaviour tree may abort it; the bot runs like a player would, without a lock.
//   LOCK-MOVE (<= TR_LOCK_RANGE): SetTarget(player) once per round (the lock target the fight tree needs for Combat) + APawn::AddMovementInput each frame until TR_STOP.
// Attacks, defence and stance changes stay the AI's own. "approach_assist": off = none of this; on / auto = from the spawn on.
static float tr_dist(void *a_, void *b_) { float a[3], b[3]; if (!tk_ptr_ok(a_) || !tk_ptr_ok(b_) || !CA->test_get_location(CA, a_, a) || !CA->test_get_location(CA, b_, b)) return -1; return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])); }
static void tr_run_frame(void *p, float d);
static void tr_frame(float dt, void *u) {
    (void)dt; (void)u; if (!g_trs.on || g_trs.phase != TP_FIGHT || !g_trs.assist_on || !tk_ptr_ok(g_trs.bot)) return;
    void *p = np_player(CA); AbsCharInfo bi; float a[3], b[3]; if (!p || !tr_char(g_trs.bot, &bi)) return;
    if (!CA->test_get_location(CA, g_trs.bot, a) || !CA->test_get_location(CA, p, b)) return;
    float dx = b[0] - a[0], dy = b[1] - a[1], d = sqrtf(dx * dx + dy * dy);
    if (!g_trs.reached && d <= TR_STOP + 10.f) { double tr = tr_now() - g_trs.t_round; g_trs.reached = 1;
        logf_("bottrial: %s: APPROACH reached %.0f uu %.1f s after the round start (%.1f s after this spawn, spawned %.0f uu away%s; run requests %d ok %d, lock %s, input frames %d)%s", g_trs.id, d, tr, tr_now() - g_trs.t_spawn, g_trs.d_spawn,
              g_trs.stuck_n ? ", a re-spawn after a block" : "", g_trs.run_calls, g_trs.run_ok, g_trs.lock_set ? "set" : "not set", g_trs.assist_frames, tr <= 10 ? "  [within 10 s]" : "  <-- SLOWER than 10 s"); }
    int mode = btr_approach(d, bi.attacking, bi.is_dead, g_trs.nav_stall ? 900.f : TR_LOCK_RANGE, TR_STOP);   // a pathfinding run that stalls near the end hands over to lock-move from 900 uu
    if (mode == BTR_AP_RUN) { tr_run_frame(p, d); return; }
    if (g_trs.brain_off) {                                                                          // in range (or attacking / dead): the fight tree takes over again; stopping it clears the CanBeHitted flag (testkit.inc), so set it back
        g_trs.brain_off = 0; int r = CA->test_ai(CA, g_trs.bot, 1), h = CA->test_hittable(CA, g_trs.bot, 1);
        logf_("bottrial: %s: approach RUN over at %.0f uu (%.1f s after the spawn, %d request(s)): fight tree restarted -> %d, hittable -> %d", g_trs.id, d, tr_now() - g_trs.t_spawn, g_trs.run_calls, r, h);
    }
    if (mode != BTR_AP_LOCK) return;
    if (!g_trs.lock_set) { g_trs.lock_set = 1; void *f = CA->find_function(CA, g_trs.bot, "SetTarget"); void *args[1] = { &p }; int st = f && CA->call_function_params(CA, g_trs.bot, f, args, NULL);
        logf_("bottrial: %s: approach LOCK-MOVE at %.0f uu (%.1f s after the spawn): SetTarget(player) -> %d", g_trs.id, d, tr_now() - g_trs.t_spawn, st); }
    static void *fn; if (!fn) fn = CA->find_function(CA, g_trs.bot, "AddMovementInput"); if (!fn) return;
    float dir[3] = { dx / d, dy / d, 0 }, scale = 1.f; uint8_t force = 1; void *args[3] = { dir, &scale, &force };
    if (CA->call_function_params(CA, g_trs.bot, fn, args, NULL)) g_trs.assist_frames++;
}
static void tr_run_frame(void *p, float d) {                                                        // RUN: MoveToActor on the bot's AI controller; re-issued whenever the controller is not moving
    void **cp = (void **)CA->prop_ptr(CA, g_trs.bot, "Controller"), *c = cp && tk_ptr_ok(*cp) ? *cp : NULL;
    if (!c || !CA->is_a(CA, c, "AIController")) { if (g_trs.run_calls++ < 2) logf_("bottrial: %s: approach RUN: the bot has no AIController (%p)", g_trs.id, c); return; }
    static void *fn, *fs; if (!fn) fn = CA->find_function(CA, c, "MoveToActor"); if (!fs) fs = CA->find_function(CA, c, "GetMoveStatus");
    if (!fn) { if (g_trs.run_calls++ < 2) logf_("bottrial: %s: approach RUN: AIController.MoveToActor not found", g_trs.id); return; }
    uint8_t ms = 0xFF; if (fs) CA->call_function_params(CA, c, fs, NULL, &ms);                     // EPathFollowingStatus: 0 Idle, 1 Waiting, 2 Paused, 3 Moving
    int nav = ms_nav_ready();                                                                       // the scene's runtime navmesh (mapscript.inc): real paths, no detours
    {   // the first bot of a world can stall ~560 uu out at the last path corner with m_bAlertedOnce 1 (later bots, alerted 0, do not): keep it 0 while running in
        void *aic = se_comp(CA, g_trs.bot, "m_AIComponent"); int al = -1;
        if (aic && CA->get_bool(CA, aic, "m_bAlertedOnce", &al) && al == 1) { CA->set_bool(CA, aic, "m_bAlertedOnce", 0); if (g_trs.alert_resets++ < 3) logf_("bottrial: %s: approach RUN at %.0f uu: m_bAlertedOnce was 1 - set to 0 for the run-in (reset %d)", g_trs.id, d, g_trs.alert_resets); }
    }
    if (tr_now() - g_trs.t_run >= 1.0) {                                                            // once a second: is the request alive, how fast does the bot go
        g_trs.t_run = tr_now(); void *mc = se_comp(CA, g_trs.bot, "CharacterMovement"); char v[96] = "?", mst[48] = "?"; uint8_t ss = 0xFF;
        if (mc) { void *f = CA->find_function(CA, mc, "BPF_GetCurrentSpeedState"); if (f) CA->call_function_params(CA, mc, f, NULL, &ss); CA->get_property_text(CA, mc, "Velocity", v, sizeof v); CA->get_property_text(CA, mc, "m_eMoveStatus", mst, sizeof mst); }
        logf_("bottrial: %s: approach RUN at %.0f uu (%.1f s): path status %d, %d request(s) (%d accepted), velocity %s, move status %s, speed state %d", g_trs.id, d, tr_now() - g_trs.t_spawn, ms, g_trs.run_calls, g_trs.run_ok, v, mst, ss);
    }
    if (!g_trs.brain_off) {
        g_trs.brain_off = 1; logf_("bottrial: %s: approach RUN: the fight tree is paused while the bot runs in (its MoveTo / StopMovement cancels every request: status 0 each frame, ~80 uu/s) -> %d", g_trs.id, CA->test_ai(CA, g_trs.bot, 0));
        void *f = CA->find_function(CA, g_trs.bot, "SetTarget"), *none = NULL, *a1[1] = { &none }; int st = f && CA->call_function_params(CA, g_trs.bot, f, a1, NULL);
        logf_("bottrial: %s: approach RUN: lock released for the run (SetTarget(None) -> %d; a locked bot only strafe-walks, ~88 uu/s)", g_trs.id, st);
    }
    { void *mc = se_comp(CA, g_trs.bot, "CharacterMovement"), *f = mc ? CA->find_function(CA, mc, "BPF_SetMoveStatus") : NULL; uint8_t ex = 0; void *a1[1] = { &ex }; if (f) CA->call_function_params(CA, mc, f, a1, NULL); }   // Exploration = free run, re-asserted each frame (Combat = strafe walk)
    if (g_trs.run_best < 0 || d < g_trs.run_best - 20.f) { g_trs.run_best = d; g_trs.t_run_best = tr_now(); }
    {   // blocked = the bot ITSELF does not move (its progress mark), not "the distance did not shrink" (a retreating player): bottrial.h btr_block_respawn
        float bl[3], mv = -1; if (CA->test_get_location(CA, g_trs.bot, bl)) { mv = sqrtf((bl[0] - g_trs.prog_loc[0]) * (bl[0] - g_trs.prog_loc[0]) + (bl[1] - g_trs.prog_loc[1]) * (bl[1] - g_trs.prog_loc[1])); if (mv >= BTR_STILL_UU) { memcpy(g_trs.prog_loc, bl, sizeof bl); g_trs.t_prog = tr_now(); g_trs.still_logged = 0; mv = 0; } }
        double still = tr_now() - g_trs.t_prog;
        if (!g_trs.run_blocked && btr_block_respawn(g_trs.respawned, g_trs.lock_set, mv, still, d)) { g_trs.run_blocked = 1; logf_("bottrial: %s: approach RUN BLOCKED at %.0f uu: the bot has not moved %.0f uu for %.1f s on its run-in from the spawn (a closed gate / furniture?)", g_trs.id, d, BTR_STILL_UU, still); }
        else if (!g_trs.run_blocked && !g_trs.still_logged && mv >= 0 && still >= BTR_STILL_S && tr_now() - g_trs.t_run_best > BTR_STILL_S) { g_trs.still_logged = 1;
            logf_("bottrial: %s: approach RUN: no progress at %.0f uu (bot moved %.0f uu in %.1f s; re-spawn used this round %d, engaged %d) - NO re-spawn, the bot keeps chasing", g_trs.id, d, mv, still, g_trs.respawned, g_trs.lock_set); }
    }
    // stall rule (a stuck AI can stand still while its run-in requests are still "accepted"): a bot that has not moved for 40 s on its run-in and never engaged ends the trial
    // instead of holding the player in a dead arena (tr_abort: banner, bot removed, travel back); a hang becomes a clear ABORTED line in the log.
    // first try to recover: after 6 s the stuck bot is replaced by a fresh one at the game mode's start pick (at most twice per round; never the player).
    {   double still_s = tr_now() - g_trs.t_prog;
        if (!g_trs.lock_set && still_s >= 6.0 && g_trs.stall_replaced < 2) { g_trs.stall_replaced++; logf_("bottrial: %s: STALL: the bot has not moved for %.0f s on its run-in (stuck AI, run requests %d ok %d) - replaced by a fresh bot (%d of 2 this round)", g_trs.id, still_s, g_trs.run_calls, g_trs.run_ok, g_trs.stall_replaced); tr_remove_bot("stalled run-in"); tr_go(TP_SPAWN); return; }
        if (!g_trs.lock_set && still_s >= 40.0) { tr_abort("the bot has not moved for 40 s on its run-in (stuck AI)"); return; } }
    if (nav && !g_trs.nav_stall && d < 900.f && tr_now() - g_trs.t_prog >= 2.0) { g_trs.nav_stall = 1; logf_("bottrial: %s: approach RUN (navmesh) stalled at %.0f uu for 2 s (path status %d) - lock-move takes over from 900 uu", g_trs.id, d, ms); }
    if (!nav) {   // detour (only without a navmesh): the straight path-less run can hit furniture (e.g. a custom arena where start A -> start B is blocked). Like a player walking around:
        // when the bot itself has not moved 60 uu for 1.5 s, it runs to an untried player start (4 s) as a waypoint, or, every third detour / with none usable,
        // 2 s to a point 450 uu aside and 150 uu ahead (sides alternate), then chases again. Movement only (MoveToLocation, no pathfinding) - never a teleport.
        float bl[3]; double now = tr_now();
        if (g_trs.detour_active && now >= g_trs.detour_until) { g_trs.detour_active = 0; ms = 0; }   // the detour's MoveToLocation is still "moving": issue the chase again now
        if (now < g_trs.detour_until && CA->test_get_location(CA, g_trs.bot, bl) && (bl[0] - g_trs.detour_to[0]) * (bl[0] - g_trs.detour_to[0]) + (bl[1] - g_trs.detour_to[1]) * (bl[1] - g_trs.detour_to[1]) < 120.f * 120.f) {
            g_trs.detour_until = now; g_trs.detour_active = 0; ms = 0; g_trs.t_prog = now; memcpy(g_trs.prog_loc, bl, sizeof bl); logf_("bottrial: %s: approach DETOUR: waypoint reached - chasing again", g_trs.id); }
        if (now < g_trs.detour_until) {
            static void *fl; if (!fl) fl = CA->find_function(CA, c, "MoveToLocation");
            if (fl && ms != 3) { float acc = 40.f; uint8_t so = 0, pf = 0, pj = 0, cs = 0, pa = 1, res = 0xFF; void *filt = NULL; void *a[8] = { g_trs.detour_to, &acc, &so, &pf, &pj, &cs, &filt, &pa }; int ok = CA->call_function_params(CA, c, fl, a, &res);
                static int said; if (said++ < 6) logf_("bottrial: %s: approach DETOUR: MoveToLocation(%.0f %.0f) -> call %d, result %d, path status %d", g_trs.id, g_trs.detour_to[0], g_trs.detour_to[1], ok, res, ms); }
            return;
        }
        if (CA->test_get_location(CA, g_trs.bot, bl) && now - g_trs.t_prog >= BTR_STILL_S && now - g_trs.t_detour_end >= 1.0) {
            float pl[3]; if (CA->test_get_location(CA, p, pl)) {
                float dx = pl[0] - bl[0], dy = pl[1] - bl[1], dd = sqrtf(dx * dx + dy * dy); if (dd < 1.f) dd = 1.f; dx /= dd; dy /= dd;
                int n = g_trs.detours++, wi = -1; float best = 1e30f;
                // first choice: the arena's player starts are known-walkable open spots: the untried one with the shortest bot -> start -> player route (a waypoint)
                if (g_trs.wp_tried == (1u << g_trs.st_n) - 1u) g_trs.wp_tried = 0;
                for (int i = 0; i < g_trs.st_n && i < 16; i++) { const float *s = g_trs.st_loc[i]; float a = sqrtf((s[0] - bl[0]) * (s[0] - bl[0]) + (s[1] - bl[1]) * (s[1] - bl[1])), b = sqrtf((s[0] - pl[0]) * (s[0] - pl[0]) + (s[1] - pl[1]) * (s[1] - pl[1]));
                    if ((g_trs.wp_tried >> i) & 1u || a < 250.f || b < 150.f) continue; if (a + b < best) { best = a + b; wi = i; } }
                if (wi >= 0 && n % 3 != 2) { g_trs.wp_tried |= 1u << wi; memcpy(g_trs.detour_to, g_trs.st_loc[wi], sizeof g_trs.detour_to); g_trs.detour_until = now + 4.0; }
                else { float side = (n % 2) ? -1.f : 1.f, sw = 450.f; g_trs.detour_to[0] = bl[0] - dy * side * sw + dx * 150.f; g_trs.detour_to[1] = bl[1] + dx * side * sw + dy * 150.f; g_trs.detour_to[2] = bl[2]; g_trs.detour_until = now + 2.0; wi = -1; }
                g_trs.detour_active = 1; g_trs.t_detour_end = g_trs.detour_until; g_trs.t_prog = g_trs.detour_until; memcpy(g_trs.prog_loc, bl, sizeof bl);
                logf_("bottrial: %s: approach DETOUR %d: the bot is stuck at %.0f %.0f, %.0f uu from the player - runs to %.0f %.0f (%s) for %.0f s, then chases again (no teleport)", g_trs.id, n + 1, bl[0], bl[1], d,
                      g_trs.detour_to[0], g_trs.detour_to[1], wi >= 0 ? "a player start as waypoint" : "450 uu aside", g_trs.detour_until - now);
                return;
            }
        }
    }
    if (ms == 3) return;
    if (g_trs.run_calls > 0 && tr_now() - g_trs.t_stop > 0.5) {                                     // the request died: the fight tree started after the first StopLogic (the bot then crawls at ~80 uu/s) - stop it again
        g_trs.t_stop = tr_now(); int r = CA->test_ai(CA, g_trs.bot, 0); if (g_trs.n_stops++ < 3) logf_("bottrial: %s: approach RUN: request cancelled (path status %d) - fight tree stopped again -> %d", g_trs.id, ms, r);
    }
    float acc = TR_STOP - 15.f; uint8_t pathfind = nav ? 1 : 0, stop_overlap = pathfind ? 0 : 1, strafe = 0, partial = 1, res = 0xFF;   /* with pathfinding stop-on-overlap must be off: it halts ~570 uu out */ void *filter = NULL;
    void *args[7] = { &p, &acc, &stop_overlap, &pathfind, &strafe, &filter, &partial }; int ok = CA->call_function_params(CA, c, fn, args, &res);
    g_trs.run_calls++; if (ok && res == 2) g_trs.run_ok++;                                          // EPathFollowingRequestResult: 0 Failed, 1 AlreadyAtGoal, 2 RequestSuccessful
    if (g_trs.run_calls <= 2) logf_("bottrial: %s: approach RUN at %.0f uu: MoveToActor(player, %.0f, %s) -> call %d, result %d (%s)", g_trs.id, d, acc, nav ? "PATHFINDING on the runtime navmesh, stop-on-overlap off" : "no pathfinding (no navmesh)", ok, res, res == 2 ? "moving" : res == 1 ? "already at goal" : res == 0 ? "FAILED" : "?");
    if (res == 0 && g_trs.run_calls <= 3) { float bl[3] = { 0, 0, 0 }, pl[3] = { 0, 0, 0 }; CA->test_get_location(CA, g_trs.bot, bl); CA->test_get_location(CA, p, pl); void **cp2 = (void **)CA->prop_ptr(CA, g_trs.bot, "Controller");
        logf_("bottrial: %s: approach RUN FAILED: bot at %.0f %.0f %.0f, player at %.0f %.0f %.0f, controller field %p, AI controller used %p, bot %p", g_trs.id, bl[0], bl[1], bl[2], pl[0], pl[1], pl[2], cp2 ? *cp2 : NULL, c, g_trs.bot); }
}
// hit bookkeeping (stagger check): every impact on the bot during a fight is logged with the bot's attack state; 0.3 s later the attack is either gone (INTERRUPTED: the
// hit staggered it) or still the same one (HIT THROUGH: resilience / steady hand kept it going). The live resilience (DefenseComponent m_iResilience) is logged with each check.
static void tr_on_hit(const AbsHitInfo *h, void *u) {
    (void)u; if (!g_trs.on || g_trs.phase != TP_FIGHT || !h || h->victim != g_trs.bot || !tk_ptr_ok(g_trs.bot)) return;
    AbsCharInfo bi; int att = tr_char(g_trs.bot, &bi) ? bi.attacking : -1;
    static const char *const on[] = { "hit", "guarded", "guard_broken", "absorbed", "parried", "dodged", "no_effect" };
    logf_("bottrial: %s: HIT on the bot: %s (%s), damage %.0f, the bot was %s", g_trs.id, h->attack, h->outcome >= 0 && h->outcome <= 6 ? on[h->outcome] : "?", h->damage, att > 0 ? "ATTACKING" : att == 0 ? "not attacking" : "?");
    if (h->outcome != ABS_HIT && h->outcome != ABS_HIT_GUARD_BROKEN) return;
    if (att <= 0) { g_trs.hits_idle++; return; }
    g_trs.hit_pending = 1; g_trs.hit_t = tr_now(); g_trs.hit_was_attacking = att; g_trs.hit_attacks_at = g_trs.attacks; snprintf(g_trs.hit_attack, sizeof g_trs.hit_attack, "%s", h->attack);
}
static void tr_hit_check(void) {
    if (!g_trs.hit_pending || tr_now() - g_trs.hit_t < 0.3) return; g_trs.hit_pending = 0;
    AbsCharInfo bi; if (!tr_char(g_trs.bot, &bi)) return;
    char jh[16] = "?", rs[16] = "?"; CA->get_property_text(CA, g_trs.bot, "m_bHasJustBeenHitted", jh, sizeof jh);
    void *dc = se_comp(CA, g_trs.bot, "m_DefenseComponent"); if (dc) CA->get_property_text(CA, dc, "m_iResilience", rs, sizeof rs);
    int same = bi.attacking > 0 && g_trs.attacks == g_trs.hit_attacks_at;                            // still attacking and no new attack started since the hit
    if (same) g_trs.hits_through++; else g_trs.hits_interrupt++;
    logf_("bottrial: %s: STAGGER check 0.3 s after %s hit the attacking bot: %s (attacking now %d, new attacks %d, m_bHasJustBeenHitted %s, live resilience %s); this round interrupted %d, hit through %d, hits on an idle bot %d",
          g_trs.id, g_trs.hit_attack, same ? "HIT THROUGH (not staggered)" : "INTERRUPTED (staggered)", bi.attacking, g_trs.attacks - g_trs.hit_attacks_at, jh, rs, g_trs.hits_interrupt, g_trs.hits_through, g_trs.hits_idle);
}
// the bot's spawn point like a second player: the arena's player starts (Respawn_PVP_C: PlayerStart BP, m_bForFirstSpawn on one start per team), scanned once per trial
static char g_tr_scan_world[160];
static void tr_scan_cb(void *obj, void *u) {
    (void)u; if (g_trs.st_n >= 16 || !tk_ptr_ok(obj) || !CA->is_a(CA, obj, "PlayerStart")) return;
    char nm[128] = "", path[400] = "", fs[16] = ""; CA->object_name(CA, obj, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) return;
    se_path_of(obj, path, sizeof path); if (!g_tr_scan_world[0] || !strstr(path, g_tr_scan_world)) return;      // only the current arena's actors
    float l[3]; if (!CA->test_get_location(CA, obj, l)) return;
    CA->get_property_text(CA, obj, "m_bForFirstSpawn", fs, sizeof fs);
    int i = g_trs.st_n++; memcpy(g_trs.st_loc[i], l, sizeof l); g_trs.st_first[i] = !_stricmp(fs, "True");
    logf_("bottrial: player start %d: %s at %.0f %.0f %.0f, first spawn %s", i, nm, l[0], l[1], l[2], fs[0] ? fs : "(no such property)");
}
static void tr_scan_starts(void) {
    if (g_trs.st_scanned) return; g_trs.st_scanned = 1; g_trs.st_n = 0; tr_world_name(g_tr_scan_world, sizeof g_tr_scan_world);
    double t0 = tr_now(); int n = CA->for_each_object(CA, tr_scan_cb, NULL);
    logf_("bottrial: %s: %d player start(s) in %s (%d objects scanned in %.2f s)", g_trs.id, g_trs.st_n, g_tr_scan_world, n, tr_now() - t0);
}
// ================================================================ the bot as a SECOND PLAYER (see also docs\PLUGIN-API.md "Bot trials")
// * Score: AFightingCharacter::ServerKilledBy -> AThePlainesGameMode::NotifyKill -> AAdversarialGameMode::Killed -> AThePlainesGameMode::Killed: killer controller's
//   AFightingPlayerState::ScoreKill(victim's player state) (AAdversarialPlayerState adds an FPlayerKill to m_Scores). AAdversarialGameState::BPF_GetTeamScore sums them over
//   GameState.PlayerArray filtered by m_iTeamNumber. An AI controller has no player state, so an NPC never scored: the bot gets one (AController::InitPlayerState = the game
//   mode's PlayerStateClass, which joins PlayerArray by itself) on team 1 -> both kills go through the stock code and the stock HUD counts both players.
//   AAdversarialGameMode::IsMatchOver only looks at the PLAYER controllers' scores (GameState +0x638): the game mode's m_iNbWinningRounds is set to 99 so the stock post-match never
//   starts (the trial ends itself and travels back); the game state keeps rounds_to_win for the HUD diamonds.
// * Intro: APAdversarialTeamIntro (client) calls AFightingPlayerController::BPF_BuildAdversarialTeamInfos (every AFightingCharacter whose PlayerState is an
//   AFightingPlayerState, by team) and plays one AAdversarialStartGameScene per team that has players, team 0 first. The bot (pawn + player state) is made while team 0's scene
//   plays, the infos are rebuilt (UFunction) and the bot's entry gets its intro row (m_intro1v1Name) -> team 1's scene plays the bot's intro like a second player's.
//   (Made before the intro instead, AAdversarialGameState::ArePlayersInitialized would count a character with a player state that has no player controller.)
// * Respawn: like a player's, GameState.m_fDeathRespawnTime after the death at the start the game mode picks for that controller (GameModeBase.FindPlayerStart ->
//   AThePlainesGameMode::ChoosePlayerStart, the 1v1 spawning component); the player state moves to the new pawn, so the bot's score persists for the match.
typedef struct { uint8_t *data; int num, max; } TrArr;
// used by lannative.c hk_scorekill ("pvp score"): it drops every kill whose victim player state has no PLAYER controller, except the trial bot's player state
// (`skip = !L_ps_is_player(victim) && !btr_trial_bot_ps(victim)`), so the stock ScoreKill counts the player's kill of the bot and the m_Scores fallback stays idle.
static int btr_trial_bot_ps(void *ps) { return ps && g_trs.on && ps == g_trs.ps; }
static void *tr_ctrl(void *ch) { void **cp = tk_ptr_ok(ch) ? (void **)CA->prop_ptr(CA, ch, "Controller") : NULL; return cp && tk_ptr_ok(*cp) ? *cp : NULL; }
static int tr_ps_team(void *ps) { int t = -1; void *f = tk_ptr_ok(ps) ? CA->find_function(CA, ps, "BPF_GetTeam") : NULL; if (f) CA->call_function_params(CA, ps, f, NULL, &t); return t; }
static int tr_player_array(void) { AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi); TrArr *pa = tk_ptr_ok(mi.game_state) ? CA->prop_ptr(CA, mi.game_state, "PlayerArray") : NULL; return pa ? pa->num : -1; }
static void tr_ps_set_team(int team) { void *f = CA->find_function(CA, g_trs.ps, "BPF_SetTeam"); void *a[1] = { &team }; if (f) CA->call_function_params(CA, g_trs.ps, f, a, NULL); }
static void tr_ps_attach(void *bot, const char *why) {
    void *ctl = tr_ctrl(bot), **cps = ctl ? (void **)CA->prop_ptr(CA, ctl, "PlayerState") : NULL, **pps = tk_ptr_ok(bot) ? (void **)CA->prop_ptr(CA, bot, "PlayerState") : NULL;
    if (!ctl || !cps || !pps) { logf_("bottrial: %s: player state (%s): the bot %p has no controller / PlayerState field - the stock score will not count it", g_trs.id, why, bot); return; }
    int made = 0; g_trs.bot_ctl = ctl;
    __try {
        if (!g_trs.ps || !tk_ptr_ok(g_trs.ps) || !CA->is_a(CA, g_trs.ps, "AdversarialPlayerState")) {
            if (g_trs.ps) logf_("bottrial: %s: WARNING the bot's player state %p is gone - a new one is made (its earlier stock points are lost)", g_trs.id, g_trs.ps);
            // AController::InitPlayerState is a virtual (vtable +0x748: AAIController::PostInitializeComponents calls it there when bWantsPlayerState); it is not in
            // symbols.bin (a call in its prologue makes it unhookable), so it is called through the controller's own vtable; the target must lie in the exe image
            void (*initps)(void *) = (void (*)(void *))(*(void ***)ctl)[0x748 / 8]; uintptr_t ip = (uintptr_t)initps;
            logf_("bottrial: %s: InitPlayerState via vtable +0x748 of %p -> exe+0x%llX (AController::InitPlayerState is exe+0x13F5600 in b1.25_575)", g_trs.id, ctl, (unsigned long long)(ip - (uintptr_t)g_base));
            if (ip - (uintptr_t)g_base != 0x13F5600) { logf_("bottrial: %s: the vtable entry is not AController::InitPlayerState of this build - refused, no player state for the bot", g_trs.id); g_trs.ps = NULL; return; }
            if (!*cps) initps(ctl);
            g_trs.ps = *cps; made = 1;
            if (!tk_ptr_ok(g_trs.ps) || !CA->is_a(CA, g_trs.ps, "AdversarialPlayerState")) { char cn[96] = "?"; if (tk_ptr_ok(g_trs.ps)) CA->class_name(CA, CA->object_class(CA, g_trs.ps), cn, sizeof cn);
                logf_("bottrial: %s: InitPlayerState gave %p (%s) - not an AdversarialPlayerState: the stock score will not count the bot", g_trs.id, g_trs.ps, cn); g_trs.ps = NULL; return; }
            tr_ps_set_team(1);
            logf_("bottrial: %s: player state %p made for the bot (InitPlayerState on its AI controller %p): team -> %d, PlayerName '%s' -> %d", g_trs.id, g_trs.ps, ctl, tr_ps_team(g_trs.ps), g_trs.bot_name,
                  CA->set_property_text(CA, g_trs.ps, "PlayerName", g_trs.bot_name));
        } else {
            *cps = g_trs.ps; void *f = CA->find_function(CA, g_trs.ps, "SetOwner"); void *a[1] = { &ctl }; if (f) CA->call_function_params(CA, g_trs.ps, f, a, NULL);
        }
        *pps = g_trs.ps;
        if (!made) tr_ps_set_team(1);                                                       // BPF_SetTeam recomputes the characters' relationships (the new pawn joins team 1)
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bottrial: %s: player state (%s): EXCEPTION 0x%08lX", g_trs.id, why, GetExceptionCode()); return; }
    void *pp = np_player(CA), **pps2 = pp ? (void **)CA->prop_ptr(CA, pp, "PlayerState") : NULL; void *pps_ = pps2 ? *pps2 : NULL; char sc[200] = "?"; CA->get_property_text(CA, g_trs.ps, "m_Scores", sc, sizeof sc);
    logf_("bottrial: %s: player state (%s): bot %p -> %p (%s, team %d, m_Scores %s); the player's player state %p team %d; GameState PlayerArray %d", g_trs.id, why, bot, g_trs.ps, made ? "new" : "moved to this pawn",
          tr_ps_team(g_trs.ps), sc, pps_, tr_ps_team(pps_), tr_player_array());
}
static void tr_ps_detach(void *bot, const char *why) {                                   // before a bot pawn is removed: AController::Destroyed would destroy a player state it still holds
    void *ctl = tr_ctrl(bot); if (!ctl && tk_ptr_ok(g_trs.bot_ctl) && CA->is_a(CA, g_trs.bot_ctl, "Controller")) ctl = g_trs.bot_ctl;   // a dead bot was unpossessed: its old controller still holds the player state
    void **cps = ctl ? (void **)CA->prop_ptr(CA, ctl, "PlayerState") : NULL, **pps = tk_ptr_ok(bot) ? (void **)CA->prop_ptr(CA, bot, "PlayerState") : NULL; int a = 0, b = 0;
    if (cps && *cps == g_trs.ps && g_trs.ps) { *cps = NULL; a = 1; }
    if (pps && *pps == g_trs.ps && g_trs.ps) { *pps = NULL; b = 1; }
    logf_("bottrial: %s: player state %p detached from the bot %p (%s): controller %d, pawn %d (kept for the next pawn)", g_trs.id, g_trs.ps, bot, why, a, b);
}
// the arena's AAdversarialStartGameScene actors (one per team), scanned once per trial; m_Dummies fills when a team's scene starts
static void tr_scene_cb(void *obj, void *u) {
    (void)u; if (g_trs.n_scenes >= 4 || !tk_ptr_ok(obj) || !CA->is_a(CA, obj, "AdversarialStartGameScene")) return;
    char nm[128] = "", path[400] = ""; CA->object_name(CA, obj, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) return;
    se_path_of(obj, path, sizeof path); if (!g_tr_scan_world[0] || !strstr(path, g_tr_scan_world)) return;
    g_trs.scenes[g_trs.n_scenes++] = obj; int *ti = CA->prop_ptr(CA, obj, "m_iTeamIndex"); logf_("bottrial: intro scene %d: %s team %d", g_trs.n_scenes - 1, nm, ti ? *ti : -1);
}
static int tr_scene_dummies(int team) {
    for (int i = 0; i < g_trs.n_scenes; i++) { int *ti = tk_ptr_ok(g_trs.scenes[i]) ? CA->prop_ptr(CA, g_trs.scenes[i], "m_iTeamIndex") : NULL; TrArr *d = ti ? CA->prop_ptr(CA, g_trs.scenes[i], "m_Dummies") : NULL; if (ti && *ti == team && d) return d->num; }
    return -1;
}
// the local controller's m_AdversarialTeamInfos (FAdversarialTeamInfos 0x18: team byte, TArray<FAdversarialPlayerInfos 0xA0>: +0 name FString, +0x10 sex, +0x18 outfits,
// +0x28 style, +0x2C local, +0x30 level, +0x38 FName m_intro1v1Name): rebuild (UFunction), set the bot's intro row, log every entry
static void tr_team_infos(const char *why, int rebuild) {
    void *pc = CA->get_local_player_controller(CA); TrArr *ti = pc ? CA->prop_ptr(CA, pc, "m_AdversarialTeamInfos") : NULL;
    if (!ti) { logf_("bottrial: %s: team infos (%s): no local player controller / m_AdversarialTeamInfos", g_trs.id, why); return; }
    if (rebuild) { void *f = CA->find_function(CA, pc, "BPF_BuildAdversarialTeamInfos"); int r = f && CA->call_function_params(CA, pc, f, NULL, NULL); g_trs.rebuilds++; logf_("bottrial: %s: team infos rebuilt (%s) -> %d", g_trs.id, why, r); }
    int b = -1; for (int i = 0; i < g_bt_n; i++) if (g_trs.bot && g_bt[i].arch && g_bt[i].arch == CA->npc_archetype(CA, g_trs.bot)) b = i;
    for (int t = 0; t < ti->num && t < 4; t++) {
        uint8_t *e = ti->data + t * 0x18; TrArr *pl = (TrArr *)(e + 8);
        for (int k = 0; k < pl->num && k < 4; k++) {
            uint8_t *p = pl->data + k * 0xA0; char name[64] = "", intro[48] = "?"; wchar_t *wn = *(wchar_t **)p; int wl = *(int *)(p + 8);
            if (wn && wl > 0) WideCharToMultiByte(CP_UTF8, 0, wn, -1, name, sizeof name, NULL, NULL);
            { CTrial *ct = tr_cur(); if (rebuild && e[0] == 0 && p[0x2C] && ct && ct->player_intro[0]) { uint64_t fn = 0; if (CA->fname_make(CA, ct->player_intro, &fn) == 0) { *(uint64_t *)(p + 0x38) = fn; logf_("bottrial: %s: team 0 player %d (local): intro row -> '%s' (trial \"player_intro\")", g_trs.id, k, ct->player_intro); } } }
            if (rebuild && e[0] == 1 && b >= 0 && g_bt[b].intro[0]) { uint64_t fn = 0; if (CA->fname_make(CA, g_bt[b].intro, &fn) == 0) { *(uint64_t *)(p + 0x38) = fn; if (*(int *)(p + 0x30) <= 0) *(int *)(p + 0x30) = 60; logf_("bottrial: %s: team 1 player %d: intro row -> '%s' (bots entry \"intro\")", g_trs.id, k, g_bt[b].intro); } }
            CA->fname_text(CA, p + 0x38, intro, sizeof intro);
            logf_("bottrial: %s: team infos (%s): team %d player %d '%s' sex %d skin %d outfits %d style %d local %d level %d intro '%s'", g_trs.id, why, e[0], k, name, p[0x10], p[0x11], ((TrArr *)(p + 0x18))->num, *(int *)(p + 0x28), p[0x2C], *(int *)(p + 0x30), intro);
        }
        if (!pl->num) logf_("bottrial: %s: team infos (%s): team %d has no players", g_trs.id, why, e[0]);
    }
}
// the start the game mode picks for the bot's controller (GameModeBase.FindPlayerStart(controller, "") -> ChoosePlayerStart); 1 = loc / yaw filled
static int tr_find_start(void *bot, float loc[3], float *yaw) {
    AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi); void *ctl = tr_ctrl(bot), *gm = mi.game_mode, *st = NULL;
    if (!ctl) {   // a dead AI is unpossessed at once; FindPlayerStart with its pawn-less controller faults inside the game (exe+0x397B30): use the 1v1 rule instead,
                  // the start farthest from the opponent (the game mode's FarthestSpawningComponent)
        void *p = np_player(CA); float pl[3]; int none[16] = { 0 }; tr_scan_starts();
        int si = p && CA->test_get_location(CA, p, pl) ? btr_pick_start((const float (*)[3])g_trs.st_loc, none, g_trs.st_n, pl, 300.f) : -1;
        if (si < 0) { logf_("bottrial: %s: respawn start: no controller and no start 300 uu from the player", g_trs.id); return 0; }
        memcpy(loc, g_trs.st_loc[si], sizeof g_trs.st_loc[si]); *yaw = atan2f(pl[1] - loc[1], pl[0] - loc[0]) * 57.29578f;
        logf_("bottrial: %s: respawn start: the dead bot is unpossessed - the start farthest from the player (player start %d at %.0f %.0f %.0f, the 1v1 farthest-spawn rule)", g_trs.id, si, loc[0], loc[1], loc[2]); return 1;
    }
    if (!tk_ptr_ok(gm)) { logf_("bottrial: %s: FindPlayerStart: no game mode", g_trs.id); return 0; }
    void *f = CA->find_function(CA, gm, "FindPlayerStart"); struct { void *d; int n, m; } name = { NULL, 0, 0 }; void *a[2] = { &ctl, &name };
    if (!f || !CA->call_function_params(CA, gm, f, a, &st) || !tk_ptr_ok(st) || !CA->test_get_location(CA, st, loc)) { logf_("bottrial: %s: FindPlayerStart(bot controller) -> %p: no start", g_trs.id, st); return 0; }
    char nm[96] = "?"; CA->object_name(CA, st, nm, sizeof nm); float fw[3]; *yaw = CA->test_get_forward(CA, st, fw) ? atan2f(fw[1], fw[0]) * 57.29578f : 0.f;
    logf_("bottrial: %s: FindPlayerStart(bot controller) -> %s at %.0f %.0f %.0f (the game mode's own pick for a respawning player)", g_trs.id, nm, loc[0], loc[1], loc[2]); return 1;
}
static float tr_gs_float(const char *prop, float dflt) { AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi); char v[32] = ""; if (!tk_ptr_ok(mi.game_state) || CA->get_property_text(CA, mi.game_state, prop, v, sizeof v) <= 0) return dflt; return (float)atof(v); }
// a bot spawned during the intro would stand next to its own intro dummy (two bots in team 1's scene): hidden until the intro ends
static int g_tr_hidden;
static void tr_hide(int hide) { if (!tk_ptr_ok(g_trs.bot)) return; void *f = CA->find_function(CA, g_trs.bot, "SetActorHiddenInGame"); uint8_t h = (uint8_t)(hide != 0); void *a[1] = { &h };
    int ok = f && CA->call_function_params(CA, g_trs.bot, f, a, NULL); g_tr_hidden = hide && ok; logf_("bottrial: %s: bot %s during the intro -> %d", g_trs.id, hide ? "hidden" : "shown", ok); }
static void tr_bot_ai(int on, const char *why) {                                         // the fight tree off while the intro / countdown runs, on for the fight
    if (!tk_ptr_ok(g_trs.bot)) return; int r = CA->test_ai(CA, g_trs.bot, on), h = on ? CA->test_hittable(CA, g_trs.bot, 1) : -1; g_trs.ai_paused = !on;
    logf_("bottrial: %s: bot AI %s (%s) -> %d%s", g_trs.id, on ? "ON" : "paused", why, r, on ? (h ? ", hittable" : ", hittable FAILED") : "");
}
static void tr_spawn(void) {
    CTrial *t = tr_cur(); const char *bid = t->bots[g_trs.m.stage]; int def = tr_np_def(bid);
    void *p = np_player(CA); AbsCharInfo pi;
    if (!p || !tr_char(p, &pi)) return;                                                            // no pawn yet (respawning): wait
    if (pi.is_dead || pi.health <= 0) { if (tr_since() > 20) { tr_abort("the player did not respawn within 20 s"); } return; }
    int resp = g_trs.resp_ok; g_trs.resp_ok = 0;                                                    // a player-like respawn at the game mode's pick (TP_BOT_DOWN waited m_fDeathRespawnTime)
    if (!g_trs.early && !resp && tr_since() < (g_trs.m.tp + g_trs.m.tb == 0 ? 0.25 : t->gap)) { g_trs.resp_ok = resp; return; }   // round 1: as the match goes InProgress (the intro ran with the arena empty)
    if (def < 0) { tr_abort("the bot entry is gone"); return; }
    if (!t->player_like) CA->test_heal(CA, p);                                                      // a player-like duel heals nobody between kills
    float pl[3], fw[3]; if (!CA->test_get_location(CA, p, pl) || !CA->test_get_forward(CA, p, fw)) { g_trs.resp_ok = resp; return; }
    float h = sqrtf(fw[0] * fw[0] + fw[1] * fw[1]); if (h < 1e-3f) { fw[0] = 1; fw[1] = 0; h = 1; }
    float dist = g_trs.near_spawn ? 150.f : t->dist; int near_spawn = g_trs.near_spawn; g_trs.near_spawn = 0;
    if (g_trs.stuck_n) { float rd = 1.5707963f * (float)(g_trs.stuck_n % 4), c = cosf(rd), sn = sinf(rd), x = fw[0] * c - fw[1] * sn, y = fw[0] * sn + fw[1] * c; fw[0] = x; fw[1] = y; }   // after a blocked spawn: try 90 degrees further round
    float loc[3] = { pl[0] + fw[0] / h * dist, pl[1] + fw[1] / h * dist, pl[2] + 60.f }, yaw = atan2f(-fw[1], -fw[0]) * 57.29578f;
    tr_scan_starts(); int si = resp ? -2 : near_spawn || g_trs.far_blocked ? -1 : btr_pick_start((const float (*)[3])g_trs.st_loc, g_trs.st_first, g_trs.st_n, pl, 300.f); g_trs.at_start = si >= 0 || si == -2;   // like a second player, at the other team's start (a blocked bot is re-spawned near instead; once a far start was blocked, later rounds start closer)
    if (si >= 0 && ms_nav_ready()) {                                                               // with a navmesh, a start that cannot reach the player (partial path: stage, upper ring) is not used
        float len; int part, np, ok = ms_nav_path(g_trs.st_loc[si], pl, &len, &part, &np);
        if (!ok || part) { int best = -1; float bd = -1;
            for (int i = 0; i < g_trs.st_n; i++) { float dx = g_trs.st_loc[i][0] - pl[0], dy = g_trs.st_loc[i][1] - pl[1], dd = dx * dx + dy * dy; float l2; int p2, n2;
                if (i == si || dd < 300.f * 300.f || dd <= bd || !ms_nav_path(g_trs.st_loc[i], pl, &l2, &p2, &n2) || p2) continue; bd = dd; best = i; }
            logf_("bottrial: %s: start %d has %s path to the player - using start %d instead", g_trs.id, si, ok ? "only a PARTIAL" : "no", best); if (best >= 0) si = best; }
        else logf_("bottrial: %s: navmesh path start %d -> player: %.0f uu, %d point(s)", g_trs.id, si, len, np);
    }
    if (si >= 0) { const float *s = g_trs.st_loc[si]; loc[0] = s[0]; loc[1] = s[1]; loc[2] = s[2] + 60.f; yaw = atan2f(pl[1] - s[1], pl[0] - s[0]) * 57.29578f; dist = sqrtf((pl[0] - s[0]) * (pl[0] - s[0]) + (pl[1] - s[1]) * (pl[1] - s[1])); }
    else if (si == -2) { const float *s = g_trs.resp_loc; loc[0] = s[0]; loc[1] = s[1]; loc[2] = s[2] + 60.f; yaw = g_trs.resp_yaw; dist = sqrtf((pl[0] - s[0]) * (pl[0] - s[0]) + (pl[1] - s[1]) * (pl[1] - s[1])); }
    else if (!near_spawn) logf_("bottrial: %s: %s - the bot spawns %.0f uu in front of the player", g_trs.id, g_trs.far_blocked ? "a far start was blocked earlier in this trial" : "no player start at least 300 uu from the player", dist);
    g_trs.level = btr_level(t->level >= 0 ? t->level : 60, t->step, g_trs.m.stage);
    if (t->level >= 0 || t->step) { char lv[16]; snprintf(lv, sizeof lv, "%d", g_trs.level); logf_("bottrial: %s: archetype m_uiInitialDifficultyLevel = %s -> %d", bid, lv, CA->set_property_text(CA, g_np[def].obj, "m_uiInitialDifficultyLevel", lv)); }
    np_ticket_dump(CA, "before the bot spawn");
    g_trs.bot = CA->npc_spawn(CA, bid, loc, &yaw, NULL); memcpy(g_trs.spawn_loc, loc, sizeof loc); g_trs.assist_checked = 0; g_trs.tk_fixed = 0; g_trs.stuck_t = tr_now(); g_trs.stuck_d = -1;
    g_trs.t_spawn = tr_now(); g_trs.d_spawn = dist; g_trs.lock_set = 0; g_trs.run_calls = 0; g_trs.run_ok = 0; g_trs.reached = 0; g_trs.t_run = 0; g_trs.hit_pending = 0; g_trs.hits_interrupt = g_trs.hits_through = g_trs.hits_idle = 0; g_trs.brain_off = 0; g_trs.run_blocked = 0; g_trs.run_best = -1; g_trs.t_stop = 0; g_trs.n_stops = 0; if (!near_spawn) g_trs.t_round = tr_now();
    memcpy(g_trs.prog_loc, loc, sizeof loc); g_trs.t_prog = tr_now(); g_trs.still_logged = 0; g_trs.wp_tried = 0; g_trs.detour_until = 0; g_trs.detour_active = 0; g_trs.nav_stall = 0;
    g_trs.assist_on = t->assist != 0;                                                               // on from the spawn (duel maps have no navmesh); "off" leaves the AI alone
    if (!g_trs.bot) { tr_abort("npc_spawn produced nothing (see the npc lines)"); return; }
    if (t->level >= 0 || t->step) CA->npc_difficulty(CA, g_trs.bot, (float)g_trs.level);
    snprintf(g_trs.bot_name, sizeof g_trs.bot_name, "%s", g_np[def].name[0] ? g_np[def].name : bid);
    { char *paren = strstr(g_trs.bot_name, " ("); if (paren && t->player_like) *paren = 0; }            // "Duelist (Faejin, level 60)" -> "Duelist": the name a player would show
    int round = g_trs.m.p + g_trs.m.b + 1; char b[160];
    if (g_trs.m.stages > 1 && round == 1) snprintf(b, sizeof b, "Bot %d/%d: %s", g_trs.m.stage + 1, g_trs.m.stages, g_trs.bot_name); else snprintf(b, sizeof b, "Round %d", round);
    if (!g_trs.early && !resp) CA->show_banner(CA, b, 3.f);
    tr_hud(); g_trs.attacks = 0; g_trs.death_victim = NULL; g_trs.t_log = 0;
    logf_("bottrial: %s: stage %d/%d round %d: %s spawned %p at %.0f %.0f %.0f (%s; player %.0f %.0f %.0f, %.0f uu away), AI level %d (live %.1f), approach %s", g_trs.id, g_trs.m.stage + 1, g_trs.m.stages, round,
          bid, g_trs.bot, loc[0], loc[1], loc[2], si == -2 ? "RESPAWN at the game mode's start pick" : si >= 0 ? (g_trs.early ? "the other player start, during the intro" : "the other player start") : near_spawn ? "re-spawn near after a block" : "in front of the player",
          pl[0], pl[1], pl[2], dist, g_trs.level, CA->npc_difficulty(CA, g_trs.bot, -1), g_trs.assist_on ? "run + lock-move" : "off (the AI alone)");
    if (t->player_like) tr_ps_attach(g_trs.bot, g_trs.early ? "intro spawn" : resp ? "respawn" : "spawn");
    bt_on_spawned(g_trs.bot, "trial spawn");                                                       // the guide stat points now (npcs also does it 1.5 s later)
    if (g_trs.early) { tr_bot_ai(0, "intro / countdown: the bot waits like a player"); logf_("bottrial: SHOT intro_bot_spawned"); return; }   // stays in TP_WAIT_MATCH until InProgress
    logf_("bottrial: SHOT round_%d_%d_start", g_trs.m.stage + 1, round);
    tr_go(TP_FIGHT);
}
static void tr_round_result(int player_won, const char *why) {
    CTrial *t = tr_cur(); AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi);
    logf_("bottrial: %s: round over (%s): %s; stock team scores BEFORE the plugin's write %d / %d%s", g_trs.id, why, player_won ? "PLAYER point" : "BOT point", mi.team_score[0], mi.team_score[1],
          !player_won ? "" : mi.team_score[0] <= g_trs.m.p ? " (the stock duel did NOT score the NPC kill)" : " (the stock duel scored on its own)");
    logf_("bottrial: %s: ROUND SUMMARY: spawned %s %.0f uu away, approach %s, hits on the attacking bot: interrupted %d, hit through %d; hits on an idle bot %d", g_trs.id, g_trs.at_start ? "at the other player start" : "in front / near",
          g_trs.d_spawn, g_trs.reached ? "reached the attack range" : "NEVER reached the attack range", g_trs.hits_interrupt, g_trs.hits_through, g_trs.hits_idle);
    g_trs.last_result = btr_round(&g_trs.m, player_won); g_trs.stuck_n = 0; g_trs.respawned = 0;
    if (t->player_like) {                                                                           // the kill went through the stock code (the bot has a player state): check, fall back only if it did not count
        int want = player_won ? g_trs.m.tp : g_trs.m.tb, got = mi.team_score[player_won ? 0 : 1];
        logf_("bottrial: %s: STOCK SCORE %s: team %d score %d, trial count %d -> %s", g_trs.id, player_won ? "player" : "bot", player_won ? 0 : 1, got, want,
              got == want ? "OK (counted by the game's own kill / score code)" : player_won ? "MISSING - the plugin writes the player's m_Scores (fallback)" : "MISSING for the bot (no fallback: see the player state lines)");
        if (player_won && got != want) {                                                         // the AI is unpossessed before the stock Killed runs (no victim player state): score it with the stock
            void *p = np_player(CA), **pps = p ? (void **)CA->prop_ptr(CA, p, "PlayerState") : NULL, *pps_ = pps ? *pps : NULL; int done = 0;   // AAdversarialPlayerState::ScoreKill(victim PS), vtable +0x740 (AThePlainesGameMode::Killed)
            __try { if (tk_ptr_ok(pps_) && g_trs.ps && CA->is_a(CA, pps_, "AdversarialPlayerState")) { void (*sk)(void *, void *, int) = (void (*)(void *, void *, int))(*(void ***)pps_)[0x740 / 8];
                    if ((uintptr_t)sk - (uintptr_t)g_base == 0x3E20E0) { sk(pps_, g_trs.ps, 0); done = 1; } else logf_("bottrial: %s: vtable +0x740 is exe+0x%llX, not AAdversarialPlayerState::ScoreKill - not called", g_trs.id, (unsigned long long)((uintptr_t)sk - (uintptr_t)g_base)); } }
            __except (EXCEPTION_EXECUTE_HANDLER) { logf_("bottrial: %s: ScoreKill: EXCEPTION 0x%08lX", g_trs.id, GetExceptionCode()); done = 0; }
            AbsMatchInfo m2; memset(&m2, 0, sizeof m2); m2.size = sizeof m2; CA->match_info(CA, &m2); char sc[200] = "?"; if (tk_ptr_ok(pps_)) CA->get_property_text(CA, pps_, "m_Scores", sc, sizeof sc);
            logf_("bottrial: %s: player point through the stock AAdversarialPlayerState::ScoreKill(player PS, bot PS) -> %d: team 0 score now %d (player m_Scores %s)", g_trs.id, done, m2.team_score[0], sc);
            if (m2.team_score[0] != want) tr_write_score(g_trs.m.tp);
        }
    } else if (player_won) tr_write_score(g_trs.m.p);
    char b[160]; snprintf(b, sizeof b, player_won ? "You take the round" : "%s takes the round", g_trs.bot_name); CA->show_banner(CA, b, 3.f); tr_hud();
    logf_("bottrial: %s: score %d - %d (stage %d/%d), total %d - %d, bot attacks this round %d -> %s", g_trs.id, g_trs.m.p, g_trs.m.b, g_trs.m.stage + 1, g_trs.m.stages, g_trs.m.tp, g_trs.m.tb, g_trs.attacks,
          g_trs.last_result == BTR_NEXT_ROUND ? "next round" : g_trs.last_result == BTR_NEXT_STAGE ? "next bot" : g_trs.last_result == BTR_TRIAL_WON ? "TRIAL WON" : "TRIAL LOST");
    logf_("bottrial: SHOT point_%d_%d", g_trs.m.tp, g_trs.m.tb);
    int over = g_trs.last_result == BTR_TRIAL_WON || g_trs.last_result == BTR_TRIAL_LOST;
    if (t->player_like && !over) { g_trs.t_down = 0; tr_go(player_won ? TP_BOT_DOWN : TP_PLAYER_DOWN); return; }   // the loser respawns like a player, the winner stays
    if (over && tk_ptr_ok(g_trs.bot)) tr_bot_ai(0, "the trial is decided");
    tr_go(TP_ROUND_END);
}
static void tr_tick(void *u) {
    (void)u; if (!g_trs.on) return; CTrial *t = tr_cur(); if (!t) { g_trs.on = 0; return; }
    char wn[160]; tr_world_name(wn, sizeof wn); void *w = CA->get_world(CA);
    if (tr_arena_phase(g_trs.phase) && w != g_trs.world) { g_trs.bot = NULL; tr_abort("the world changed during the trial"); return; }
    if ((g_trs.phase == TP_WAIT_MATCH || tr_arena_phase(g_trs.phase)) && w == g_trs.world) {                 // fall guard in the arena only (kill-z): back to the last spot the player STOOD on in this world
        void *p = np_player(CA); float l[3]; AbsCharInfo pi;
        if (p && tr_char(p, &pi) && !pi.is_dead && CA->test_get_location(CA, p, l)) {
            float sz = g_trs.fall.safe[2]; int r = tk_fall_step(&g_trs.fall, w, l, 1000.f, 2000.f, GetTickCount64());
            if (r == TK_FALL_RETURN) { float s[3] = { g_trs.fall.safe[0], g_trs.fall.safe[1], g_trs.fall.safe[2] + 40.f }; g_trs.falls++;
                logf_("bottrial: %s: FALL GUARD: player z %.0f is %.0f below the last spot it stood on - put back at %.0f %.0f %.0f -> %d (fall %d)", g_trs.id, l[2], sz - l[2], s[0], s[1], s[2], CA->test_teleport(CA, p, s, NULL), g_trs.falls); }
            else if (r == TK_FALL_GIVEUP) { tr_abort("the player keeps falling out of the arena (more than 3 falls in 10 s)"); return; }
        }
    }
    if (tr_arena_phase(g_trs.phase) && !g_trs.pm_logged) {                     // the stock post-match flow must never run in a trial (it ends by travel)
        AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi);
        if (strstr(mi.state, "PostMatch")) { g_trs.pm_logged = 1; logf_("bottrial: %s: WARNING the stock match went to %s during the trial (scores %d / %d, winning rounds %d) - the stock post-match screen may show", g_trs.id, mi.state, mi.team_score[0], mi.team_score[1], mi.winning_rounds); }
    }
    switch (g_trs.phase) {
    case TP_TRAVEL:
        if (btr_is_map(wn, t->map)) { g_trs.world = w; logf_("bottrial: %s: arrived in %s after %.1f s", g_trs.id, wn, tr_since()); tr_go(TP_WAIT_MATCH); }
        else if (tr_since() > 60) tr_abort("the map did not load within 60 s (is its pak installed?)");
        return;
    case TP_WAIT_MATCH: {
        if (w != g_trs.world) { tr_abort("left the arena while waiting for the match"); return; }
        AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; CA->match_info(CA, &mi);
        if (!g_trs.logged_ip && mi.game_mode) { char r[8]; snprintf(r, sizeof r, "%d", t->rounds); g_trs.logged_ip = 1; logf_("bottrial: %s: game mode %s, state %s: m_iNbWinningRounds = %s -> mask %d", g_trs.id, mi.mode, mi.state, r, CA->set_mode_prop(CA, "m_iNbWinningRounds", r));
            if (t->player_like) { char back[16] = "?"; int ok = CA->set_property_text(CA, mi.game_mode, "m_iNbWinningRounds", "99"); CA->get_property_text(CA, mi.game_mode, "m_iNbWinningRounds", back, sizeof back);   // IsMatchOver reads the GAME MODE copy: the trial ends itself
                logf_("bottrial: %s: game mode m_iNbWinningRounds -> 99 (%d, reads %s): AAdversarialGameMode::IsMatchOver never ends the match; the game state keeps %s for the HUD diamonds", g_trs.id, ok, back, r); } }
        if (t->player_like && t->intro && !g_trs.bot && strstr(mi.state, "Intro")) {                 // the bot joins while team 0's intro scene plays, then team 1's scene shows it
            if (!g_trs.scenes_scanned) { g_trs.scenes_scanned = 1; tr_world_name(g_tr_scan_world, sizeof g_tr_scan_world); CA->for_each_object(CA, tr_scene_cb, NULL); logf_("bottrial: %s: %d intro scene(s) in %s", g_trs.id, g_trs.n_scenes, g_tr_scan_world); }
            int d0 = tr_scene_dummies(0);
            // a bot spawned before the scene's runtime navmesh is READY accepts every pathfinding request and never moves (its path following has no nav
            // data yet): wait for the build (mapscript g_nav.st 1/4/2 = building, 3 ready, -1 failed, 0 none) while team 0's scene plays; at most 11 s after the arrival
            // st 0 is ambiguous - "no navmesh for this world" or "its scene has not been built yet" (the intro spawn can come first). ms_nav_expected tells them apart.
            int nav_building = g_nav.st == 1 || g_nav.st == 4 || g_nav.st == 2 || ms_nav_expected();
            if (nav_building && tr_since() < 11) { static double t_w; if (tr_now() - t_w > 1) { t_w = tr_now(); logf_("bottrial: %s: intro: the navmesh is still building (state %d) - the bot enters when it is ready", g_trs.id, g_nav.st); } return; }
            if ((d0 > 0 && !nav_building) || tr_since() > 12) {
                logf_("bottrial: %s: intro: team 0's scene has %d dummy(ies) (%.1f s in %s) - the bot enters now", g_trs.id, d0, tr_since(), mi.state);
                g_trs.early = 1; tr_spawn(); g_trs.early = 0; if (g_trs.bot) tr_hide(1);
                if (g_trs.bot) tr_team_infos("intro: the bot joins team 1", 1);
            }
            return;
        }
        if (g_trs.bot && !g_trs.intro_done) {                                                       // the bot's own intro: team 1's scene spawns its dummy
            int d1 = tr_scene_dummies(1); static double t_last;
            if (d1 > 0) { g_trs.intro_done = 1; logf_("bottrial: %s: INTRO: team 1's scene plays with %d dummy(ies) (the bot's intro, %.1f s after the bot joined)", g_trs.id, d1, tr_now() - g_trs.t_spawn); logf_("bottrial: SHOT intro_bot"); }
            else if (tr_now() - t_last > 2) { t_last = tr_now(); logf_("bottrial: %s: intro: state %s, team 0 dummies %d, team 1 dummies %d", g_trs.id, mi.state, tr_scene_dummies(0), d1); }
        }
        if (g_tr_hidden && g_trs.bot && !strstr(mi.state, "Intro")) tr_hide(0);   // the intro scenes are over: visible for the countdown like the player
        if (g_trs.bot && g_trs.ai_paused && !strstr(mi.state, "InProgress")) { static double t_p; if (tr_now() - t_p > 1) { t_p = tr_now(); CA->test_ai(CA, g_trs.bot, 0); } }   // keep it waiting (its tree may start after the first stop)
        if (strstr(mi.state, "InProgress")) {
            if (g_trs.logged_ip == 1) { g_trs.logged_ip = 2; g_trs.t_start = tr_now();
                logf_("bottrial: %s: match InProgress (%s, teams %d, winning rounds %d, scores %d / %d)", g_trs.id, mi.mode, mi.team_count, mi.winning_rounds, mi.team_score[0], mi.team_score[1]); if (!t->player_like) tr_write_score(0); CA->show_banner(CA, t->name[0] ? t->name : "Bot trial", 3.f); }
            if (t->player_like && g_trs.bot) {                                                      // it already stands at its start: the fight begins for both at once
                if (!g_trs.intro_done) logf_("bottrial: %s: INTRO: team 1's scene never showed a dummy (see the team infos lines)", g_trs.id);
                tr_bot_ai(1, "InProgress"); np_engage(CA, g_trs.bot, "match start"); g_trs.t_round = tr_now(); g_trs.t_spawn = tr_now(); tr_go(TP_FIGHT); logf_("bottrial: SHOT round_1_1_start"); return;
            }
            tr_go(TP_SPAWN);                                                                         // no extra wait: the bot enters with the match like a second player
        } else if (tr_since() > 90) tr_abort("the match never reached InProgress (solo duel / pvp flow off?)");
        return; }
    case TP_SPAWN: tr_spawn(); return;
    case TP_FIGHT: {
        void *p = np_player(CA); AbsCharInfo bi, pi; int bok = tr_char(g_trs.bot, &bi), pok = tr_char(p, &pi);
        if (!bok) { logf_("bottrial: %s: the bot %p is gone without a death - the round is replayed", g_trs.id, g_trs.bot); g_trs.bot = NULL; tr_go(TP_SPAWN); return; }
        if (g_trs.death_victim == g_trs.bot || bi.is_dead) { tr_round_result(1, g_trs.death_victim ? "on_death" : "bot health 0"); return; }
        if ((p && g_trs.death_victim == p) || (pok && pi.is_dead)) { tr_round_result(0, g_trs.death_victim ? "on_death" : "player health 0"); return; }
        tr_hit_check();
        if (g_trs.run_blocked && !t->block_respawn) { g_trs.run_blocked = 0; g_trs.respawned = 1; logf_("bottrial: %s: the run-in looks blocked - NO re-spawn (block_respawn off: a player is never teleported), the bot keeps chasing", g_trs.id); }
        if (g_trs.run_blocked) { if (g_trs.at_start) g_trs.far_blocked = 1;
            logf_("bottrial: %s: the run-in is blocked - the round is replayed with the bot 150 uu away (the ONE re-spawn of this round%s)", g_trs.id, g_trs.at_start ? "; later rounds spawn in front of the player, not at the far start" : "");
            tr_remove_bot("run blocked"); g_trs.respawned = 1; g_trs.near_spawn = 1; g_trs.stuck_n++; tr_go(TP_SPAWN); return; }
        if (!g_trs.tk_fixed && g_trs.attacks == 0 && tr_since() > 8 && p) {                         // a close bot that never attacks: is it in the attack-ticket list of the player?
            int st = np_ticket_state(CA, g_trs.bot, p); g_trs.tk_fixed = 1; np_ticket_dump(CA, "bot without attacks after 8 s");
            if (st == 0 || st == -1) logf_("bottrial: %s: the bot is %s - RegisterForTarget(bot, player) -> %d, now %d", g_trs.id, st == 0 ? "NOT in the player's attacker list" : "not tracked (no ticket entry for the player)", np_ticket_register(CA, g_trs.bot, p), np_ticket_state(CA, g_trs.bot, p));
            else logf_("bottrial: %s: the bot is in the player's attacker list (state %d) but has not attacked", g_trs.id, st);
        }
        if (g_trs.assist_on && pok && !bi.attacking && t->block_respawn) {                          // assisted but blocked (furniture / a gate between): respawn the round 150 uu ahead
            float a[3], b[3], d = -1; if (CA->test_get_location(CA, g_trs.bot, a) && CA->test_get_location(CA, p, b)) d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]));
            if (d < 200 || g_trs.stuck_d < 0 || fabsf(d - g_trs.stuck_d) > 25) { g_trs.stuck_d = d; g_trs.stuck_t = tr_now(); }
            else if (tr_now() - g_trs.stuck_t > 12 && !g_trs.respawned) { logf_("bottrial: %s: the bot is stuck %.0f uu away for 12 s (blocked?) - the round is replayed 150 uu away, 90 degrees further round (the ONE re-spawn of this round)", g_trs.id, d); tr_remove_bot("stuck"); g_trs.respawned = 1; g_trs.near_spawn = 1; g_trs.stuck_n++; tr_go(TP_SPAWN); return; }
        }
        if (t->player_like && tr_since() > t->round_s) { if (tr_now() - g_trs.t_down > t->round_s) { g_trs.t_down = tr_now(); logf_("bottrial: %s: WARNING no knock-out for %.0f s (player-like: nobody is moved; the fight goes on)", g_trs.id, tr_since()); } }
        else if (tr_since() > t->round_s) { logf_("bottrial: %s: round time %.0f s is over without a knock-out - replayed", g_trs.id, t->round_s); CA->show_banner(CA, "Time - the round is replayed", 3.f); tr_remove_bot("round time"); tr_go(TP_SPAWN); return; }
        if (tr_now() - g_trs.t_log >= 5) {
            g_trs.t_log = tr_now(); float a[3], b[3], d = -1; if (pok && CA->test_get_location(CA, g_trs.bot, a) && CA->test_get_location(CA, p, b)) d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]));
            logf_("bottrial: %s: fight t=%.0f: distance %.0f, bot %.0f/%.0f, player %.0f/%.0f, bot attacks %d, assist %s (%d frames), AI level %.1f", g_trs.id, tr_since(), d, bi.health, bi.max_health, pok ? pi.health : -1, pok ? pi.max_health : -1, g_trs.attacks, g_trs.assist_on ? "on" : "off", g_trs.assist_frames, CA->npc_difficulty(CA, g_trs.bot, -1));
        }
        return; }
    case TP_BOT_DOWN: {                                                                             // the bot was knocked out: it respawns like a player (the game's respawn delay, the game mode's start pick)
        if (g_trs.t_down == 0) {
            g_trs.t_down = tr_now(); g_trs.death_respawn_s = tr_gs_float("m_fDeathRespawnTime", 5.f); if (g_trs.death_respawn_s < 1.f || g_trs.death_respawn_s > 30.f) g_trs.death_respawn_s = 5.f;
            g_trs.resp_ok = tr_find_start(g_trs.bot, g_trs.resp_loc, &g_trs.resp_yaw);
            logf_("bottrial: %s: the bot is down: respawn in %.1f s (GameState m_fDeathRespawnTime) at %s", g_trs.id, g_trs.death_respawn_s, g_trs.resp_ok ? "the game mode's pick" : "the other player start (no pick)");
        }
        if (tr_now() - g_trs.t_down < (g_trs.death_respawn_s > 3.5f ? g_trs.death_respawn_s : 3.5f)) return;
        if (tk_ptr_ok(g_trs.bot)) tr_ps_detach(g_trs.bot, "respawn");
        { int keep = g_trs.resp_ok; tr_remove_bot("knocked out - respawning"); g_trs.resp_ok = keep; }
        tr_go(TP_SPAWN); return; }
    case TP_PLAYER_DOWN: {                                                                          // the player was knocked out: the stock duel respawns the player; the bot stays where it is (a surviving player is not reset)
        if (g_trs.t_down == 0) { AbsCharInfo bi; g_trs.t_down = tr_now(); logf_("bottrial: %s: the player is down - waiting for the stock respawn; the bot stays (health %.0f)", g_trs.id, tr_char(g_trs.bot, &bi) ? bi.health : -1.f); }
        void *p = np_player(CA); AbsCharInfo pi;
        if (!p || !tr_char(p, &pi) || pi.is_dead || pi.health <= 0) { if (tr_now() - g_trs.t_down > 30) tr_abort("the player did not respawn within 30 s"); return; }
        logf_("bottrial: %s: the player respawned (%p) %.1f s after the knock-out - the bot is sent at the new pawn", g_trs.id, p, tr_now() - g_trs.t_down);
        if (!tk_ptr_ok(g_trs.bot)) { tr_go(TP_SPAWN); return; }
        np_engage(CA, g_trs.bot, "player respawned");
        if (g_trs.brain_off) { g_trs.brain_off = 0; tr_bot_ai(1, "player respawned: the run-in pause ends"); }
        g_trs.wp_tried = 0; g_trs.detour_until = 0; g_trs.detour_active = 0; g_trs.nav_stall = 0;
        g_trs.lock_set = 0; g_trs.reached = 0; g_trs.run_calls = g_trs.run_ok = 0; g_trs.run_best = -1; g_trs.respawned = 0; g_trs.stall_replaced = 0; g_trs.run_blocked = 0; g_trs.t_round = tr_now(); g_trs.t_spawn = tr_now();
        { float bl[3]; if (CA->test_get_location(CA, g_trs.bot, bl)) { memcpy(g_trs.prog_loc, bl, sizeof bl); g_trs.d_spawn = tr_dist(g_trs.bot, p); } g_trs.t_prog = tr_now(); }
        g_trs.attacks = 0; g_trs.death_victim = NULL; g_trs.t_down = 0; logf_("bottrial: SHOT round_%d_%d_start", g_trs.m.stage + 1, g_trs.m.p + g_trs.m.b + 1);
        tr_go(TP_FIGHT); return; }
    case TP_ROUND_END:
        if (tr_since() < 3.5) return;
        if (t->player_like && (g_trs.last_result == BTR_TRIAL_WON || g_trs.last_result == BTR_TRIAL_LOST)) { tr_go(TP_END); return; }   // the bot stays in the arena to the end, like a player
        if (tk_ptr_ok(g_trs.bot) && t->player_like) tr_ps_detach(g_trs.bot, "round over");
        tr_remove_bot("round over");
        if (g_trs.last_result == BTR_TRIAL_WON || g_trs.last_result == BTR_TRIAL_LOST) { tr_go(TP_END); return; }
        tr_go(TP_SPAWN); return;
    case TP_END:                                                                                    // the reward is shown here and PAID after the travel back: XP granted in the arena is lost once the open world loads
        if (!g_trs.paid) {
            g_trs.paid = 1; AbsTrialReward r; memset(&r, 0, sizeof r); r.size = sizeof r;
            g_trs.xp = CA->trial_reward(CA, NULL, t->ranks[0] ? t->ranks : NULL, g_trs.m.won, g_trs.m.tp, 0, 0, &r); g_trs.points = r.points;
            char b[160]; btr_end_text(&g_trs.m, g_trs.bot_name, g_trs.xp, b, sizeof b); CA->show_banner(CA, b, t->end_delay);   // duel wording, never "Trial lost"
            logf_("bottrial: %s: END: %s %d - %d over %d bot(s); trial_reward(%s) = %d XP, %d point(s) - paid after the travel back", g_trs.id, g_trs.m.won ? "WON" : "LOST", g_trs.m.tp, g_trs.m.tb, g_trs.m.stages,
                  t->ranks[0] ? t->ranks : "stock 1v1", g_trs.xp, r.points);
            logf_("bottrial: SHOT result");
        }
        if (tr_since() >= t->end_delay) { tr_go(TP_PAYOUT); tr_finish_travel(); }
        return;
    case TP_PAYOUT: {
        void *p = np_player(CA);
        if (!btr_is_map(wn, t->ret) || !p) { if (tr_since() > 90) { logf_("bottrial: %s: never arrived in %s - the reward is NOT paid", g_trs.id, t->ret); tr_go(TP_DONE); g_trs.on = 0; } else g_trs.t_log = tr_now(); return; }
        if (tr_now() - g_trs.t_log < 4) return;                                                     // let the character and its stats load
        AbsTrialReward r; memset(&r, 0, sizeof r); r.size = sizeof r;
        int xp = CA->trial_reward(CA, NULL, t->ranks[0] ? t->ranks : NULL, g_trs.m.won, g_trs.m.tp, 0, 1, &r); g_trs.xp = xp; g_trs.points = r.points;
        char b[160]; snprintf(b, sizeof b, "Bot trial reward: +%d XP, +%d point(s)", xp, r.points); CA->show_banner(CA, b, 5.f);
        logf_("bottrial: %s: PAID in %s: %d XP, %d point(s), xp_add %d, Rift Coins %d", g_trs.id, wn, xp, r.points, r.paid, r.rift_coins);
        logf_("bottrial: SHOT payout");
        tr_go(TP_DONE); g_trs.on = 0; logf_("bottrial: %s: DONE", g_trs.id); return; }
    }
}
static int tr_start(int ti, const char *who) {
    if (g_trs.on) { logf_("bottrial: %s: start of '%s' refused - trial '%s' is running", who, g_trl[ti].id, g_trs.id); return 0; }
    CTrial *t = &g_trl[ti];
    for (int i = 0; i < t->nbots; i++) if (tr_np_def(t->bots[i]) < 0) { logf_("bottrial: %s: '%s' refused - no bots / npcs entry '%s' was applied (see the bots: lines)", who, t->id, t->bots[i]); CA->show_message(CA, "Bot trial: unknown bot (see the log)", 4.f); return 0; }
    int keep_timer = g_trs.timer, keep_hooked = g_trs.hooked, keep_ticked = g_trs.ticked; memset(&g_trs, 0, sizeof g_trs); g_trs.timer = keep_timer; g_trs.hooked = keep_hooked; g_trs.ticked = keep_ticked;
    g_trs.on = 1; g_trs.ti = ti; snprintf(g_trs.id, sizeof g_trs.id, "%s", t->id); btr_init(&g_trs.m, t->nbots, t->rounds); g_trs.phase = TP_IDLE;
    if (!g_trs.hooked) { g_trs.hooked = 1; logf_("bottrial: on_death -> %d, on_attack_start -> %d, on_hit -> %d", CA->on_death(CA, tr_on_death, NULL), CA->on_attack_start(CA, tr_on_attack, NULL), CA->on_hit(CA, tr_on_hit, NULL)); }
    if (!g_trs.timer) g_trs.timer = CA->set_timer_id(CA, 0.25f, 1, tr_tick, NULL);
    if (!g_trs.ticked) { g_trs.ticked = 1; CA->on_tick(CA, tr_frame, NULL); }
    g_trs.prev_solo = CA->solo_duel(CA, -1); CA->solo_duel(CA, 1);
    char bl[400] = ""; size_t n = 0; for (int i = 0; i < t->nbots && n < sizeof bl - 40; i++) n += snprintf(bl + n, sizeof bl - n, "%s%s (level %d)", i ? ", " : "", t->bots[i], btr_level(t->level >= 0 ? t->level : 60, t->step, i));
    logf_("bottrial: %s: START (%s): map %s, first to %d, bot(s) %s, rewards %s, return %s", t->id, who, t->map, t->rounds, bl, t->ranks[0] ? t->ranks : "stock 1v1", t->ret);
    tr_go(TP_TRAVEL); int ok = CA->travel(CA, t->map); if (!ok) { tr_abort("travel refused"); return 0; }
    return 1;
}
static void tr_menu_cb(void *u) { int ti = (int)(intptr_t)u; if (ti >= 0 && ti < g_trl_n) tr_start(ti, "F2 menu"); }
static int trials_apply(const char *mod, const char *id, const char *json, void *u) {
    (void)u; JDoc d; if (jdoc_parse(&d, json) != 0) return 0;
    static CTrial t; char err[200] = ""; int ok = tr_parse(&d, &t, err, sizeof err); jdoc_free(&d); if (!ok) { logf_("bottrial: %s/%s: %s", mod, id, err); return 0; }
    int ti = -1; for (int i = 0; i < g_trl_n; i++) if (!strcmp(g_trl[i].id, id)) ti = i;
    if (ti < 0) { if (g_trl_n >= 16) { logf_("bottrial: too many bot_trials entries (16), %s ignored", id); return 0; } ti = g_trl_n++; }
    int added = g_trl[ti].menu_added; g_trl[ti] = t; g_trl[ti].menu_added = added; snprintf(g_trl[ti].mod, sizeof g_trl[ti].mod, "%s", mod); snprintf(g_trl[ti].id, sizeof g_trl[ti].id, "%s", id);
    if (!g_trl[ti].name[0]) snprintf(g_trl[ti].name, sizeof g_trl[ti].name, "%s", id);
    if (t.menu && !added) { char l[140]; snprintf(l, sizeof l, "Bot trial: %s", g_trl[ti].name); g_trl[ti].menu_added = CA->add_menu_item(CA, l, tr_menu_cb, (void *)(intptr_t)ti) == 0; }
    int missing = 0; for (int i = 0; i < t.nbots; i++) if (tr_np_def(t.bots[i]) < 0) missing++;
    logf_("bottrial: %s/%s ready: %s, %d bot(s), first to %d, AI level %d step %d, rewards %s, menu %d%s", mod, id, t.map, t.nbots, t.rounds, t.level, t.step, t.ranks[0] ? t.ranks : "stock", g_trl[ti].menu_added,
          missing ? " (some bots are not applied yet - checked again at start)" : "");
    return 0;
}

// ================================================================ "maps" entries with "menu": {"label", "travel", "return"}: one F2 Mods page item per entry that starts a SOLO DUEL on that map
// (solo_duel(1) + travel; pressed again inside that map it travels to "return", default MainMap_Main). The solo duel switch goes back to its old value on the next world
// that is not that map. Data only (no plugin), e.g. for a custom arena map.
typedef struct { char label[96], map[200], ret[200]; } MsTravel;
static MsTravel g_mst[8]; static int g_mst_n, g_mst_active = -1, g_mst_prev = -1, g_mst_hooked;
static const char *mst_short(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int mst_in(const char *world_path, int i) { char nm[200]; snprintf(nm, sizeof nm, "%s", mst_short(g_mst[i].map)); char *dot = strchr(nm, '.'); if (dot) *dot = 0; return nm[0] && strstr(world_path, nm) != NULL; }
static void mst_world(const char *map, void *u) {
    (void)u; if (g_mst_active < 0 || mst_in(map, g_mst_active)) return;
    logf_("maps: left %s (now %s): solo duel back to %d", g_mst[g_mst_active].label, map, g_mst_prev); CA->solo_duel(CA, g_mst_prev); g_mst_active = -1; g_mst_prev = -1;
}
static void mst_cb(void *u) {
    int i = (int)(intptr_t)u; if (i < 0 || i >= g_mst_n) return; MsTravel *m = &g_mst[i];
    if (g_trs.on) { logf_("maps: menu '%s' refused - bot trial '%s' is running", m->label, g_trs.id); CA->show_message(CA, "A bot trial is running", 3.f); return; }
    char wp[300] = ""; void *w = world(); if (w) se_path_of(w, wp, sizeof wp);
    if (g_mst_active == i && mst_in(wp, i)) { logf_("maps: menu '%s': back to %s -> %d", m->label, m->ret, CA->travel(CA, m->ret)); return; }
    if (g_mst_prev < 0) g_mst_prev = CA->solo_duel(CA, -1);
    CA->solo_duel(CA, 1); g_mst_active = i;
    logf_("maps: menu '%s': solo duel on (was %d), travel %s -> %d", m->label, g_mst_prev, m->map, CA->travel(CA, m->map));
}
static int maps_apply(const char *mod, const char *id, const char *json, void *u) {          // content kind "maps": the scene (mapscript.inc) + the optional travel menu item
    int rc = ms_apply(mod, id, json, u); JDoc d; if (jdoc_parse(&d, json) != 0) return rc;
    int m = jfind(&d, 0, "menu"); const char *lb = m >= 0 ? cs_str(&d, m, "label") : NULL, *tv = m >= 0 ? cs_str(&d, m, "travel") : NULL, *rt = m >= 0 ? cs_str(&d, m, "return") : NULL;
    if (m >= 0 && (!lb || !*lb || !tv || tv[0] != '/')) logf_("maps: %s/%s: \"menu\" needs \"label\" and \"travel\" (a /Game/Maps/... path) - no menu item", mod, id);
    else if (m >= 0) {
        int i = -1; for (int k = 0; k < g_mst_n; k++) if (!strcmp(g_mst[k].label, lb)) i = k;
        if (i < 0 && g_mst_n < 8) {
            i = g_mst_n++; char l[140]; snprintf(l, sizeof l, "Travel: %s (solo duel; again inside = back)", lb);
            if (CA->add_menu_item(CA, l, mst_cb, (void *)(intptr_t)i)) logf_("maps: %s/%s: menu item not added (menu full)", mod, id);
            if (!g_mst_hooked) { g_mst_hooked = 1; CA->on_world_loaded(CA, mst_world, NULL); }
        }
        if (i >= 0) { MsTravel *t = &g_mst[i]; snprintf(t->label, sizeof t->label, "%s", lb); snprintf(t->map, sizeof t->map, "%s", tv); snprintf(t->ret, sizeof t->ret, "%s", rt && *rt ? rt : "/Game/Maps/MainMap/MainMap_Main"); logf_("maps: %s/%s: menu 'Travel: %s' -> %s (return %s)", mod, id, t->label, t->map, t->ret); }
    }
    jdoc_free(&d); return rc;
}

// ---- API 2.16
static int api_p_bot_trial_start(const AbsApi *self, const char *trial_id) {
    const char *who = plug_name_of(plug_of(self));
    if (!api_p_is_game_thread(self) || !trial_id) { logf_("plugin %s: bot_trial_start refused - needs the game thread and an id", who); return 0; }
    for (int i = 0; i < g_trl_n; i++) if (!strcmp(g_trl[i].id, trial_id)) return tr_start(i, who);
    logf_("plugin %s: bot_trial_start('%s'): no bot_trials entry with that id", who, trial_id); return 0;
}
static int api_p_bot_trial_status(const AbsApi *self, AbsTrialStatus *out) {
    (void)self; if (!out || out->size < 8) return 0; AbsTrialStatus r; memset(&r, 0, sizeof r); r.size = sizeof r;
    int have = g_trs.id[0] != 0;
    if (have) { snprintf(r.id, sizeof r.id, "%s", g_trs.id); snprintf(r.phase, sizeof r.phase, "%s", TP_NAME[g_trs.phase]); r.stage = g_trs.m.stage; r.stages = g_trs.m.stages; r.player_score = g_trs.m.p; r.bot_score = g_trs.m.b;
                r.rounds_to_win = g_trs.m.rounds_to_win; r.total_player = g_trs.m.tp; r.total_bot = g_trs.m.tb; r.finished = g_trs.m.done; r.won = g_trs.m.won; r.xp = g_trs.xp; r.points = g_trs.points; r.ai_level = g_trs.level; r.bot = g_trs.bot; }
    memcpy(out, &r, out->size < sizeof r ? out->size : sizeof r); return have;
}

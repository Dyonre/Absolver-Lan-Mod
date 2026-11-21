/* Cheats - test helpers for playing alone: a "Cheats" tab of the loader window (F1; F10 opens it there, plugin.cfg ui_key), or a window of its own on loaders
 * without mod tabs. Every entry toggles (or runs once) and shows a toast; toggles are kept in LanNative\data\Cheats.txt (kv store) and re-applied in every world,
 * so they survive restarts. Fly: the entry ENABLES the cheat (remembered too); while it is enabled the fly key (plugin.cfg fly_key, default B) starts / stops
 * flying. A new world always starts on the ground.
 *   Infinite shards      every 0.25 s: AttackComponent BPF_SetShards(GetShardsMax())
 *   God mode             HealthComponent invincible (test_set_invincible), re-asserted every 0.25 s; off = vulnerable again
 *   Infinite stamina     DefenseComponent m_fCurrentGuard (the stamina bar) = max every 0.25 s
 *   One-hit kills        damage_mod damage_dealt x100 on the player
 *   Fly                  CharacterMovement mode Flying (5): velocity from W A S D (level, steered by the camera yaw) + up / down keys (plugin.cfg fly_up / fly_down = UE key names,
 *                        default SpaceBar / LeftControl; Shift x2.5; I = help; those keys are swallowed while flying; K2_SetActorLocation with sweep + one slide, so walls
 *                        still block). With an optional hover clip (plugin.cfg hover_clip, played in the AnimBP's "Cinematic" slot with PlayerAnim.m_fCinematicWeight
 *                        held at 1) the body takes a hover pose; without it you fly in the normal pose. Off = walking mode again.
 *                        Keys are read with GetAsyncKeyState (the loader routes it to the game window's own keys in background test runs).
 *   Heal me              test_heal (health + stamina full, down state cleared)
 * Multiplayer: while another player is in the game every cheat is off and the actions are refused (the tab says why); the toggles come back when you are alone.
 * The setting off_with_players = 0 keeps them on; the host then tells every joiner "Host has cheats enabled" (loader API 2.35 net_notice).
 * plugin.cfg: fly_speed = 900 (uu/s), fly_key = B, fly_up = SpaceBar, fly_down = LeftControl, hover_clip = <AnimSequence path>, shards = 0|1, god = 0|1, stamina = 0|1, onehit = 0|1
 * (cfg values are the defaults; a toggle in the tab overrides them through the kv store). test_* keys are for automated test runs only.
 * The tab: rows grouped Player / World / NPCs / Game speed / Debug / Items; check boxes, buttons, number fields and an NPC picker; mouse, keys and pad (cheatsui.cpp).
 * The "Add items" page: a searchable list of the game's loaded item data assets (gear, weapons, powers, mod items) built when the page opens, with count, colour
 * variant and "equip after adding"; "Add to inventory" runs inventory_add on the game thread. Nothing is ever added at load.
 * mod.json "settings" (F1 > Mods > Settings): off_with_players, fly_speed, ui_key, default_count, equip_after_add; ui_key applies after a restart.
 * Log lines: "cheats: ..." in LanNative\lannative.log. Build: build.bat (MSVC x64). Allow it in Mods\plugins.txt. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include "../../tools/lannative/AbsolverSE.h"
#include "cheatsui.h"

static const AbsApi *A;
enum { C_SHARDS, C_GOD, C_STAMINA, C_ONEHIT, C_FLY, C_N };
static const char *const C_KEY[C_N] = { "shards", "god", "stamina", "onehit", "fly" };
static const char *const C_LABEL[C_N] = { "Infinite shards", "God mode", "Infinite stamina", "One-hit kills", "Fly" };
static int g_on[C_N], g_applied_onehit = -1, g_applied_god = -1;
static float g_speed = 900.f; static char g_up[32] = "SpaceBar", g_down[32] = "LeftControl", g_clip_path[256] = "/Game/Mods/Cheats/Cheats_Hover.Cheats_Hover";
static float g_fz;   /* the height the flyer is held at (run FB3-fly: something in the game pulled it up ~400 uu every 0.5 s while it flew down) */
static void *g_montage, *g_clip; static int g_clip_tried; static float g_fly_log_t;
static uint64_t n_slot;
typedef struct { uint64_t name; void *a, *b; } FKey;   /* InputCore FKey: FName + TSharedPtr<FKeyDetails> (24 bytes; IsInputKeyDown only compares the name) */
static FKey k_up, k_down; static int vk_up = VK_SPACE, vk_down = VK_CONTROL, vk_fly = 'B', g_flying; static char g_flykey[32] = "B";
/* Up / down keys: PlayerController::IsInputKeyDown never sees unbound keys (AFightingPlayerController::InputKey does not pass them on to PlayerInput, so only bound
 * keys like W A S D are "down"), so keys are read with GetAsyncKeyState while the game window has the focus; while flying the up / down / Shift / I keys are swallowed in the InputKey hook (other mods may hook it
 * too, hooks stack), so Space does not dodge and Shift does not block in the air. Q / E / Z / C (stances), attacks and the camera still reach the game. */
typedef char (*InputKey_t)(void *pc, FKey *key, int event, float amount, char gamepad);
static InputKey_t o_key; static uint64_t n_lshift, n_rshift, n_help;
static void *g_pawn_fly;
static int focused(void) { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static int vdown(int vk) { return focused() && (GetAsyncKeyState(vk) & 0x8000) != 0; }
static int vk_of(const char *n) {                                                  /* UE key name -> virtual key: a letter / digit, SpaceBar, Left/RightControl, Left/RightShift, Left/RightAlt, Tab, or a number */
    if (n[0] && !n[1] && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    if (n[0] && !n[1] && n[0] >= 'a' && n[0] <= 'z') return n[0] - 32;
    static const struct { const char *n; int vk; } t[] = { { "SpaceBar", VK_SPACE }, { "LeftControl", VK_LCONTROL }, { "RightControl", VK_RCONTROL }, { "LeftShift", VK_LSHIFT },
        { "RightShift", VK_RSHIFT }, { "LeftAlt", VK_LMENU }, { "RightAlt", VK_RMENU }, { "Tab", VK_TAB } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!_stricmp(n, t[i].n)) return t[i].vk;
    if ((n[0] == 'F' || n[0] == 'f') && atoi(n + 1) >= 1 && atoi(n + 1) <= 24) return VK_F1 + atoi(n + 1) - 1;
    if (!_stricmp(n, "Insert")) return VK_INSERT; if (!_stricmp(n, "Home")) return VK_HOME; if (!_stricmp(n, "End")) return VK_END;
    return atoi(n);
}
static char g_uikey[32];   /* the window key (defined with its default further down) */
static void fly_help(void) {
    char l1[120]; snprintf(l1, sizeof l1, "FLY cheat ON - %s flies / lands; Cheats window (%s) > Fly turns the cheat off; I this help\n", g_flykey, g_uikey);
    char t[300]; snprintf(t, sizeof t, "%s%s", l1,
                       "W A S D fly level where the camera faces, Space up, Ctrl down, Shift fast\n"
                       "Stances, attacks, camera work as usual; Space/Ctrl/Shift do not dodge or block");
    A->show_message(A, t, 20.f);
}
static char hk_key(void *pc, FKey *key, int event, float amount, char gp) {
    if (g_pawn_fly && key && pc == A->get_local_player_controller(A) && (key->name == k_up.name || key->name == k_down.name || key->name == n_lshift || key->name == n_rshift || key->name == n_help)) {
        if (key->name == n_help && event == 0) fly_help();
        return 1;                                                                  /* handled: the fly keys do not dodge / block / guard */
    }
    return o_key(pc, key, event, amount, gp);
}

static int call0(void *obj, const char *fn, void **args, void *ret) { void *f = obj ? A->find_function(A, obj, fn) : NULL; return f && A->call_function_params(A, obj, f, args, ret); }
static void *objp(void *o, const char *prop) { void **p = o ? A->prop_ptr(A, o, prop) : NULL; return p ? *p : NULL; }
static void *pawn_(void) { void *pc = A->get_local_player_controller(A), *p = NULL; call0(pc, "K2_GetPawn", NULL, &p); return p; }
static void key_make(FKey *k, const char *name) { memset(k, 0, sizeof *k); if (A->fname_make(A, name, &k->name)) A->log(A, "cheats: key name '%s' not made", name); }

/* ---- fly */
static void set_mode(void *cmc, int mode) { unsigned char m = (unsigned char)mode, c = 0; void *a[2] = { &m, &c }; call0(cmc, "SetMovementMode", a, NULL); }
static int cur_mode(void *cmc) { unsigned char *m = cmc ? A->prop_ptr(A, cmc, "MovementMode") : NULL; return m ? *m : -1; }
static void *anim_inst(void *pawn) { void *mesh = objp(pawn, "Mesh"), *ai = NULL; call0(mesh, "GetAnimInstance", NULL, &ai); return ai; }
static void *hover_on(void *pawn) {                                               /* plays the hover in the Cinematic slot of any character; the montage or NULL */
    if (!g_clip && !g_clip_tried) { g_clip_tried = 1; g_clip = A->load_object(A, g_clip_path); A->log(A, "cheats: hover clip %s -> %p%s", g_clip_path, g_clip, g_clip ? "" : " (zzzCheats.pak not mounted? flying without the pose)"); if (g_clip) A->add_to_root(A, g_clip, 1); }
    void *ai = anim_inst(pawn), *m = NULL; if (!ai || !g_clip) return NULL;
    float bin = 0.3f, bout = 0.3f, rate = 1.f, trig = -1.f, start = 0.f; int loops = 100000; void *a[8] = { &g_clip, &n_slot, &bin, &bout, &rate, &loops, &trig, &start };
    call0(ai, "PlaySlotAnimationAsDynamicMontage", a, &m); return m;
}
static void hover_play(void *pawn) { g_montage = hover_on(pawn); A->log(A, "cheats: hover montage %p (slot Cinematic)", g_montage); }
static void hover_stop(void *pawn) { void *ai = anim_inst(pawn); float *cw = ai ? A->prop_ptr(A, ai, "m_fCinematicWeight") : NULL; if (cw) *cw = 0.f; float bout = 0.3f; void *a[2] = { &bout, &n_slot }; if (ai) call0(ai, "StopSlotAnimation", a, NULL); g_montage = NULL; }
static void fly_begin(void *pawn) {
    void *cmc = objp(pawn, "CharacterMovement"); if (!cmc) { A->log(A, "cheats: fly: no CharacterMovement"); return; }
    int m = cur_mode(cmc); set_mode(cmc, 5);
    float l[3] = { 0 }; A->test_get_location(A, pawn, l);
    A->log(A, "cheats: fly ON at %.0f %.0f %.0f: movement mode %d -> %d", l[0], l[1], l[2], m, cur_mode(cmc));
    float up[3] = { l[0], l[1], l[2] + 40.f }; unsigned char sweep = 1, tele = 0, hit[0x88] = { 0 }, ok = 0; void *sa[4] = { up, &sweep, hit, &tele }; call0(pawn, "K2_SetActorLocation", sa, &ok);
    g_fz = up[2]; g_pawn_fly = pawn; hover_play(pawn);
}
static void fly_end(void *pawn, const char *why) {
    if (!pawn) { g_pawn_fly = NULL; return; }
    void *cmc = objp(pawn, "CharacterMovement"); if (cmc) set_mode(cmc, 3);   /* falling: lands by itself, the game's landing code sets walking */
    hover_stop(pawn); g_pawn_fly = NULL;
    float l[3] = { 0 }; A->test_get_location(A, pawn, l); A->log(A, "cheats: fly OFF (%s) at %.0f %.0f %.0f, mode %d", why, l[0], l[1], l[2], cur_mode(cmc));
}
/* A free camera (a mod that looks through a spawned CameraActor): while a CameraActor is the view target the keys steer that camera, not the body, so the
 * flight input is ignored (the body hovers where it is). */
static int cam_elsewhere(void) {
    void *cm = objp(A->get_local_player_controller(A), "PlayerCameraManager"); void **vt = cm ? (void **)A->prop_ptr(A, cm, "ViewTarget") : NULL;
    return vt && *vt && A->is_a(A, *vt, "CameraActor");
}
static void fly_tick(void *pawn, float dt) {
    void *cmc = objp(pawn, "CharacterMovement"); if (!cmc) return;
    if (cur_mode(cmc) != 5) set_mode(cmc, 5);                                    /* landing / attacks may put walking back */
    void *cm = objp(A->get_local_player_controller(A), "PlayerCameraManager"); float rot[3] = { 0 }; call0(cm, "GetCameraRotation", NULL, rot);
    float yaw = rot[1] * 3.14159265f / 180.f, fx = cosf(yaw), fy = sinf(yaw);   /* level flight: only the camera's yaw steers, its pitch never sinks / lifts you */
    float f = (float)(vdown('W') - vdown('S')), r = (float)(vdown('D') - vdown('A')), u = (float)(vdown(vk_up) - vdown(vk_down));
    if (cui_is_open()) f = r = u = 0.f;                                         /* the window has the keys: hover in place */
    { static int was; int now = cam_elsewhere(); if (now) f = r = u = 0.f; if (now != was) { was = now; A->log(A, "cheats: fly: %s", now ? "another camera (a free camera mod) has the view - flight input ignored" : "the character's own view again - flight input back"); } }
    float sp = g_speed * (vdown(VK_SHIFT) ? 2.5f : 1.f), v[3] = { (fx * f - fy * r) * sp, (fy * f + fx * r) * sp, u * sp };   /* W A S D level, Space / Ctrl the only way up / down */
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (len > sp) { v[0] *= sp / len; v[1] *= sp / len; v[2] *= sp / len; }
    /* writing CharacterMovement.Velocity moves nothing (the locomotion is root-motion driven: Flying + anim root motion overrides the velocity), so the
     * actor is swept to the new spot itself (K2_SetActorLocation, sweep on: walls and floors still block); the Flying mode only switches gravity off. */
    float *vel = A->prop_ptr(A, cmc, "Velocity"); if (vel) vel[0] = vel[1] = vel[2] = 0.f;
    if (dt > 0.1f) dt = 0.1f;
    float l0[3]; if (A->test_get_location(A, pawn, l0)) {
        float d[3] = { v[0] * dt, v[1] * dt, v[2] * dt + (g_fz - l0[2]) };          /* + back to the held height if the game moved it */
        if (fabsf(g_fz - l0[2]) > 30.f) { static ULONGLONG last_z; ULONGLONG nw = GetTickCount64(); if (nw - last_z > 1000) { last_z = nw; A->log(A, "cheats: fly: the game moved the flyer %+.0f uu vertically - held at %.0f", l0[2] - g_fz, g_fz); } }
        for (int pass = 0; pass < 2; pass++) {                                   /* sweep; on a blocking hit slide the rest along the surface once */
            float nl[3] = { l0[0] + d[0], l0[1] + d[1], l0[2] + d[2] }; unsigned char sweep = 1, tele = 0, hit[0x88] = { 0 }, ok = 0; void *sa[4] = { nl, &sweep, hit, &tele };
            if (!call0(pawn, "K2_SetActorLocation", sa, &ok) || !(hit[0] & 1)) break;   /* FHitResult: bBlockingHit bit 0, Time +0x04, ImpactNormal +0x30 */
            float t = *(float *)(hit + 4), *n = (float *)(hit + 0x30), rest = 1.f - t, dn;
            { static ULONGLONG last_hit; ULONGLONG nw = GetTickCount64(); if (nw - last_hit > 1000) { last_hit = nw; A->log(A, "cheats: fly blocked (t %.2f, normal %.2f %.2f %.2f, start penetrating %d)", t, n[0], n[1], n[2], (hit[0] >> 1) & 1); } }
            if (!A->test_get_location(A, pawn, l0)) break;
            for (int k = 0; k < 3; k++) d[k] *= rest;
            dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2]; for (int k = 0; k < 3; k++) d[k] -= dn * n[k];
        }
        float la[3]; if (A->test_get_location(A, pawn, la)) g_fz = la[2];
    }
    unsigned char playing = 0; void *ma[1] = { &g_montage }; void *ai = anim_inst(pawn);
    float *cw = ai ? A->prop_ptr(A, ai, "m_fCinematicWeight") : NULL; if (cw) *cw = 1.f;   /* the AnimBP blends its "Cinematic" slot by this weight (0 outside cutscenes: the montage would play invisibly) */
    static ULONGLONG last_try; ULONGLONG now = GetTickCount64();
    if (g_clip && now - last_try > 1000 && (!g_montage || (ai && call0(ai, "Montage_IsPlaying", ma, &playing) && !(playing & 1)))) { last_try = now; hover_play(pawn); }   /* an attack / hit stopped the slot: at most one retry a second */
    if (u != 0.f) { static ULONGLONG last_u; ULONGLONG nw = GetTickCount64(); if (nw - last_u > 100) { last_u = nw; float l[3] = { 0 }; A->test_get_location(A, pawn, l); A->log(A, "cheats: fly %s z %.0f (dt %.3f)", u > 0 ? "up" : "down", l[2], dt); } }
    g_fly_log_t += dt; if (g_fly_log_t > 1.f) { g_fly_log_t = 0; float l[3] = { 0 }; A->test_get_location(A, pawn, l); A->log(A, "cheats: fly at %.0f %.0f %.0f vel %.0f %.0f %.0f (keys f%+.0f r%+.0f u%+.0f) mode %d", l[0], l[1], l[2], v[0], v[1], v[2], f, r, u, cur_mode(cmc)); }
}

static void toggle(void *u);
static void world_keep(void *p);
/* Cheats are for playing alone. While any other player is in this game (you joined someone, or someone joined you) every cheat is
 * OFF: the toggles keep what you chose, but nothing is applied and the actions are refused; the tab says why. They come back when you are alone again. */
static int players(void **ps, void **pawn, int cap);
static volatile LONG g_mp;                                                          /* 1 = another player is in this game and the cheats are off for it */
static volatile LONG g_offwp = 1;                                                   /* setting off_with_players (default 1). 0: cheats keep working with other players and
                                                                                       joiners see the notice "Host has cheats enabled" (API 2.35 net_notice) */
static int g_mp_n;                                                                  /* players seen at the last check */
int cheats_mp(int *n) { if (n) *n = g_mp_n; return g_mp != 0; }
int cheats_mp_allowed(void) { return !g_offwp; }
static void notice_update(void) {                                                    /* the host's notice for joiners: only while the setting lets cheats work with others */
#ifdef ABS_HAS_NET_NOTICE
    if (A->size < offsetof(AbsApi, net_notice) + sizeof(void *) || !A->net_notice) return;
    char t[100] = ""; static char last[100] = "-";
    if (!g_offwp) { int w = snprintf(t, sizeof t, "Host has cheats enabled"); int first = 1;
        for (int i = 0; i < C_N; i++) if (g_on[i] && w < (int)sizeof t - 24) { w += snprintf(t + w, sizeof t - (size_t)w, "%s%s", first ? " (on: " : ", ", C_LABEL[i]); first = 0; }
        if (!first && w < (int)sizeof t - 2) snprintf(t + w, sizeof t - (size_t)w, ")"); }
    if (strcmp(t, last)) { snprintf(last, sizeof last, "%s", t); A->net_notice(A, t); A->log(A, "cheats: notice for joiners: %s", t[0] ? t : "(none)"); }
#endif
}
/* host / solo = the own pawn has authority (Role 3). The "host only" rows move or spawn things, which a joined client cannot do */
static int is_host(void *p) { unsigned char *r = p ? A->prop_ptr(A, p, "Role") : NULL; return r && *r == 3; }
/* ---- the 0.25 s keeper + per-frame fly */
static void keep(void *u) {
    (void)u; void *p = pawn_(); world_keep(p);
    notice_update();
    { void *ps[16], *pw[16]; int n = players(ps, pw, 16), mp = n > 1 && g_offwp; g_mp_n = n;
      { static int said; if (n > 1 && !g_offwp && !said) { said = 1; A->log(A, "cheats: %d player(s) in this game - cheats stay ON (setting off_with_players = 0; joiners are told)", n); } if (n < 2) said = 0; }
      if (mp != (int)g_mp) {
          InterlockedExchange(&g_mp, mp);
          A->log(A, "cheats: %d player(s) in this game - cheats %s", n, mp ? "OFF (co-op / PvP: another player is here; the toggles keep their state)" : "back (alone again)");
          A->show_message(A, mp ? "Cheats are off: another player is in this game" : "Cheats are back: you are alone in this game", 3.f);
          if (mp) g_flying = 0;                                                    /* land; tick() ends the flight */
      } }
    if (!p) return;
    int host = is_host(p);
    int on[C_N]; for (int i = 0; i < C_N; i++) on[i] = g_on[i] && !g_mp;          /* what is applied now */
    if (on[C_GOD] && !host) { static int warned; if (!warned++) A->log(A, "cheats: god mode is host only - not applied on this joined client"); }
    if (on[C_SHARDS]) { void *atk = objp(p, "m_AttackComponent"); float mx = 0.f; if (call0(atk, "GetShardsMax", NULL, &mx) && mx > 0.f) { void *a[1] = { &mx }; call0(atk, "BPF_SetShards", a, NULL); } }
    if (host && (on[C_GOD] || g_applied_god == 1)) { int want = on[C_GOD]; AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci;
        if (A->char_info(A, p, &ci) && ci.invincible != want) { static int n; A->test_set_invincible(A, p, want); if (n++ < 3 || n % 200 == 0) A->log(A, "cheats: god mode %d set on the player (#%d; the title-screen pawn refuses it)", want, n); } g_applied_god = want; }
    if (on[C_STAMINA]) { void *dc = objp(p, "m_DefenseComponent"); float *cur = dc ? A->prop_ptr(A, dc, "m_fCurrentGuard") : NULL; AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci;
        if (cur && A->char_info(A, p, &ci) && ci.max_guard > 0.f && *cur < ci.max_guard) *cur = ci.max_guard; }
    static ULONGLONG last_dm; ULONGLONG nowt = GetTickCount64();   /* the title-screen pawn refuses damage_mod: retry every 5 s, not every 0.25 s (a log line each) */
    if (g_applied_onehit != on[C_ONEHIT] && (nowt - last_dm > 5000 || g_mp)) { last_dm = nowt; if (A->damage_mod(A, p, "damage_dealt", on[C_ONEHIT] ? 100.f : 1.f)) { g_applied_onehit = on[C_ONEHIT]; A->log(A, "cheats: damage_dealt x%d", g_on[C_ONEHIT] ? 100 : 1); } }
}
static void tick(float dt, void *u) {
    (void)u; int want = g_on[C_FLY] && g_flying && !g_mp; void *p = want || g_pawn_fly ? pawn_() : NULL;
    if (want && p && !is_host(p)) { static int warned; if (!warned++) { A->log(A, "cheats: fly is host only - not flying on this joined client"); A->show_message(A, "Fly: host only", 2.f); } want = 0; }
    if (g_pawn_fly && p != g_pawn_fly) { A->log(A, "cheats: fly: the pawn changed (respawn / map) - landed (cheat stays enabled)"); g_pawn_fly = NULL; g_montage = NULL; g_flying = 0; return; }
    if (want && p) { if (!g_pawn_fly) fly_begin(p); fly_tick(p, dt); }
    else if (!want && g_pawn_fly) fly_end(g_pawn_fly, g_on[C_FLY] ? "fly key" : "cheat disabled");
}
/* test only (plugin.cfg test_fly = <start s>,<duration s>): Fly on that long after a MainMap world appears, then off again - the loader menu reads real keys, scripts cannot press it */
static float g_tf_start, g_tf_len;
static void tf_on(void *u) { (void)u; if (!g_on[C_FLY]) toggle((void *)(intptr_t)C_FLY); else g_flying = 1; }
static void tf_off(void *u) { (void)u; if (g_on[C_FLY]) toggle((void *)(intptr_t)C_FLY); }
static char g_test_add[160], g_test_add2[160];
static void test_add_run(void *u);
static int g_tw_step; static float g_tw_start; static void tw_step(void *u);   /* test_world (World tab section) */
static void npcl_refresh(void *u);
static void on_world(const char *map, void *u) { (void)u;
    npcl_refresh(NULL);
    if (g_tf_start > 0 && map && strstr(map, "MainMap")) { A->set_timer(A, g_tf_start, 0, tf_on, NULL); A->set_timer(A, g_tf_start + g_tf_len, 0, tf_off, NULL); A->log(A, "cheats: test_fly armed (%.0f s, %.0f s long)", g_tf_start, g_tf_len); } if (map && strstr(map, "MainMap")) { const char *sp[2] = { g_test_add, g_test_add2 }; for (int k = 0; k < 2; k++) if (sp[k][0]) { A->set_timer(A, (float)atof(sp[k]), 0, test_add_run, (void *)sp[k]); A->log(A, "cheats: test_add armed: %s", sp[k]); } }
    if (g_tw_start > 0 && map && strstr(map, "MainMap") && !g_tw_step) { A->set_timer(A, g_tw_start, 0, tw_step, NULL); A->log(A, "cheats: test_world armed (%.0f s)", g_tw_start); }
    A->log(A, "cheats: world %s - re-applying (shards %d god %d stamina %d onehit %d)", map, g_on[0], g_on[1], g_on[2], g_on[3]); g_applied_onehit = -1; g_applied_god = -1; g_pawn_fly = NULL; g_montage = NULL; g_flying = 0; }

/* ---- menu */
static int has_rows(void) { return A->size >= offsetof(AbsApi, ui_set) + sizeof(void *); }   /* loader API 2.27: the toggles are also rows in the F2 Mods page section */
static void set_on(int i, int on, const char *how) {
    if (g_on[i] == on) return;                                                     /* the rows' first call at start carries the value we already have */
    g_on[i] = on;
    A->kv_set(A, C_KEY[i], g_on[i] ? "1" : "0");
    if (i == C_FLY) g_flying = g_on[i];                                            /* enabling starts flying at once; the fly key lands / takes off from then on */
    char m[96]; snprintf(m, sizeof m, "%s: %s", C_LABEL[i], g_on[i] ? "ON" : "OFF"); if (i == C_FLY && g_on[i]) fly_help(); else A->show_message(A, m, 2.f); A->log(A, "cheats: %s %s", how, m);
    if (i != C_FLY) keep(NULL);
}
static void toggle(void *u) {                                                      /* the window, test timers: flip, then tell the loader's row (ui_set: no callback back) */
    int i = (int)(intptr_t)u; set_on(i, !g_on[i], "menu");
    if (has_rows()) A->ui_set(A, C_KEY[i], g_on[i]);
}
static void on_row(int on, void *u) { set_on((int)(intptr_t)u, on != 0, "F1 > Mods row"); }   /* the loader's row (F2 Mods page, a bound ui_<key>): the window reads g_on, so it follows */
static void on_fly_key(int vk, void *u) {
    (void)vk; (void)u; if (!g_on[C_FLY]) return;
    if (cui_is_open()) { A->log(A, "cheats: fly key %s ignored - the Cheats window is open", g_flykey); return; }
    g_flying = !g_flying; float l[3] = { 0 }; void *p = pawn_(); if (p) A->test_get_location(A, p, l);
    A->log(A, "cheats: fly key %s -> %s at z %.0f", g_flykey, g_flying ? "flying" : "landing", l[2]);
    char m[96]; snprintf(m, sizeof m, g_flying ? "Flying (%s lands, I help)" : "Landing (%s flies again)", g_flykey); A->show_message(A, m, 2.f);
}
static void on_ui_key(int vk, void *u) { (void)vk; (void)u; cui_set_open(!cui_is_open()); A->log(A, "cheats: window %s (toggle key %s)", cui_is_open() ? "open" : "closed", g_uikey); }
static void open_ui(void *u) { (void)u; cui_set_open(1); A->log(A, "cheats: window open (F1 > Mods row)"); }
static void heal_me(void *u) { (void)u; void *p = pawn_(); int ok = p && A->test_heal(A, p); A->show_message(A, ok ? "Healed" : "Heal: no character", 2.f); A->log(A, "cheats: heal -> %d", ok); }

/* ---- the window's side: values it reads, requests it queues (all run on the game thread) */
static CRITICAL_SECTION g_cs; static Catalog g_cat; static volatile LONG g_cat_state, g_cat_serial, g_status_serial; static char g_status[160];
static char g_uikey[32] = "F10"; static int g_defcount = 1, g_defequip, vk_ui;
int cheats_count(void) { return C_N; }
const char *cheats_label(int i) { return i >= 0 && i < C_N ? C_LABEL[i] : ""; }
int cheats_is_on(int i) { return i >= 0 && i < C_N && g_on[i]; }
void cheats_request_toggle(int i) { if (i >= 0 && i < C_N) A->run_on_game_thread(A, toggle, (void *)(intptr_t)i); }
static void set_status(const char *fmt, ...);
static int mp_refused(const char *what) { if (!g_mp) return 0; set_status("%s: refused - another player is in this game (cheats are off in co-op / PvP)", what); A->log(A, "cheats: %s refused - %d players in this game", what, g_mp_n); return 1; }
void cheats_request_heal(void) { if (mp_refused("Heal me")) return; A->run_on_game_thread(A, heal_me, NULL); }
float cheats_speed(void) { return g_speed; }
void cheats_set_speed(float v) { if (v >= 50.f && v <= 20000.f) g_speed = v; }
const char *cheats_fly_key_name(void) { return g_flykey; }
const char *cheats_ui_key_name(void) { return g_uikey; }
int cheats_default_count(void) { return g_defcount; }
int cheats_default_equip(void) { return g_defequip; }
unsigned cheats_catalog_serial(void) { return (unsigned)g_cat_serial; }
int cheats_catalog_state(void) { return (int)g_cat_state; }
Catalog *cheats_catalog_lock(void) { EnterCriticalSection(&g_cs); return &g_cat; }
void cheats_catalog_unlock(void) { LeaveCriticalSection(&g_cs); }
void cheats_status(char *out, int cap, unsigned *serial) { EnterCriticalSection(&g_cs); snprintf(out, (size_t)cap, "%s", g_status); if (serial) *serial = (unsigned)g_status_serial; LeaveCriticalSection(&g_cs); }
static void set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); EnterCriticalSection(&g_cs); vsnprintf(g_status, sizeof g_status, fmt, ap); InterlockedIncrement(&g_status_serial); LeaveCriticalSection(&g_cs); va_end(ap);
    A->log(A, "cheats: %s", g_status);
}

/* ---- item catalog: every loaded BaseItemData asset (gear, weapons, powers, mod items). One scan of the live objects, started by the window when the tab opens or by Refresh. */
#define SCAN_MAX 6000
static void *g_scan[SCAN_MAX]; static int g_scan_n;
static void scan_cb(void *o, void *u) { (void)u; if (g_scan_n < SCAN_MAX && A->is_a(A, o, "BaseItemData")) g_scan[g_scan_n++] = o; }
/* an FText property as text: INVTEXT("x") -> x, NSLOCTEXT("ns","key","src") -> src, a plain quoted string -> its last quoted part, else the raw text */
static void text_of(const char *in, char *out, int cap) {
    const char *q[8]; int nq = 0; out[0] = 0;
    for (const char *p = in; *p && nq < 8; p++) if (*p == '"') { q[nq++] = p; }
    if (nq >= 2) {
        const char *a = q[nq - 2] + 1, *b = q[nq - 1]; if (!strncmp(in, "NSLOCTEXT", 9) && nq >= 6) { a = q[4] + 1; b = q[5]; }
        int n = (int)(b - a); if (n >= cap) n = cap - 1; memcpy(out, a, (size_t)n); out[n] = 0;
        for (char *r = out, *w = out;; r++) { if (*r == '\\' && r[1]) continue; *w++ = *r; if (!*r) break; }   /* the engine escapes quotes in the text: drop the backslashes */
    } else snprintf(out, (size_t)cap, "%s", in);
}
static void cat_fill(CatItem *c, void *o) {
    char buf[320], path[256], nm[64]; memset(c, 0, sizeof *c);
    A->object_path(A, o, path, sizeof path); snprintf(c->path, sizeof c->path, "%s", path);
    c->mod = strncmp(path, "/Game/Mods/", 11) == 0;
    A->object_name(A, o, nm, sizeof nm);
    if (A->get_property_text(A, o, "m_ItemName", buf, sizeof buf) > 0) text_of(buf, c->name, sizeof c->name);
    if (!c->name[0] || strchr(c->name, '/') || strstr(c->name, "NSLOCTEXT")) snprintf(c->name, sizeof c->name, "%s", nm);   /* a key or an empty text: the asset's own name */
    if (A->get_property_text(A, o, "m_EquipmentSlot", buf, sizeof buf) > 0) { const char *colons = strrchr(buf, ':'); snprintf(c->slot, sizeof c->slot, "%s", colons ? colons + 1 : buf); }
    c->kind = A->is_a(A, o, "BaseWeaponData") ? CAT_WEAPON : A->is_a(A, o, "UsableItemData") ? CAT_POWER : CAT_GEAR;
    if (c->kind == CAT_POWER && !c->slot[0]) snprintf(c->slot, sizeof c->slot, "Item");
    if (c->kind == CAT_WEAPON && !c->slot[0]) snprintf(c->slot, sizeof c->slot, "Weapon");
    if (c->kind == CAT_GEAR) { unsigned char *arr = A->prop_ptr(A, o, "m_Materials"); if (arr) { int num = *(int *)(arr + 8); c->nvar = num < 0 ? 0 : num > CAT_VARS ? CAT_VARS : num; } }
    c->owned = A->inventory_index(A, o) >= 0;
}
static void cat_build(void *u) {
    (void)u; ULONGLONG t0 = GetTickCount64(); g_scan_n = 0; A->for_each_object(A, scan_cb, NULL);
    Catalog nc = { 0 }; int skipped = 0;
    for (int i = 0; i < g_scan_n; i++) {
        char path[256]; A->object_path(A, g_scan[i], path, sizeof path);
        if (strncmp(path, "/Game/", 6) || strstr(path, "Default__") || strstr(path, "/Engine/") || strstr(path, "/QuestItems/")) { skipped++; continue; }
        CatItem c; cat_fill(&c, g_scan[i]);
        if (cat_add(&nc, &c) == -2) { A->log(A, "cheats: catalog full (%d)", CAT_MAX); break; }
        if (nc.n <= 12) A->log(A, "cheats: catalog sample: %s | name '%s' slot '%s' kind %d variants %d owned %d", c.path, c.name, c.slot, c.kind, c.nvar, c.owned);
    }
    cat_sort(&nc);
    EnterCriticalSection(&g_cs); cat_free(&g_cat); g_cat = nc; LeaveCriticalSection(&g_cs);
    int ng = 0, nw = 0, np = 0, nm = 0; for (int i = 0; i < nc.n; i++) { ng += nc.it[i].kind == CAT_GEAR; nw += nc.it[i].kind == CAT_WEAPON; np += nc.it[i].kind == CAT_POWER; nm += nc.it[i].mod; }
    InterlockedIncrement(&g_cat_serial); g_cat_state = CU_BUILD_READY;
    A->log(A, "cheats: catalog built in %llu ms: %d items (%d gear, %d weapons, %d powers, %d from mods); %d objects scanned of that class, %d skipped", GetTickCount64() - t0, nc.n, ng, nw, np, nm, g_scan_n, skipped);
}
void cheats_request_catalog(void) { if (g_cat_state == CU_BUILD_RUNNING) return; g_cat_state = CU_BUILD_RUNNING; A->run_on_game_thread(A, cat_build, NULL); }

/* ---- add to inventory (game thread). Gear may be added several times (duplicates are legal in a save); a weapon or power is a single item: an owned one is not added twice. */
typedef struct { char path[200]; int count, variant, equip; } AddReq;
static void do_add(void *u) {
    AddReq *r = (AddReq *)u; void *it = A->find_object(A, r->path);
    if (!it) { set_status("Add: %s is not loaded", r->path); free(r); return; }
    char nm[64]; A->object_name(A, it, nm, sizeof nm);
    int power = A->is_a(A, it, "UsableItemData"), single = power || A->is_a(A, it, "BaseWeaponData"), have = A->inventory_index(A, it), idx = have, added = 0, n = single ? 1 : cat_count_clamp(r->count);
    if (single && have >= 0) set_status("%s: already owned (inventory %d), not added again", nm, have);
    else for (int k = 0; k < n; k++) { int eq = r->equip && !power && k == n - 1; int i2 = A->inventory_add(A, it, r->variant, eq); if (i2 < 0) { set_status("Add %s failed (no world / inventory yet?)", nm); break; } idx = i2; added++; }
    if (added) set_status("Added %s x%d (colour %d) -> inventory %d%s", nm, added, r->variant, idx, r->equip ? (power ? "" : ", equipped") : "");
    if (added) {   /* read the inventory back: the index the game reports and the entry's own text (material index, count, equipped flag) */
        int back = A->inventory_index(A, it); void *inv = objp(pawn_(), "m_InventoryComponent"); char *txt = (char *)malloc(1 << 20);
        if (txt && inv && A->get_property_text(A, inv, "m_Inventory", txt, 1 << 20) > 0) {
            char *at = strstr(txt, nm), *last = at; while (at) { last = at; at = strstr(at + 1, nm); }
            if (last) { char *b = last - 60 < txt ? txt : last - 60, *e = last + 260; if (e > txt + strlen(txt)) e = txt + strlen(txt); *e = 0; A->log(A, "cheats: readback inventory_index %d (added at %d); entry text: ...%s", back, idx, b); }
            else A->log(A, "cheats: readback inventory_index %d (added at %d); '%s' not found in the m_Inventory text", back, idx, nm);
        } else A->log(A, "cheats: readback inventory_index %d (added at %d); m_Inventory text not available", back, idx);
        free(txt);
    }
    if (idx >= 0 && r->equip && power) { int ok = A->inventory_equip_slot(A, idx, 0); A->log(A, "cheats: power %s equipped into slot 0 -> %d", nm, ok); }
    EnterCriticalSection(&g_cs); for (int i = 0; i < g_cat.n; i++) if (!strcmp(g_cat.it[i].path, r->path)) g_cat.it[i].owned = A->inventory_index(A, it) >= 0; InterlockedIncrement(&g_cat_serial); LeaveCriticalSection(&g_cs);
    free(r);
}
void cheats_request_add(const char *path, int count, int variant, int equip) {
    if (mp_refused("Add to inventory")) return;
    AddReq *r = (AddReq *)calloc(1, sizeof *r); if (!r) return;
    snprintf(r->path, sizeof r->path, "%s", path); r->count = count; r->variant = variant; r->equip = equip; A->run_on_game_thread(A, do_add, r);
}

/* test only: test_add = <delay s>,<search words>,<variant>,<equip>; the first catalog match is added the same way the window's button does it */
static void test_add_run(void *u) {
    const char *spec = (const char *)u; char buf[160]; snprintf(buf, sizeof buf, "%s", spec);
    char *f[5] = { 0 }; int n = 0; for (char *p = strtok(buf, ","); p && n < 5; p = strtok(NULL, ",")) f[n++] = p;
    if (n < 2) { A->log(A, "cheats: test_add '%s' malformed", spec); return; }
    if (g_cat_state != CU_BUILD_READY) cat_build(NULL);
    int idx[CAT_MAX], m; EnterCriticalSection(&g_cs); m = cat_filter(&g_cat, f[1], CAT_ALL, idx, CAT_MAX); char path[200] = ""; int owned = 0;
    if (m) { snprintf(path, sizeof path, "%s", g_cat.it[idx[0]].path); owned = g_cat.it[idx[0]].owned; } LeaveCriticalSection(&g_cs);
    A->log(A, "cheats: test_add '%s': %d matches, first %s (owned %d)", f[1], m, path[0] ? path : "-", owned);
    EnterCriticalSection(&g_cs); for (int i = 0; i < m && i < 40; i++) { const CatItem *c = &g_cat.it[idx[i]]; A->log(A, "cheats: test_add match %d: %s | %s | %s | %d variants | owned %d", i, c->path, c->name, c->slot, c->nvar, c->owned); } LeaveCriticalSection(&g_cs);
    if (path[0]) cheats_request_add(path, 1, n > 2 ? atoi(f[2]) : 0, n > 3 ? atoi(f[3]) : 0);
}

/* ---- World tab (0.5.0): the world rows (teleport, spawn, NPC tools).  Everything runs on the game thread; the window reads a snapshot (WorldView) under g_cs. */
#define NPC_N 19
static const char *const NPC_LABEL[NPC_N] = { "Calbot (Trickster)", "Grunt", "Agile", "Avoid", "Absorb", "Parry", "Spartan", "Cadencer", "Distancer", "Sword (Forest)", "Sammo",
    "Mini-boss: Path (First Marked)", "Mini-boss: Swamp", "Mini-boss: Village", "Mini-boss: Coliseum", "Mini-boss: Pier", "Mini-boss: Lower tower", "Mini-boss: Drunken", "Passive (does nothing)" };
static const char *const NPC_PATH[NPC_N] = { "Trickster/Trickster", "Grunt/Grunt_v1", "Agile/Forest_Agile_v1", "Avoid/Forest_Avoid_v1", "Absorb/Forest_Absorb_v1", "Parry/Forest_Parry_v1",
    "Spartan/Forest_Spartan_v1", "Cadencer/City_Cadencer_v1", "Distancer/Tower_Distancer_v1", "Forest_Weaponized/Forest_Weaponized", "Sammo/Sammo_v1", "Forest_Path_MiniBoss/Path_MiniBoss",
    "Forest_Swamp_MiniBoss/Swamp_MiniBoss", "Forest_Village_MiniBoss/Village_MiniBoss", "City_Coliseum_MiniBoss/Coliseum_MiniBoss", "City_Pier_MiniBoss/Pier_MiniBoss",
    "Tower_Lower_MiniBoss/Lower_MiniBoss", "MiniBoss_Drunken/Drunken_MiniBoss", "PVE/AINothing/T1_AI_Nothing" };          /* under /Game/DB/AI/NPCs/, object name = last part */
static const float DOOR_PT[3] = { -41546.f, -20720.f, -160.f };                   /* gen_arena.py RETURN_PT: 300 uu in front of the hidden arena door, 110 up */
static const float ALTAR_PT[3] = { -42421.f, -23001.f, -462.f };                  /* PlayerStart135 next to BP_RespawnAltar_03_E3Demo3 */
#define NEAR_R 3000.f                                                              /* "NPCs near me" */
#define CAP_R 6000.f                                                               /* spawn cap radius */
#define CH_MAX 256
typedef struct { void **data; int num, max; } TArr;
static void *g_ch[CH_MAX]; static void *g_ch_mont[CH_MAX]; static int g_nch;     /* live AFightingCharacters (on_pawn_spawned / destroyed) + their hover montage */
static void *g_spawned[64]; static int g_nspawned;
static float g_spot[3]; static int g_have_spot;
static WorldView g_wv;                                                             /* snapshot for the window (g_cs) */
static volatile LONG g_w_player = 1, g_w_npc, g_w_npcmax = 8; static volatile float g_w_dil = 1.f;
/* The NPC picker lists the loader's own NPC list (API 2.34 npc_list: the F2 > Spawn NPC list = built-in groups + every mod's "npcs" /
 * "bots" content entries), read on the game thread when a world loads and when the tab opens, so NPCs added by mods appear without a Cheats update.
 * Older loaders: the 19 built-in entries below. g_nl is guarded by g_cs (the render thread reads it). */
#define NPCL_MAX 512
typedef struct { char group[40], label[48], spawn[200], cls[140]; } NpcEnt;
static NpcEnt g_nl[NPCL_MAX]; static int g_nl_n; static volatile LONG g_nl_serial;
static void npcl_builtin(void) {
    g_nl_n = 0;
    for (int k = 0; k < NPC_N && g_nl_n < NPCL_MAX; k++) { NpcEnt *e = &g_nl[g_nl_n++]; const char *leaf = strrchr(NPC_PATH[k], '/');
        snprintf(e->group, sizeof e->group, "Built in"); snprintf(e->label, sizeof e->label, "%s", NPC_LABEL[k]);
        snprintf(e->spawn, sizeof e->spawn, "/Game/DB/AI/NPCs/%s.%s", NPC_PATH[k], leaf ? leaf + 1 : NPC_PATH[k]); e->cls[0] = 0; }
}
static void npcl_refresh(void *u) {                                                 /* game thread */
    (void)u; int n = -1;
#ifdef ABS_HAS_NPC_LIST
    if (A->size >= offsetof(AbsApi, npc_list) + sizeof(void *) && A->npc_list) n = A->npc_list(A, -1, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
#endif
    EnterCriticalSection(&g_cs);
    char keep[48]; snprintf(keep, sizeof keep, "%s", g_w_npc >= 0 && g_w_npc < g_nl_n ? g_nl[g_w_npc].label : "");
    if (n <= 0) npcl_builtin();
#ifdef ABS_HAS_NPC_LIST
    else { g_nl_n = 0; for (int i = 0; i < n && g_nl_n < NPCL_MAX; i++) { NpcEnt *e = &g_nl[g_nl_n];
               if (A->npc_list(A, i, e->group, sizeof e->group, e->label, sizeof e->label, e->spawn, sizeof e->spawn, e->cls, sizeof e->cls) == 1) g_nl_n++; } }
#endif
    int sel = 0; for (int i = 0; i < g_nl_n; i++) if (keep[0] && !strcmp(g_nl[i].label, keep)) { sel = i; break; }
    g_w_npc = sel; int got = g_nl_n;
    LeaveCriticalSection(&g_cs); InterlockedIncrement(&g_nl_serial);
    A->log(A, "cheats: NPC list: %d entries (%s)", got, n > 0 ? "the loader's list: built-in groups + mods' npcs / bots entries" : "built-in 19: this loader has no npc_list");
}
void cheats_npcl_refresh(void) { A->run_on_game_thread(A, npcl_refresh, NULL); }
int cheats_npcl_get(int i, char *group, int gcap, char *label, int lcap) {           /* render thread */
    EnterCriticalSection(&g_cs); int ok = i >= 0 && i < g_nl_n;
    if (ok) { if (group) snprintf(group, (size_t)gcap, "%s", g_nl[i].group); if (label) snprintf(label, (size_t)lcap, "%s", g_nl[i].label); }
    LeaveCriticalSection(&g_cs); return ok;
}
static void on_spawned(void *c, void *u) { (void)u; for (int i = 0; i < g_nch; i++) if (g_ch[i] == c) return; if (g_nch < CH_MAX) { g_ch_mont[g_nch] = NULL; g_ch[g_nch++] = c; } }
static void on_destroyed(void *c, int reason, void *u) {
    (void)u; (void)reason;
    for (int i = 0; i < g_nch; i++) if (g_ch[i] == c) { g_ch[i] = g_ch[g_nch - 1]; g_ch_mont[i] = g_ch_mont[g_nch - 1]; g_nch--; break; }
    for (int i = 0; i < g_nspawned; i++) if (g_spawned[i] == c) { g_spawned[i] = g_spawned[--g_nspawned]; break; }
}
static float dist3(const float a[3], const float b[3]) { float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }
static int live_npc(void *c) { AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; return A->char_info(A, c, &ci) && !ci.is_player && !ci.is_dead; }
static int npcs_within(void *me, float r, void **out, int cap) {                  /* live non-player characters within r of me */
    float m[3], l[3]; int n = 0; if (!me || !A->test_get_location(A, me, m)) return 0;
    for (int i = 0; i < g_nch && n < cap; i++) if (g_ch[i] != me && live_npc(g_ch[i]) && A->test_get_location(A, g_ch[i], l) && dist3(m, l) <= r) { if (out) out[n] = g_ch[i]; n++; }
    return n;
}
/* players in GameState.PlayerArray order; pawn = the live character whose PlayerState is that entry (works on a joined client too) */
static int players(void **ps, void **pawn, int cap) {
    AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; A->match_info(A, &mi);
    TArr *pa = mi.game_state ? A->prop_ptr(A, mi.game_state, "PlayerArray") : NULL; int n = 0;
    if (!pa || pa->num < 0 || pa->num > 64) return 0;
    for (int i = 0; i < pa->num && n < cap; i++) {
        ps[n] = pa->data[i]; pawn[n] = NULL; if (!ps[n]) continue;
        for (int k = 0; k < g_nch; k++) if (objp(g_ch[k], "PlayerState") == ps[n]) { pawn[n] = g_ch[k]; break; }
        n++;
    }
    return n;
}
static void player_name(void *ps, char *out, int cap) {
    char b[160]; out[0] = 0; if (!ps || A->get_property_text(A, ps, "PlayerName", b, sizeof b) <= 0) { snprintf(out, (size_t)cap, "?"); return; }
    char *s = b; size_t L = strlen(s); if (L >= 2 && s[0] == '"' && s[L - 1] == '"') { s[L - 1] = 0; s++; } snprintf(out, (size_t)cap, "%s", s);
}
/* own pawn or any actor -> loc: K2_TeleportTo (refuses spots inside geometry), else a plain non-sweeping K2_SetActorLocation */
static int warp(void *actor, const float t[3], const char *what) {
    int ok = A->test_teleport(A, actor, t, NULL), plain = 0;
    if (!ok) { float nl[3] = { t[0], t[1], t[2] }; unsigned char sweep = 0, tele = 1, hit[0x88] = { 0 }, r = 0; void *sa[4] = { nl, &sweep, hit, &tele }; plain = call0(actor, "K2_SetActorLocation", sa, &r) && (r & 1); }
    float l[3] = { 0 }; A->test_get_location(A, actor, l);
    if (actor == g_pawn_fly) g_fz = l[2];                                          /* flying: hold the new height, not the old one */
    A->log(A, "cheats: world %s -> %s; at %.0f %.0f %.0f (target %.0f %.0f %.0f, off by %.0f uu)", what, ok ? "teleported" : plain ? "teleport check failed, placed anyway" : "FAILED", l[0], l[1], l[2], t[0], t[1], t[2], dist3(l, t));
    return ok || plain;
}
static void fwd_of(void *actor, float out[3]) { if (!A->test_get_forward(A, actor, out)) { out[0] = 1.f; out[1] = out[2] = 0.f; } }
static void emote_name(int i, uint64_t *n) { char b[8]; snprintf(b, sizeof b, "%03d", i); A->fname_make(A, b, n); }
static int emotes_unlocked(void *p) {
    void *soc = objp(p, "m_SocialComponent"); int n = 0; if (!soc) return -1;
    for (int i = 1; i <= 53; i++) { uint64_t nm; unsigned char r = 0; emote_name(i, &nm); void *a[1] = { &nm }; if (call0(soc, "BPF_IsEmoteUnlocked", a, &r) && (r & 1)) n++; }
    return n;
}
static const char *const W_LABEL[W_N] = { "Teleport to the crosshair", "Go to player #", "Bring player # here", "Bring all players here", "Save my spot", "Go to saved spot",
    "Go to the hidden arena door", "Go to the first altar", "Spawn NPC in front of me", "NPCs near me: attack me", "NPCs near me: kill", "Remove the NPCs I spawned",
    "Game speed", "Game debug menu", "Unlock all 53 stock emotes" };
static const unsigned W_HOST = (1u << W_TPAIM) | (1u << W_GOTO) | (1u << W_BRING) | (1u << W_BRINGALL) | (1u << W_LOADSPOT) | (1u << W_DOOR) | (1u << W_ALTAR) |
    (1u << W_SPAWN) | (1u << W_AGGRO) | (1u << W_KILL) | (1u << W_CLEAR) | (1u << W_SPEED);
static void world_do(void *u) {
    int w = (int)(intptr_t)u; void *p = pawn_(), *pc = A->get_local_player_controller(A); float me[3] = { 0 }, f[3];
    if (w < 0 || w >= W_N) return;
    if (!p) { set_status("%s: no character", W_LABEL[w]); return; }
    if ((W_HOST >> w & 1) && !is_host(p)) { set_status("%s: host only", W_LABEL[w]); return; }
    A->test_get_location(A, p, me); fwd_of(p, f);
    switch (w) {
    case W_TPAIM: {   /* camera ray (own pawn ignored), land 1 m short of the hit, 1 m up */
        void *cm = objp(pc, "PlayerCameraManager"), *ks = A->find_cdo(A, "KismetSystemLibrary"); float s[3] = { 0 }, r[3] = { 0 };
        if (!cm || !ks || !call0(cm, "GetCameraLocation", NULL, s) || !call0(cm, "GetCameraRotation", NULL, r)) { set_status("Teleport: no camera"); break; }
        float pi = r[0] * 3.14159265f / 180.f, ya = r[1] * 3.14159265f / 180.f, d[3] = { cosf(pi) * cosf(ya), cosf(pi) * sinf(ya), sinf(pi) }, e[3] = { s[0] + d[0] * 100000.f, s[1] + d[1] * 100000.f, s[2] + d[2] * 100000.f };
        float col[4] = { 0 }, t = 0.f; unsigned char ch = 0, cx = 0, dbg = 0, self = 1, hit[0x88] = { 0 }; TArr ign = { 0 }; int ret = 0;
        void *a[12] = { &p, s, e, &ch, &cx, &ign, &dbg, hit, &self, col, col, &t };
        if (!call0(ks, "LineTraceSingle", a, &ret) || !(ret & 0xFF)) { set_status("Teleport: the crosshair hits nothing"); break; }
        float *ip = (float *)(hit + 0x18), tg[3] = { ip[0] - d[0] * 100.f, ip[1] - d[1] * 100.f, ip[2] - d[2] * 100.f + 100.f };
        set_status("Teleport to the crosshair: %s", warp(p, tg, "crosshair") ? "done" : "failed"); break; }
    case W_GOTO: case W_BRING: {
        void *ps[16], *pw[16]; int n = players(ps, pw, 16), k = (int)g_w_player - 1; char nm[64];
        if (k < 0 || k >= n || !pw[k]) { set_status("%s: no player #%d (%d players)", W_LABEL[w], k + 1, n); break; }
        player_name(ps[k], nm, sizeof nm);
        if (pw[k] == p) { set_status("%s: #%d is you", W_LABEL[w], k + 1); break; }
        float o[3], of[3]; A->test_get_location(A, pw[k], o); fwd_of(pw[k], of);
        if (w == W_GOTO) { float t[3] = { o[0] + of[0] * 150.f, o[1] + of[1] * 150.f, o[2] + 20.f }; set_status("Go to %s: %s", nm, warp(p, t, "go to player") ? "done" : "failed"); }
        else { float t[3] = { me[0] + f[0] * 150.f, me[1] + f[1] * 150.f, me[2] + 20.f }; set_status("Bring %s: %s", nm, warp(pw[k], t, "bring player") ? "done" : "failed"); }
        break; }
    case W_BRINGALL: {
        void *ps[16], *pw[16]; int n = players(ps, pw, 16), moved = 0, j = 0;
        for (int i = 0; i < n; i++) if (pw[i] && pw[i] != p) { j++; float side = (float)((j + 1) / 2) * 120.f * (j & 1 ? 1.f : -1.f);
            float t[3] = { me[0] + f[0] * 150.f - f[1] * side, me[1] + f[1] * 150.f + f[0] * side, me[2] + 20.f }; moved += warp(pw[i], t, "bring all"); }
        set_status("Bring all players here: %d of %d other player(s) moved", moved, j); break; }
    case W_SAVESPOT: memcpy(g_spot, me, sizeof g_spot); g_have_spot = 1; set_status("Spot saved: %.0f %.0f %.0f (this session only)", me[0], me[1], me[2]); A->log(A, "cheats: world spot saved"); break;
    case W_LOADSPOT: if (!g_have_spot) { set_status("Go to saved spot: no spot saved"); break; } set_status("Go to saved spot: %s", warp(p, g_spot, "saved spot") ? "done" : "failed"); break;
    case W_DOOR: set_status("Go to the hidden arena door: %s", warp(p, DOOR_PT, "arena door") ? "done" : "failed"); break;
    case W_ALTAR: set_status("Go to the first altar: %s", warp(p, ALTAR_PT, "first altar") ? "done" : "failed"); break;
    case W_SPAWN: {
        int k = (int)g_w_npc, cap = (int)g_w_npcmax, near_ = npcs_within(p, CAP_R, NULL, 0);
        if (near_ >= cap) { set_status("Spawn: %d NPCs within 60 m already (max %d)", near_, cap); break; }
        if (g_nspawned >= 64) { set_status("Spawn: 64 spawned NPCs alive - remove some first"); break; }
        EnterCriticalSection(&g_cs); if (!g_nl_n) npcl_builtin(); if (k < 0 || k >= g_nl_n) k = 0;
        char path[200], cls[140], lab[48]; snprintf(path, sizeof path, "%s", g_nl[k].spawn); snprintf(cls, sizeof cls, "%s", g_nl[k].cls); snprintf(lab, sizeof lab, "%s", g_nl[k].label);
        LeaveCriticalSection(&g_cs);
        float t[3] = { me[0] + f[0] * 400.f, me[1] + f[1] * 400.f, me[2] }, yaw = atan2f(-f[1], -f[0]) * 180.f / 3.14159265f;
        void *c = A->npc_spawn(A, path, t, &yaw, cls[0] ? cls : NULL);
        if (!c) { set_status("Spawn %s: failed (archetype %s)", lab, path); break; }
        g_spawned[g_nspawned++] = c; on_spawned(c, NULL);
        AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; A->char_info(A, c, &ci); float l[3] = { 0 }; A->test_get_location(A, c, l);
        set_status("Spawned %s (%.0f hp) %.1f m in front", lab, ci.max_health, dist3(l, me) / 100.f);
        A->log(A, "cheats: world spawn %s: %s at %.0f %.0f %.0f, health %.1f / %.1f, %.0f uu from me", path, ci.name, l[0], l[1], l[2], ci.health, ci.max_health, dist3(l, me)); break; }
    case W_AGGRO: case W_KILL: {
        void *c[CH_MAX]; int n = npcs_within(p, NEAR_R, c, CH_MAX), done = 0;
        for (int i = 0; i < n; i++) {
            if (w == W_AGGRO) done += A->npc_set_target(A, c[i], p) != 0;
            else { void *hc = objp(c[i], "m_HealthComponent"); float dmg = 999999.f; unsigned char dd = 0; void *a[2] = { &dmg, &dd }; done += call0(hc, "BPF_ApplyDamage", a, NULL);
                AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; A->char_info(A, c[i], &ci); A->log(A, "cheats: world kill %s -> health %.1f", ci.name, ci.health); }
        }
        set_status(w == W_AGGRO ? "%d of %d NPC(s) within 30 m: attack you" : "%d of %d NPC(s) within 30 m: killed", done, n); break; }
    case W_CLEAR: { int n = g_nspawned, r = 0; while (g_nspawned) { void *c = g_spawned[--g_nspawned]; r += A->npc_remove(A, c); } set_status("Removed %d of %d spawned NPC(s)", r, n); break; }
    case W_SPEED: { void *gs = A->find_cdo(A, "GameplayStatics"); float v = g_w_dil, back = 0.f; if (v < 0.1f) v = 0.1f; if (v > 3.f) v = 3.f;
        void *a[2] = { &p, &v }, *ga[1] = { &p }; int ok = call0(gs, "SetGlobalTimeDilation", a, NULL); call0(gs, "GetGlobalTimeDilation", ga, &back);
        set_status("Game speed %.2f (%s, now %.2f)", v, ok ? "set" : "FAILED", back); break; }
    case W_DEBUG: set_status("Game debug menu: %s", call0(pc, "BPE_ToggleDebugMenu", NULL, NULL) ? "toggled" : "not available"); break;
    case W_EMOTES: {
        int before = emotes_unlocked(p), calls = 0;
        for (int i = 1; i <= 53; i++) { uint64_t nm; unsigned char lock = 0; emote_name(i, &nm); void *a[2] = { &lock, &nm }; calls += call0(pc, "BPF_SetEmoteLock", a, NULL); }
        int after = emotes_unlocked(p); set_status("%d of 53 emotes unlocked (were %d; equip them in the emote menu)", after, before);
        A->log(A, "cheats: world emotes: before %d, %d SetEmoteLock calls, after %d", before, calls, after); break; }
    }
}
void cheats_world(int w) { if (w >= 0 && w < W_N && !mp_refused(cheats_world_label(w))) A->run_on_game_thread(A, world_do, (void *)(intptr_t)w); }
const char *cheats_world_label(int w) { return w >= 0 && w < W_N ? W_LABEL[w] : ""; }
int cheats_world_host_only(int w) { return w >= 0 && w < W_N && (W_HOST >> w & 1); }
int cheats_npc_count(void) { EnterCriticalSection(&g_cs); int n = g_nl_n; LeaveCriticalSection(&g_cs); return n; }
const char *cheats_npc_label(int i) { static char b[48]; return cheats_npcl_get(i, NULL, 0, b, sizeof b) ? b : ""; }   /* render thread only (one buffer) */
int cheats_get_int(int which) { return which == 0 ? (int)g_w_player : which == 1 ? (int)g_w_npc : (int)g_w_npcmax; }
void cheats_set_int(int which, int v) { if (which == 0 && v >= 1 && v <= 8) g_w_player = v; else if (which == 1 && v >= 0 && v < cheats_npc_count()) g_w_npc = v; else if (which == 2 && v >= 1 && v <= 40) g_w_npcmax = v; }
float cheats_dilation(void) { return g_w_dil; }
void cheats_set_dilation(float v) { if (v >= 0.1f && v <= 3.f) g_w_dil = v; }
void cheats_world_view(WorldView *out) { EnterCriticalSection(&g_cs); *out = g_wv; LeaveCriticalSection(&g_cs); }
/* every 0.25 s from keep(): the window's snapshot + the hover pose on every OTHER flying character (the movement mode replicates) */
static void world_keep(void *p) {
    WorldView v; memset(&v, 0, sizeof v);
    if (p) { v.have_pawn = 1; v.host = is_host(p); A->test_get_location(A, p, v.pos); float r[3] = { 0 }; void *cm = objp(A->get_local_player_controller(A), "PlayerCameraManager"); if (cm && call0(cm, "GetCameraRotation", NULL, r)) v.yaw = r[1]; }
    void *ps[16], *pw[16]; int n = players(ps, pw, 16); v.nplayers = n < WV_PLAYERS ? n : WV_PLAYERS;
    for (int i = 0; i < v.nplayers; i++) { player_name(ps[i], v.player[i], sizeof v.player[i]); v.is_me[i] = p && pw[i] == p; }
    v.npcs_near = p ? npcs_within(p, CAP_R, NULL, 0) : 0; v.spawned = g_nspawned; v.have_spot = g_have_spot;
    EnterCriticalSection(&g_cs); g_wv = v; LeaveCriticalSection(&g_cs);
    for (int i = 0; i < g_nch; i++) {
        void *c = g_ch[i]; if (c == g_pawn_fly) continue;
        int fl = cur_mode(objp(c, "CharacterMovement")) == 5; void *ai = anim_inst(c); float *cw = ai ? A->prop_ptr(A, ai, "m_fCinematicWeight") : NULL;
        if (fl && g_clip_tried && !g_clip) continue;                               /* no hover pak: flying without the pose */
        if (fl) { if (!g_ch_mont[i]) { g_ch_mont[i] = hover_on(c); char nm[64] = ""; A->object_name(A, c, nm, sizeof nm); A->log(A, "cheats: hover on another flying character %s -> montage %p", nm, g_ch_mont[i]); } if (cw) *cw = 1.f; }
        else if (g_ch_mont[i]) { if (cw) *cw = 0.f; float bout = 0.3f; void *a[2] = { &bout, &n_slot }; if (ai) call0(ai, "StopSlotAnimation", a, NULL); g_ch_mont[i] = NULL; }
    }
}

/* test only: test_world = <delay s> runs every World row once, 2.5 s apart, after a MainMap world appears (log lines "cheats: TEST ...") */
static void tw_step(void *u) {
    (void)u; void *p = pawn_(); float l[3] = { 0 }; if (p) A->test_get_location(A, p, l); char st[160]; cheats_status(st, sizeof st, NULL);
    if (g_tw_step > 0) A->log(A, "cheats: TEST step %d result: %s | me at %.0f %.0f %.0f", g_tw_step - 1, st, l[0], l[1], l[2]);
    static const struct { int w, a, b; } S[] = { { W_SAVESPOT }, { W_DOOR }, { W_ALTAR }, { W_LOADSPOT }, { W_TPAIM }, { W_GOTO, 0, 1 }, { W_BRINGALL },
        { W_SPAWN, 1, 0 }, { W_SPAWN, 1, 11 }, { W_SPAWN, 1, 18 }, { W_AGGRO }, { W_KILL }, { W_CLEAR }, { W_SPEED, 2 }, { W_SPEED, 3 }, { W_DEBUG }, { W_DEBUG }, { W_EMOTES } };
    int n = (int)(sizeof S / sizeof S[0]);
    if (g_tw_step >= n) { WorldView v; cheats_world_view(&v); A->log(A, "cheats: TEST view: host %d, %d player(s), #1 '%s' me %d, pos %.0f %.0f %.0f yaw %.0f, npcs near %d", v.host, v.nplayers, v.player[0], v.is_me[0], v.pos[0], v.pos[1], v.pos[2], v.yaw, v.npcs_near); A->log(A, "cheats: TEST world DONE (%d steps)", n); return; }
    int w = S[g_tw_step].w;
    if (S[g_tw_step].a == 1) cheats_set_int(1, S[g_tw_step].b); if (S[g_tw_step].a == 2) cheats_set_dilation(0.5f); if (S[g_tw_step].a == 3) cheats_set_dilation(1.f);
    A->log(A, "cheats: TEST step %d: %s (npc %d, speed %.2f)", g_tw_step, W_LABEL[w], (int)g_w_npc, g_w_dil);
    g_tw_step++; world_do((void *)(intptr_t)w); A->set_timer(A, 2.5f, 0, tw_step, NULL);
}

/* API 2.24: every on_key is a bindable action "on_key_<VK hex>" on the F2 Keys rows; naming it gives the row a label and a context (fewer false conflict warnings).
 * Older loaders: nothing (the size check). The callbacks still get the VK registered with on_key, whatever the player binds. */
static void name_key(int vk, const char *label, const char *input, const char *ctx) {
    if (A->size < offsetof(AbsApi, bind_name) + sizeof(void *)) return;
    char id[24]; snprintf(id, sizeof id, "on_key_%02X", vk);
    A->log(A, "cheats: key action %s '%s' (default %s, context %s) -> %d", id, label, input, ctx, A->bind_action(A, id, label, input, ctx));
}
static void ui_ready(void *u) {
    (void)u; int rc = cui_start(A); vk_ui = vk_of(g_uikey);
    if (!rc && vk_ui) { A->on_key(A, vk_ui, on_ui_key, NULL); name_key(vk_ui, "Cheats window", g_uikey, "always"); }
    A->log(A, "cheats: window %s (toggle key %s, VK %d; hidden until opened)", rc ? "unavailable (overlay_register failed or the overlay is off)" : "registered", g_uikey, vk_ui);
}
static int cfg_int(const char *k, int def) { char v[32]; return A->config_get(A, k, v, sizeof v) && v[0] ? atoi(v) : def; }
int cheats_test_ui(char *query, int cap, int *select_first) { char v[96]; query[0] = 0; *select_first = cfg_int("test_select", 0);
    if (A->config_get(A, "test_query", v, sizeof v)) snprintf(query, (size_t)cap, "%s", v);
    return cfg_int("test_tab", 0); }
/* the values the Mods page settings form can change (mod.json "settings"); read at load and again when the page saved new values */
static void read_settings(void) {
    { char ov[16]; if (A->config_get(A, "off_with_players", ov, sizeof ov)) g_offwp = !(ov[0] == '0' || !_stricmp(ov, "false") || !_stricmp(ov, "off")); }
    char v[64]; float sp;
    if (A->config_get(A, "fly_speed", v, sizeof v) && (sp = (float)atof(v)) > 50.f) g_speed = sp;
    if (A->config_get(A, "ui_key", v, sizeof v) && v[0]) {                   /* F3..F12 only: F1 / F2 are the loader's menus, other keys reach the game */
        int vk = vk_of(v); if (vk >= VK_F3 && vk <= VK_F12) snprintf(g_uikey, sizeof g_uikey, "%s", v); else A->log(A, "cheats: ui_key '%s' refused (F3 .. F12) - %s", v, g_uikey); }
    g_defcount = cat_count_clamp(cfg_int("default_count", 1)); g_defequip = cfg_int("equip_after_add", 0) != 0;
}
__declspec(dllexport) void gleam_settings_changed(const AbsApi *api) { (void)api; if (!A) return; read_settings(); A->log(A, "cheats: settings changed: fly speed %.0f, default count %d, equip after add %d (ui key %s needs a restart)", g_speed, g_defcount, g_defequip, g_uikey); }
__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    A = api;
    if (api->version != ABS_API_VERSION || api->size < offsetof(AbsApi, poise_set)) { if (api->version == ABS_API_VERSION) api->log(api, "cheats: loader too old (needs damage_mod)"); return 1; }
    char v[256];
    InitializeCriticalSection(&g_cs); read_settings();
    if (A->config_get(A, "fly_up", v, sizeof v) && v[0]) snprintf(g_up, sizeof g_up, "%s", v);
    if (A->config_get(A, "fly_down", v, sizeof v) && v[0]) snprintf(g_down, sizeof g_down, "%s", v);
    if (A->config_get(A, "hover_clip", v, sizeof v) && v[0]) snprintf(g_clip_path, sizeof g_clip_path, "%s", v);
    if (A->config_get(A, "test_fly", v, sizeof v) && v[0]) { g_tf_start = (float)atof(v); char *c = strchr(v, ','); g_tf_len = c ? (float)atof(c + 1) : 10.f; }
    if (A->config_get(A, "fly_key", v, sizeof v) && v[0]) snprintf(g_flykey, sizeof g_flykey, "%s", v);
    vk_fly = vk_of(g_flykey); if (!vk_fly) { A->log(A, "cheats: fly_key '%s' unknown - B", g_flykey); vk_fly = 'B'; snprintf(g_flykey, sizeof g_flykey, "B"); }
    for (int i = 0; i < C_N; i++) { g_on[i] = cfg_int(C_KEY[i], 0); if (A->kv_get(A, C_KEY[i], v, sizeof v)) g_on[i] = atoi(v) != 0; }
    if (has_rows()) for (int i = 0; i < C_N; i++) A->log(A, "cheats: F2 row '%s' -> %d", C_LABEL[i], A->ui_toggle(A, C_KEY[i], C_LABEL[i], g_on[i], on_row, (void *)(intptr_t)i));   /* default = the stored state, so the first switch to rows keeps it */
    else A->log(A, "cheats: loader older than API 2.27 - no F2 rows, the window only");
    key_make(&k_up, g_up); key_make(&k_down, g_down); vk_up = vk_of(g_up); vk_down = vk_of(g_down);
    A->fname_make(A, "LeftShift", &n_lshift); A->fname_make(A, "RightShift", &n_rshift); A->fname_make(A, "I", &n_help);
    if (!vk_up || !vk_down) A->log(A, "cheats: fly_up '%s' / fly_down '%s': unknown key name (use a letter, SpaceBar, LeftControl, ...)", g_up, g_down);
    static const uint8_t kb[] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };   /* AFightingPlayerController::InputKey 0x39A6C0 */
    o_key = (InputKey_t)A->hook_rva(A, "Cheats InputKey", 0x39A6C0u, kb, sizeof kb, (void *)hk_key);
    if (!o_key) A->log(A, "cheats: InputKey hook failed - Space / Ctrl / Shift also dodge / block while flying");
    A->fname_make(A, "Cinematic", &n_slot);
    { char lab[64]; snprintf(lab, sizeof lab, "Open the Cheats window (%s)", g_uikey); A->log(A, "cheats: menu '%s' -> %d (the only menu row; everything else is in the window)", lab, A->add_menu_item(A, lab, open_ui, NULL)); }
    A->on_pawn_spawned(A, on_spawned, NULL); A->on_pawn_destroyed(A, on_destroyed, NULL);
    if (A->config_get(A, "test_world", v, sizeof v) && v[0]) g_tw_start = (float)atof(v);
    A->on_key(A, vk_fly, on_fly_key, NULL); name_key(vk_fly, "Fly on / off", g_flykey, "foot");
    A->on_engine_ready(A, ui_ready, NULL);                                         /* the overlay hooks the swap chain at the first overlay_register: not from AbsPlugin_Load */
    if (A->config_get(A, "test_add", v, sizeof v) && v[0]) { snprintf(g_test_add, sizeof g_test_add, "%s", v); }
    if (A->config_get(A, "test_add2", v, sizeof v) && v[0]) { snprintf(g_test_add2, sizeof g_test_add2, "%s", v); }
    EnterCriticalSection(&g_cs); npcl_builtin(); LeaveCriticalSection(&g_cs);          /* until the first world asks the loader for its list */
    A->set_timer(A, 0.25f, 1, keep, NULL); A->on_tick(A, tick, NULL); A->on_world_loaded(A, on_world, NULL);
    A->log(A, "cheats: loaded (shards %d god %d stamina %d onehit %d fly %d (key %s); fly speed %.0f, up %s (VK %d), down %s (VK %d), clip %s)", g_on[0], g_on[1], g_on[2], g_on[3], g_on[C_FLY], g_flykey, g_speed, g_up, vk_up, g_down, vk_down, g_clip_path);
    return 0;
}

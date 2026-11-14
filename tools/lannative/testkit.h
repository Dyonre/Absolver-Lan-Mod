// Pure logic of the test_* functions (AbsApi 2.6: test_heal, test_attack, test_guard, ...). No engine calls.
// The engine-facing half is testkit.inc. Background: docs\PLUGIN-API.md "Functions for automated checks".
#ifndef LANNATIVE_TESTKIT_H
#define LANNATIVE_TESTKIT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

// ----- watchdog decision: what must be put right on a non-player character so a test never leaves it dead or breakable
enum { TK_FIX_NONE = 0, TK_FIX_INVINCIBLE = 1, TK_FIX_HEALTH = 2, TK_FIX_DOWN = 4 };
// health/max: current and maximum health; invincible: 1 if the character is currently invincible (-1 = unknown, then it is set again to be sure);
// is_down: HealthComponent::m_bIsDown; min_frac: 0..1 (below it the health is topped up to the full value).
static int tk_watch_decide(float health, float max_health, int invincible, int is_down, float min_frac) {
    int fix = TK_FIX_NONE;
    if (invincible != 1) fix |= TK_FIX_INVINCIBLE;
    if (is_down) fix |= TK_FIX_DOWN | TK_FIX_HEALTH;
    if (max_health > 0.0f) {
        if (min_frac < 0.0f) min_frac = 0.0f;
        if (min_frac > 1.0f) min_frac = 1.0f;
        if (health < max_health * min_frac || health <= 0.0f) fix |= TK_FIX_HEALTH;
    }
    return fix;
}

// ----- attack row names: the attacks table keys are plain tokens ("135", "BT2_01", "MyJab"); anything else is refused before it reaches the engine
static int tk_attack_name_ok(const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s); if (n > 31) return 0;
    for (; *s; s++) { char c = *s; if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0; }
    return 1;
}

// ----- quadrant (EQuadrantTypes: 0 FrontLeft, 1 FrontRight, 2 BackRight, 3 BackLeft, 4 Count, 5 None): the attack's own start quadrant wins, else the character's current one, else FrontLeft
static int tk_quadrant_pick(int row_start, int current) {
    if (row_start >= 0 && row_start <= 3) return row_start;
    if (current >= 0 && current <= 3) return current;
    return 0;
}

// ----- DelayedActionAttack (the object UAttackComponent::PrepareToLaunchAttack takes), layout from the constructor at base+0x2E3090.
//   +0x10 blocking layer type, +0x70 InputAction, +0x71 quadrant, +0x72 consume stamina, +0x73 attack rush, +0x74 perfect link, +0x75 attack cancel,
//   +0x78 attack FName (8 bytes), +0x80 force debug play, +0x81 drunken attack (ECardinalPoints, 5 = None)
#define TK_DAA_SIZE 0x90
#define TK_DAA_FORCE 0x80
// 1 if a freshly constructed object has the fields where this file says they are (checked at run time before the first launch)
static int tk_daa_check(const uint8_t *b, int input, int quadrant, uint64_t name) {
    uint64_t nm; memcpy(&nm, b + 0x78, 8);
    return b[0x70] == (uint8_t)input && b[0x71] == (uint8_t)quadrant && nm == name && b[TK_DAA_FORCE] == 0 && b[0x81] == 5;
}

// Plausibility check for a pointer before it is used as an object.
static int tk_ptr_ok(const void *p) { return p && (uintptr_t)p > 0x10000 && (uintptr_t)p < 0x00007FFFFFFFFFFFull; }

// ----- an attack row's m_Anim soft path, accepted for StaticLoadObject only as "/Root/.../Pkg.Obj" (not None/empty, no spaces or quotes, an object part after the last '/')
static int tk_anim_path_ok(const char *s) {
    if (!s || s[0] != '/' || strlen(s) >= 300) return 0;
    const char *slash = strrchr(s, '/'), *dot = strrchr(s, '.');
    if (!dot || dot < slash || dot == slash + 1 || !dot[1]) return 0;
    for (; *s; s++) if (*s == ' ' || *s == '"' || *s == '\'' || *s == '\t') return 0;
    return 1;
}
// ----- row name list without duplicates (moves + deck seed of an item): returns the new count; rows that do not fit (cap, length >= 49) are left out
static int tk_rowlist_add(char (*list)[49], int n, int cap, const char *row) {      // 49 = DL_ID_BUF (decklayout.h: row names up to 48 characters)
    if (!row || !*row || strlen(row) >= 49 || n >= cap) return n;
    for (int i = 0; i < n; i++) if (!strcmp(list[i], row)) return n;
    memcpy(list[n], row, strlen(row) + 1); return n + 1;
}

// ----- sparring loop (spawn a bot, remove it when it is down, spawn the next): what to do this tick. on: sparring wanted; have_bot: a bot character exists; alive: it is up and above 0 health.
// TK_SPAR_REMOVE = take the dead / downed bot away (the caller then waits `delay_ms` before the next spawn); TK_SPAR_SPAWN = spawn a new one (not before `next_ms`, set by the caller after a removal).
enum { TK_SPAR_NONE = 0, TK_SPAR_REMOVE = 1, TK_SPAR_SPAWN = 2 };
static int tk_spar_decide(int on, int have_bot, int alive, unsigned long long now_ms, unsigned long long next_ms) {
    if (!on) return TK_SPAR_NONE;
    if (have_bot) return alive ? TK_SPAR_NONE : TK_SPAR_REMOVE;
    return now_ms >= next_ms ? TK_SPAR_SPAWN : TK_SPAR_NONE;
}

// ----- fall guard (bot trials, automated tests): puts the player back on solid ground after a fall out of the map.
// Fed the player's location every ~0.25 s. The safe spot is the last sample where the player stood vertically still (|dz| < 100 to the previous sample) IN THIS WORLD.
// A new world (travel), or a jump of more than `jump` uu between two samples (a game teleport: Practice drops the player ~19600 uu, a respawn, a trial spawn) is no fall:
// the safe spot is forgotten and learnt again where the player now stands (a spot from another world would put the player at the wrong coordinates).
// TK_FALL_RETURN: z is more than `drop` below the safe spot -> the caller puts the player back there. More than 3 returns within 10 s = the spot does not hold:
// TK_FALL_GIVEUP (safe spot forgotten, no more teleports until a new one is learnt).
typedef struct { float safe[3], prev[3]; int safe_set, prev_set, n_ret; const void *world; unsigned long long t_ret0; } TkFall;
enum { TK_FALL_NONE = 0, TK_FALL_LEARN = 1, TK_FALL_RESET = 2, TK_FALL_RETURN = 3, TK_FALL_GIVEUP = 4 };
static int tk_fall_step(TkFall *s, const void *world, const float l[3], float drop, float jump, unsigned long long now_ms) {
    if (world != s->world) { memset(s, 0, sizeof *s); s->world = world; memcpy(s->prev, l, sizeof s->prev); s->prev_set = 1; return TK_FALL_RESET; }
    if (s->prev_set) {
        float dx = l[0] - s->prev[0], dy = l[1] - s->prev[1], dz = l[2] - s->prev[2];
        if (dx * dx + dy * dy + dz * dz > jump * jump) { s->safe_set = 0; memcpy(s->prev, l, sizeof s->prev); return TK_FALL_RESET; }
    }
    if (s->safe_set && l[2] < s->safe[2] - drop) {
        s->prev_set = 0;                                                                           // the return itself is a jump: not compared with this sample
        if (now_ms - s->t_ret0 >= 10000ull) { s->t_ret0 = now_ms; s->n_ret = 0; }
        if (++s->n_ret > 3) { s->safe_set = 0; return TK_FALL_GIVEUP; }
        return TK_FALL_RETURN;
    }
    int learn = s->prev_set && fabsf(l[2] - s->prev[2]) < 100.f;
    if (learn) { memcpy(s->safe, l, sizeof s->safe); s->safe_set = 1; }
    memcpy(s->prev, l, sizeof s->prev); s->prev_set = 1;
    return learn ? TK_FALL_LEARN : TK_FALL_NONE;
}

#endif

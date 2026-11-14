// hitpre.h - pure maths of on_hit_pre / damage_mod / poise (loader glue: hitpre.inc).
// No game access, no allocation: everything here works on plain numbers and fixed tables so it can be tested offline.
//
// Where each number goes in the game (UHitComponent::GenerateImpact @1403ffea0).
//   damage     *dmg = raw split into normal + special part, x GetDamageMultiplier(attacker) x GetReceivedDamageMultiplier(victim) -> ApplyDamage.
//              hitpre multiplies the GetReceivedDamageMultiplier result, so the scale lands on the FINAL damage that on_hit reports.
//   special    the special ("cut") share of the raw damage = BPF_GetAttackSpecialDamageRatio (weapon) or GetBareHandSpecialRatioFromEffects (bare hands);
//              normal part = raw x (1 - ratio), special part = raw x ratio, each against its own defence.
//   stun       frames from the attack row (FAttackDataRow bytes below) x 1/30 s -> the Hitted order; the row getters cannot be hooked (a branch in
//              their first 14 bytes), so hitpre scales the row bytes for the duration of the one GenerateImpact call and restores them.
//   knockback  local coefficient = GetKnockbackCoefFromEffects(victim) -> ComputeKnockBackPosition (hit and guard).
//   poise      resilience check: victim DefenseComponent m_iResilience (+0x260) + GetResilienceBonusFromEffects(victim) [+ m_iResilienceAttackBonus
//              (+0x264) while the victim attacks] > the attack's EAttackPowers (Light 0, Medium 1, Strong 2) -> the hit is absorbed by resilience
//              (damage still applies, NotifyResilienceImpact instead of the Hitted order = no interruption).
#ifndef HITPRE_H
#define HITPRE_H
#include <stdint.h>
#include <string.h>

// ---- damage_mod keys: "_taken" keys apply when the character is hit, "_dealt" keys and special_ratio when it hits
enum { HPK_DAMAGE_TAKEN, HPK_DAMAGE_DEALT, HPK_STUN_TAKEN, HPK_STUN_DEALT, HPK_KNOCKBACK_TAKEN, HPK_KNOCKBACK_DEALT, HPK_SPECIAL_RATIO, HPK_N };
static const char *const hp_key_names[HPK_N] = { "damage_taken", "damage_dealt", "stun_taken", "stun_dealt", "knockback_taken", "knockback_dealt", "special_ratio" };
static int hp_key(const char *k) { if (k) for (int i = 0; i < HPK_N; i++) if (!strcmp(k, hp_key_names[i])) return i; return -1; }
#define HP_SCALE_MAX 100.0f
static int hp_scale_ok(float v) { return v == v && v >= 0.0f && v <= HP_SCALE_MAX; }      // NaN, negative and absurd values are refused

typedef struct { float damage, special, stun, knockback; } HpScales;
// the scales of one hit from the victim's and the attacker's multipliers (NULL = that character has none: all 1)
static HpScales hp_combine(const float *victim_m, const float *attacker_m) {
    static const float one[HPK_N] = { 1, 1, 1, 1, 1, 1, 1 };
    const float *v = victim_m ? victim_m : one, *a = attacker_m ? attacker_m : one; HpScales s;
    s.damage = v[HPK_DAMAGE_TAKEN] * a[HPK_DAMAGE_DEALT]; s.stun = v[HPK_STUN_TAKEN] * a[HPK_STUN_DEALT];
    s.knockback = v[HPK_KNOCKBACK_TAKEN] * a[HPK_KNOCKBACK_DEALT]; s.special = a[HPK_SPECIAL_RATIO];
    return s;
}
// special share actually used: forced (>= 0) wins over the scaled game value; always 0..1 (a share above 1 would make the normal part negative)
static float hp_special(float game_ratio, float scale, float forced) {
    float r = forced >= 0.0f ? forced : game_ratio * scale;
    return r != r || r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
}
static uint8_t hp_scale_frames(uint8_t f, float s) { float v = (float)f * s + 0.5f; return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v; }   // rounded, clamped to the byte

// ---- stun frames of an FAttackDataRow (the bytes FAttackDataRow::GetNbFrameStuned* read): base value + hits 2..4, for hit / on guard / perfect link / perfect link on guard
#define HP_STUN_N 16
static const uint16_t hp_stun_off[HP_STUN_N] = { 0x10, 0x189, 0x18A, 0x18B,   0x11, 0x18C, 0x18D, 0x18E,   0xFB, 0xFC, 0xFD, 0xFE,   0xFF, 0x100, 0x101, 0x102 };
static void hp_stun_scale(uint8_t *row, float s, uint8_t saved[HP_STUN_N]) { for (int i = 0; i < HP_STUN_N; i++) { saved[i] = row[hp_stun_off[i]]; row[hp_stun_off[i]] = hp_scale_frames(saved[i], s); } }
static void hp_stun_restore(uint8_t *row, const uint8_t saved[HP_STUN_N]) { for (int i = 0; i < HP_STUN_N; i++) row[hp_stun_off[i]] = saved[i]; }

// ---- poise: a gauge on top of the game's resilience check.
//   gauge > 0 : resilience is forced to heavy_power -> attacks below that power (lights) are absorbed (no interruption), their damage drains the gauge.
//   gauge = 0 : resilience is forced to 0 -> the next hit interrupts, and the gauge refills.
//   a hit at or above heavy_power interrupts at once and refills the gauge.
// heavy_power: EAttackPowers 0 Light, 1 Medium, 2 Strong (default 2: Strong = heavy).
#define HP_HEAVY_DEFAULT 2
typedef struct { float max, cur; int heavy_power; } HpPoise;
enum { HPP_NONE, HPP_ABSORBED, HPP_BROKEN, HPP_INTERRUPTED };
static const char *const hp_poise_ev[4] = { "none", "absorbed", "absorbed, gauge broken (next hit interrupts)", "interrupted, gauge refilled" };
static void hp_poise_set(HpPoise *p, float max, int heavy_power) { p->max = max > 0.0f ? max : 0.0f; p->cur = p->max; p->heavy_power = heavy_power < 0 || heavy_power > 3 ? HP_HEAVY_DEFAULT : heavy_power; }
static int hp_poise_target(const HpPoise *p) { return p->max <= 0.0f ? -1 : p->cur > 0.0f ? p->heavy_power : 0; }    // resilience to force; -1 = no poise, leave the game's
static signed char hp_res_bonus(int target, int base) { int b = target - base; return (signed char)(b < -128 ? -128 : b > 127 ? 127 : b); }   // bonus so that base + bonus == target
// after the game resolved the hit: was_hit = a real hit landed (not guarded / parried / dodged / absorbed by a power), res_absorbed = the game took the resilience branch
static int hp_poise_after(HpPoise *p, int was_hit, int res_absorbed, float damage) {
    if (p->max <= 0.0f || !was_hit) return HPP_NONE;
    if (res_absorbed) { p->cur -= damage > 0.0f ? damage : 0.0f; if (p->cur <= 0.0f) { p->cur = 0.0f; return HPP_BROKEN; } return HPP_ABSORBED; }
    p->cur = p->max; return HPP_INTERRUPTED;
}

// ---- per-character table: 64 slots, linear scan per impact (a hash if fights ever have dozens of modded characters)
#define HP_CHARS 64
typedef struct { void *ch; float m[HPK_N]; HpPoise poise; int checked; } HpChar;   // checked = the poise content entries were matched against this character once
typedef struct { HpChar c[HP_CHARS]; int full_logged; } HpTable;
static void hp_char_reset(HpChar *c, void *ch) { memset(c, 0, sizeof *c); c->ch = ch; for (int i = 0; i < HPK_N; i++) c->m[i] = 1.0f; }
static HpChar *hp_find(HpTable *t, const void *ch) { if (ch) for (int i = 0; i < HP_CHARS; i++) if (t->c[i].ch == ch) return &t->c[i]; return NULL; }
static HpChar *hp_get(HpTable *t, void *ch) {                                     // find or make; NULL = table full
    HpChar *c = hp_find(t, ch); if (c || !ch) return c;
    for (int i = 0; i < HP_CHARS; i++) if (!t->c[i].ch) { hp_char_reset(&t->c[i], ch); return &t->c[i]; }
    return NULL;
}
static int hp_char_default(const HpChar *c) { for (int i = 0; i < HPK_N; i++) if (c->m[i] != 1.0f) return 0; return c->poise.max <= 0.0f && !c->checked; }
static void hp_drop(HpTable *t, const void *ch) { HpChar *c = hp_find(t, ch); if (c) memset(c, 0, sizeof *c); }
static int hp_count(const HpTable *t) { int n = 0; for (int i = 0; i < HP_CHARS; i++) n += t->c[i].ch != NULL; return n; }
// 1 = stored (value 1 on a character without anything else frees its slot), 0 = bad key / value / table full
static int hp_set_mod(HpTable *t, void *ch, int key, float v) {
    if (!ch || key < 0 || key >= HPK_N || !hp_scale_ok(v)) return 0;
    HpChar *c = hp_get(t, ch); if (!c) return 0;
    c->m[key] = v; if (hp_char_default(c)) memset(c, 0, sizeof *c); return 1;
}

// ---- "poise" content entries: archetype = an npcs / bots entry id or a stock archetype name ("Trickster") or full path, case-insensitive
static int hp_ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; } return *a == *b; }
static int hp_arch_match(const char *want, const char *arch_name, const char *arch_path) { return want && want[0] && ((arch_name && hp_ieq(want, arch_name)) || (arch_path && hp_ieq(want, arch_path))); }
#endif

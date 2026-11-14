/* bots.h - pure helpers of the "bots" content kind (bots.inc): deck mapping (bot_decks JSON -> AttacksComboDB text), AI profiles (vanilla / classic_hard)
 * as property tables, style and profile names. No game access. */
#ifndef BOTS_H
#define BOTS_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "npcs.h"                                                    /* np_alt_part: the alternates' engine text, shared with the npcs kind */

enum { BT_VANILLA = 0, BT_CLASSIC_HARD = 2 };
/* the AI profile as property writes. Element writes use "name[i]". Values are engine text. */
typedef struct { const char *k, *v; } BtKV;
static int bt_profile(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "vanilla")) return BT_VANILLA;
    if (!strcmp(s, "classic_hard")) return BT_CLASSIC_HARD;
    return -1;
}
#define BT_PROFILE_NAMES "vanilla or classic_hard"
static const char *bt_profile_name(int p) { return p == BT_VANILLA ? "vanilla" : p == BT_CLASSIC_HARD ? "classic_hard" : "?"; }
/* style: a name (bot_decks "style": Faejin, Forsaken, Windfall, Kahlt / Khalt, Stagger; any case) or a number 0..63; -1 = unknown */
static int bt_style(const char *s) {
    static const char *const n[][2] = { { "forsaken", "0" }, { "windfall", "1" }, { "kahlt", "3" }, { "khalt", "3" }, { "stagger", "4" }, { "faejin", "5" } };
    if (!s || !*s) return -1;
    if (s[0] >= '0' && s[0] <= '9') { int v = atoi(s); return v <= 63 ? v : -1; }
    for (int i = 0; i < 6; i++) if (!_stricmp(s, n[i][0])) return atoi(n[i][1]);
    return -1;
}
/* the special ability DB a style needs on an NPC archetype (Stagger -> DrunkenDB, Faejin -> JeetDB, as the player styles use); NULL = keep the template's */
static const char *bt_style_ability(int style) { return style == 4 ? "/Game/DB/Drunken/DrunkenDB.DrunkenDB" : style == 5 ? "/Game/DB/JKD/JeetDB.JeetDB" : NULL; }

/* in-game deck rows / alternate slots, in the order the deck editor and bot_decks use: 0 FR, 1 FL, 2 BR, 3 BL; -1 = not a row key */
static int bt_quad(const char *k) {
    static const char *const n[4] = { "FR", "FL", "BR", "BL" };
    if (!k) return -1;
    for (int i = 0; i < 4; i++) if (!strcmp(k, n[i])) return i;
    return -1;
}
/* the AttacksComboDB field that holds in-game row q. mirror = 1 (default): the asset fields are left/right mirrored like the player's save decks
 * (FR is stored in ...FrontLeftXls); mirror = 0: same side. */
static const char *bt_row_field(int q, int mirror) {
    static const char *const m[4] = { "m_ComboQuadrantsFrontLeftXls", "m_ComboQuadrantsFrontRightXls", "m_ComboQuadrantsBackLeftXls", "m_ComboQuadrantsBackRightXls" };
    static const char *const s[4] = { "m_ComboQuadrantsFrontRightXls", "m_ComboQuadrantsFrontLeftXls", "m_ComboQuadrantsBackRightXls", "m_ComboQuadrantsBackLeftXls" };
    return q >= 0 && q < 4 ? (mirror ? m[q] : s[q]) : NULL;
}
/* a deck: rows[q][i] attack row names (in-game order FR FL BR BL), alts[q] ("" = none) */
#define BT_ROW_MAX 8
typedef struct { char rows[4][BT_ROW_MAX][16]; int n[4]; char alts[4][16]; int moves; } BtDeck;
/* the AttacksComboDB m_Struct text in the engine's own export order (FL, FR, BR, BL, then the 4 alternates as [i]=...), so an exact read-back compare works.
 * Unused rows become (), unused alternates None. Returns the length, -1 if it does not fit. */
static int bt_struct_text(const BtDeck *d, int mirror, char *out, int cap) {
    static const char *const order[4] = { "m_ComboQuadrantsFrontLeftXls", "m_ComboQuadrantsFrontRightXls", "m_ComboQuadrantsBackRightXls", "m_ComboQuadrantsBackLeftXls" };
    int len = snprintf(out, cap, "(");
    for (int f = 0; f < 4; f++) {
        int q = -1; for (int k = 0; k < 4; k++) if (!strcmp(bt_row_field(k, mirror), order[f])) q = k;
        len += snprintf(out + len, len < cap ? cap - len : 0, "%s%s=(", f ? "," : "", order[f]);
        for (int i = 0; i < d->n[q]; i++) len += snprintf(out + len, len < cap ? cap - len : 0, "%s\"%s\"", i ? "," : "", d->rows[q][i]);
        len += snprintf(out + len, len < cap ? cap - len : 0, ")");
    }
    for (int a = 0; a < 4 && len < cap; a++) len = np_alt_part(out, len, cap, 1, a, d->alts[a]);
    len += snprintf(out + len, len < cap ? cap - len : 0, ")");
    return len < cap ? len : -1;
}
/* "name[3]" -> base "name", returns 3; "name" -> -1; malformed ("a[", "a[x]", "a[99]") -> -2 */
static int bt_split_index(const char *name, char *base, int cap) {
    const char *b = strchr(name, '['); int n = b ? (int)(b - name) : (int)strlen(name);
    snprintf(base, cap, "%.*s", n, name);
    if (!b) return -1;
    char *end; long i = strtol(b + 1, &end, 10);
    if (end == b + 1 || *end != ']' || end[1] || i < 0 || i > 63) return -2;
    return (int)i;
}

/* on the bot's ArchetypeAsset copy: the stock AI's most common values ("vanilla") */
static const BtKV *bt_arch_props(int profile, int *n) {
    static const BtKV van[] = {
        { "m_FeintData", "None" },
        { "m_fQuadrantChangeTimer", "2.0" }, { "m_uiQuadrantChangeProba", "0" }, { "m_uiAttackAlternativeProba", "0" }, { "m_uiPerfectLinkProbas", "30" },
        { "m_uiMinMaxAttacksCount", "3" }, { "m_uiMaxMaxAttacksCount", "5" }, { "m_fAttackCooldown", "2.0" }, { "m_fAttackCooldownDeviation", "1.0" },
        { "m_fDefenseOpenState", "2.0" }, { "m_fDefenseOpenStateDeviation", "0.2" },
        { "m_DefenseUseProbabilities[0]", "(m_uiSuccessUseProba=50,m_uiFailUseProba=50)" }, { "m_DefenseUseProbabilities[1]", "(m_uiSuccessUseProba=30,m_uiFailUseProba=30)" },
        { "m_DefenseUseProbabilities[2]", "(m_uiSuccessUseProba=50,m_uiFailUseProba=50)" },
        { "m_fObserveStepBackDodge", "120.0" }, { "m_fDefenseStepBackDodge", "150.0" }, { "m_fStepBackDodgeCooldown", "3.0" }, { "m_fDamageMultiplier", "0.9" },
        { "m_bCanAttack", "True" }, { "m_bCanDefend", "True" } };
    if (profile == BT_VANILLA) { *n = (int)(sizeof van / sizeof *van); return van; }
    *n = 0; return NULL;                                             /* classic_hard: the template as it is */
}
/* on the copies of the archetype's easy / hard AIDifficultyDataAsset; only an add-on profile changes them (n = 0: the template's presets stay) */
static const BtKV *bt_diff_props(int profile, int *n) {
    (void)profile; *n = 0; return NULL;
}
/* ai_level_fixed: written on the bot's own copy of its AIDifficultyVariationDataAsset (m_difficultyVariationData). Every writer of the live level
 * (UAIFightingComponent m_fCurrentDifficultyLevel +0x2DC: OnGuardSuccess, OnDodgeSuccess, OnSpecialAbilitySuccess, OnAvoided, DodgedCallBack, DamagesDealt, HitDetection,
 * ChangeCurrentDifficultyLevel, GoBackToInitialDifficultyLevelOverTime) adds a value of THIS asset (rate, damage bonus / malus, 8 cause deltas) - all zero = the level
 * stays at m_uiInitialDifficultyLevel. A copy (not None) keeps the feint-gauge updates that DamagesDealt skips when the asset is missing. */
static const BtKV *bt_fixed_props(int *n) {
    static const BtKV f[] = { { "m_fRateOverTime", "0.0" }, { "m_fDamageReceivedToDifficultyBonus", "0.0" }, { "m_fDamageDealtToDifficultyMalus", "0.0" },
                              { "m_fDifficultyVariations[0]", "0.0" }, { "m_fDifficultyVariations[1]", "0.0" }, { "m_fDifficultyVariations[2]", "0.0" }, { "m_fDifficultyVariations[3]", "0.0" },
                              { "m_fDifficultyVariations[4]", "0.0" }, { "m_fDifficultyVariations[5]", "0.0" }, { "m_fDifficultyVariations[6]", "0.0" }, { "m_fDifficultyVariations[7]", "0.0" } };
    *n = (int)(sizeof f / sizeof *f); return f;
}
/* bots entry "resilience": the archetype's m_uiResilience (a byte; 0 = interruptible like a player). "0".."10"; -1 = not a number in range */
static int bt_resilience(const char *s) {
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return -1;
    int v = atoi(s); return strlen(s) <= 2 && v <= 10 ? v : -1;
}
/* ---- stats like a player (Steam guide 3615970885 "optimal stats", level 60 = 84 points incl. the style's base points; values read from its two tables).
 * Order = EDistributionStatType: 0 Vitality, 1 Strength, 2 Agility (the menu's Dexterity), 3 Endurance, 4 Faith (the menu's Will). The guide's "non-Will" build keeps Will at the
 * style's base (3); the "Will" build spends into Will because Will feeds powers and weapons (the guide ties the choice to how much weapons / powers are used in a duel).
 * health / stamina = what the guide's stat screen shows for that build (an AI's health / guard come from its archetype, not from its stat points: bots.inc writes both). */
typedef struct { int style, will; int pts[5]; float health, stamina; } BtStatBuild;
static const BtStatBuild *bt_stat_build(int style, int will) {
    static const BtStatBuild t[] = {
        /* non-Will:      Vit Str Dex End Will */
        { 0, 0, { 24, 20, 22, 15, 3 }, 1239.f, 226.f },   /* Forsaken */
        { 3, 0, { 23, 17, 24, 17, 3 }, 1258.f, 232.f },   /* Kahlt */
        { 1, 0, { 24, 22, 17, 18, 3 }, 1198.f, 226.f },   /* Windfall */
        { 4, 0, { 32, 20, 16, 13, 3 }, 1224.f, 226.f },   /* Stagger */
        { 5, 0, { 24, 18, 18, 21, 3 }, 1198.f, 249.f },   /* Faejin */
        /* Will: */
        { 0, 1, { 17, 18, 17, 22, 10 }, 1151.f, 245.f },  /* Forsaken */
        { 1, 1, { 20, 15, 12, 24, 13 }, 1151.f, 241.f },  /* Windfall */
        { 3, 1, { 18, 17, 11, 22, 16 }, 1198.f, 245.f },  /* Kahlt */
        { 4, 1, { 26, 15, 13, 18, 12 }, 1171.f, 241.f },  /* Stagger */
        { 5, 1, { 24, 18, 18, 18, 6 }, 1198.f, 241.f } }; /* Faejin */
    for (int i = 0; i < (int)(sizeof t / sizeof *t); i++) if (t[i].style == style && t[i].will == (will ? 1 : 0)) return &t[i];
    return NULL;
}
/* the deck rule: a bot with a sword deck uses the Will build (it fights with the sword: both decks combined, or sword only); bare hands only -> the non-Will build */
static int bt_will_build(int has_bare, int has_sword) { (void)has_bare; return has_sword ? 1 : 0; }
/* "stats": "guide" (auto by the decks), "guide_will", "guide_no_will", "none"; -2 = bad text. Returns 0 no-will, 1 will, 2 auto, -1 none */
static int bt_stats_mode(const char *s) {
    if (!s || !*s || !strcmp(s, "none")) return -1;
    if (!strcmp(s, "guide")) return 2;
    if (!strcmp(s, "guide_will")) return 1;
    if (!strcmp(s, "guide_no_will")) return 0;
    return -2;
}
/* "outfit" keys -> EquipmentSelectionData.m_CharacterEquipments index (the save's m_Outfits order) */
static int bt_outfit_slot(const char *k) {
    static const char *const n[10] = { "mask", "elbow", "shoulder", "undertop", "overtop", "gloves", "belt", "trousers", "shoes", "hair" };
    if (!k) return -1;
    for (int i = 0; i < 10; i++) if (!strcmp(k, n[i])) return i;
    return -1;
}
/* engine text compare for a read-back: exact, or both numbers within 0.001 ("0.5" vs "0.500000"), or bool case ("True" / "true") */
static int bt_text_same(const char *want, const char *got) {
    if (!want || !got) return 0;
    if (!strcmp(want, got) || !_stricmp(want, got)) return 1;
    { size_t w = strlen(want), g = strlen(got); if (g > w + 2 && got[g - 1] == '\'' && got[g - w - 2] == '\'' && !strncmp(got + g - w - 1, want, w)) return 1; }   /* object reference: Class'path' */
    char *e1, *e2; double a = strtod(want, &e1), b = strtod(got, &e2);
    return e1 != want && !*e1 && e2 != got && !*e2 && a - b < 0.001 && b - a < 0.001;
}
#endif

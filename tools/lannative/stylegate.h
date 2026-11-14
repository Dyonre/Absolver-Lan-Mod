// stylegate.h - pure decision for a gated "styles" entry (kind "styles", field "gleam_level"). Offline test: test\stylegate_test.c.
// Faejin is not flagged in the save: the game computes its unlock live from the Gleam level (Progression.m_Rewards[42] = GleamLvl 6, UStatsComponent::BPF_IsProgressionRewardUnlocked
// compares m_CurrentGobalStats.m_CharacterStats.m_iResourcesLvl). A gated extra style mirrors that: unlocked when the game's own answer is yes (save flag
// m_FightingStyleProgresses[id] >= 1.0, school style) OR the Gleam level reaches the entry's level. An entry WITHOUT the field is always unlocked.
#ifndef LANNATIVE_STYLEGATE_H
#define LANNATIVE_STYLEGATE_H
#include <stdlib.h>
#define SG_MAX_LEVEL 99
// "6" -> 6; NULL or "" -> 0 (no gate); anything else (not a whole number 1..99) -> -1 (invalid)
static int sg_parse_level(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return -1;
    int v = atoi(s); return v >= 1 && v <= SG_MAX_LEVEL ? v : -1;
}
// gate_level: 0 = not gated; game_answer: the unmodified BPF_IsFightingStyleUnlocked result; gleam_level: m_iResourcesLvl of that stats component, -1 when unreadable
static int sg_unlocked(int gate_level, int game_answer, int gleam_level) {
    if (gate_level <= 0) return 1;
    if (game_answer) return 1;
    return gleam_level >= gate_level;
}
// The vanilla button (BP_FightingStyleButton_C) shows a lock icon and the text "lvl N" from its instance variable `Reward` (ECharacterProgressionRewardTypes = an index into
// Progression.m_Rewards): when m_Rewards[Reward].m_eUnlockCondition is GleamLvl it prints m_iUnlockParameter. A freshly created button has Reward = 0 (a Tutorial reward) and shows a bare
// lock. The vanilla GleamLvl rows are [42] level 6 (Faejin, UnlockFS_5), [43] level 2, [44] level 4; -1 = no such row, the label stays empty.
static int sg_reward_for_level(int level) {
    switch (level) { case 6: return 42; case 2: return 43; case 4: return 44; default: return -1; }
}
#endif

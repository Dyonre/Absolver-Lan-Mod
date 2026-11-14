// learn.inc: rows.json adds with "learn": "always" count as learned for every character, without editing the save.
// Included from content.inc (needs CA for hook_symbol and g_rowops from rowedit.inc); pure part: learn.h.
// The game's one learned-check: UStatsComponent::BPF_GetAttackLearningState(this, const FName *attack) -> EAttackLearningState
//   (0x4BFCD0: Unknown 0, Learning 1 = has an m_AttackProgress entry, Unlocked 2 = in UAttackComponent::m_UnlockedAttacks).
//   FilterAttack (the deck editor's attack lists, EnumerateAttacksFromFilter) and the mastery UI call it. The detour answers Unlocked (2) for the
//   learn rows and passes everything else on. Nothing is written to the save: m_UnlockedAttacks / m_AttackProgress stay as they are (XP gained with
//   such a move may still create an m_AttackProgress entry the normal way). Combat decks are not gated by learning.
#include "learn.h"
static LearnSet g_learn; static uint64_t g_learn_fn[LEARN_MAX]; static const RowOp *g_learn_src; static int g_learn_src_n = -1;
static volatile LONG g_learn_hits; static uint8_t (*g_o_learnstate)(void *, const uint64_t *);
static void learn_refresh(void) {                                   // the set follows g_rowops (boot load; a hot reload replaces the array)
    if (g_learn_src == g_rowops.a && g_learn_src_n == g_rowops.count) return;
    learn_collect(&g_rowops, &g_learn);
    for (int k = 0; k < g_learn.n; k++) { wchar_t w[ROWS_NAME_MAX]; widen(g_learn.name[k], w, ROWS_NAME_MAX); FNameRaw f = fname_of(w); memcpy(&g_learn_fn[k], &f, 8); }
    g_learn_src = g_rowops.a; g_learn_src_n = g_rowops.count;
    logf_("learn: %d attack row(s) count as learned (\"learn\": \"always\")%s%s%s", g_learn.n, g_learn.n ? ", first " : "", g_learn.n ? g_learn.name[0] : "", g_learn.dropped ? " - LIMIT: more than 512, the rest are NOT learned" : "");
}
static uint8_t learn_state_det(void *stats, const uint64_t *name) {
    if (name && g_rowops.count) {
        learn_refresh();
        for (int k = 0; k < g_learn.n; k++) if (g_learn_fn[k] == *name) {
            LONG h = InterlockedIncrement(&g_learn_hits);
            if (h <= 3 || (h & (h - 1)) == 0) logf_("learn: %s reported as learned (Unlocked) - query %ld", g_learn.name[k], h);    // first 3, then powers of two
            return 2;
        }
    }
    return g_o_learnstate(stats, name);
}
static void learn_install(void) {
    if (g_o_learnstate) return;
    g_o_learnstate = (uint8_t (*)(void *, const uint64_t *))CA->hook_symbol(CA, "UStatsComponent::BPF_GetAttackLearningState", (void *)learn_state_det);
    logf_("learn: learned-check detour %s", g_o_learnstate ? "installed" : "NOT installed - \"learn\": \"always\" rows need `mastery learn` on the save");
}

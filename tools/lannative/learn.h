#ifndef LANNATIVE_LEARN_H
#define LANNATIVE_LEARN_H
// learn.h - which attack rows count as learned for every character: the rows.json adds that carry "learn": "always".
// Pure logic (no engine calls). The engine side (detour of UStatsComponent::BPF_GetAttackLearningState) is learn.inc.
// Row names compare case-insensitively, like FNames. Only "attacks" table adds count.
#include "rowsjson.h"

#define LEARN_MAX 512                          // fixed cap, logged when exceeded
typedef struct { int n, dropped; char name[LEARN_MAX][ROWS_NAME_MAX]; } LearnSet;

// Collects the distinct "learn: always" attack rows of ops (load order kept). Returns the number collected.
static int learn_collect(const RowOps *ops, LearnSet *s) {
    s->n = 0; s->dropped = 0;
    for (int i = 0; i < ops->count; i++) {
        const RowOp *o = &ops->a[i];
        if (!o->is_add || !o->learn || _stricmp(o->table, "attacks")) continue;
        int dup = 0; for (int k = 0; k < s->n && !dup; k++) if (!_stricmp(s->name[k], o->row)) dup = 1;
        if (dup) continue;
        if (s->n == LEARN_MAX) { s->dropped++; continue; }
        snprintf(s->name[s->n++], ROWS_NAME_MAX, "%s", o->row);
    }
    return s->n;
}
static int learn_has(const LearnSet *s, const char *row) { for (int k = 0; k < s->n; k++) if (!_stricmp(s->name[k], row)) return 1; return 0; }
#endif

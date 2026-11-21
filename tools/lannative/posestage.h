// posestage.h - the bookkeeping behind API 2.32 pose_stage / pose_handoff. Pure code. The pose maths (local space, inertialization) is tools/posekit/posestage.h; posestage.inc does the engine side.
// One stage per skeletal mesh, owned by one plugin. A slot keeps its PsStage memory when it is switched off (the FinalizeBoneTransform detour may
// still be reading it on an animation thread), so a later stage reuses it instead of freeing it.
#ifndef LANNATIVE_POSESTAGE_H
#define LANNATIVE_POSESTAGE_H
#include "../posekit/posestage.h"
#define PST_MAX 8
typedef struct {
    void *mesh, *pawn; int plugin; unsigned gen; volatile int live;
    PsStage *st;                       // allocated once per slot, never freed
    unsigned frame; int dups;          // last loader frame the mesh was staged in; extra calls in one frame (they get dt 0)
    int shown;                         // poses staged since the stage went on (pose_handoff needs one)
} PstSlot;
typedef struct { PstSlot s[PST_MAX]; } PstTable;

static PstSlot *pst_find(PstTable *t, const void *mesh) { for (int i = 0; i < PST_MAX; i++) if (t->s[i].live && t->s[i].mesh == mesh) return &t->s[i]; return NULL; }
// a free slot (one with memory first); NULL = all PST_MAX in use
static PstSlot *pst_free_slot(PstTable *t) {
    PstSlot *e = NULL;
    for (int i = 0; i < PST_MAX; i++) if (!t->s[i].live) { if (t->s[i].st) return &t->s[i]; if (!e) e = &t->s[i]; }
    return e;
}
static int pst_live_count(const PstTable *t) { int n = 0; for (int i = 0; i < PST_MAX; i++) n += t->s[i].live != 0; return n; }
// the game time to advance this call: frame_dt once per loader frame, 0 for a second call in the same frame (the pose is re-applied, not aged)
static float pst_step_dt(PstSlot *s, unsigned frame, float frame_dt) {
    if (s->shown && s->frame == frame) { s->dups++; return 0.f; }
    s->frame = frame; s->shown++;
    return frame_dt > 0.f && frame_dt < 0.25f ? frame_dt : 1.f / 60.f;
}
#endif

/* posestage.h - inertialized pose hand-offs on a skeletal mesh's component-space bone buffer (pure C, header only, no engine calls). Own code.
   Pose stage: a plugin that switches clips (dynamic montages in the Cinematic slot) cuts to the new
   clip with blend 0 and calls ps_handoff; from the next frame the difference between the pose that was on screen and the new clip is decayed away
   (inertialize.h: Bollo offsets, critically damped, overshoot guard) instead of a crossfade that dips through both poses.
   Use, inside a FinalizeBoneTransform detour, after the engine filled the read buffer and BEFORE any IK:
       ps_frame(&stage, read_buffer, bones, dt)   every frame of the mesh while the stage may be needed (it keeps the last two shown poses)
       ps_handoff(&stage, decay)                  at a clip switch (any thread that the caller syncs; it only sets a flag)
   Works in LOCAL space (parent table): offsets of a parent carry its children, so bone lengths stay what the clips have during the blend.
   dt = the game time the animation advanced (slow motion included). Bones must be ordered parents before children (ps_init checks). */
#ifndef POSEKIT_POSESTAGE_H
#define POSEKIT_POSESTAGE_H
#include "inertialize.h"

#define PS_MAX_BONES 256
typedef struct PsStage {
    int n, par[PS_MAX_BONES];
    InXf hist[2][PS_MAX_BONES]; int hist_n; float hist_dt;   /* the last two shown poses (local), hist[1] = newest, hist_dt = time between them */
    InXf loc[PS_MAX_BONES];
    InBone b[PS_MAX_BONES]; Inert I;
    float pending;                                           /* > 0: decay (s) of a hand-off that starts with the next frame */
    int switches, ignored;                                   /* hand-offs started / dropped for want of a shown pose (for logs) */
} PsStage;

static void ps_qrot(const float *q, const float *v, float *o) {   /* o = q v q* */
    float t[3] = { 2.f * (q[1] * v[2] - q[2] * v[1]), 2.f * (q[2] * v[0] - q[0] * v[2]), 2.f * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
/* component = parent (component) applied to local: rotation p.q * l.q, translation p.t + p.q (p.s * l.t), scale p.s * l.s (UE: Local * Parent) */
static void ps_compose(const InXf *p, const InXf *l, InXf *o) {
    float st[3] = { p->s[0] * l->t[0], p->s[1] * l->t[1], p->s[2] * l->t[2] }, r[3]; InXf x;
    in_qmul(p->q, l->q, x.q); ps_qrot(p->q, st, r);
    for (int i = 0; i < 3; i++) { x.t[i] = p->t[i] + r[i]; x.s[i] = p->s[i] * l->s[i]; }
    x.t[3] = x.s[3] = 0.f; *o = x;
}
/* local = parent^-1 applied to component (exact for uniform parent scale, like the engine's own GetRelativeTransform) */
static void ps_relative(const InXf *p, const InXf *c, InXf *o) {
    float pc[4], d[3], r[3]; InXf x; in_qconj(p->q, pc); in_qmul(pc, c->q, x.q);
    for (int i = 0; i < 3; i++) d[i] = c->t[i] - p->t[i];
    ps_qrot(pc, d, r);
    for (int i = 0; i < 3; i++) { float s = p->s[i] != 0.f ? p->s[i] : 1.f; x.t[i] = r[i] / s; x.s[i] = c->s[i] / s; }
    x.t[3] = x.s[3] = 0.f; *o = x;
}
static void ps_to_local(const InXf *cs, const int *par, int n, InXf *ls) { for (int i = 0; i < n; i++) if (par[i] < 0) ls[i] = cs[i]; else ps_relative(&cs[par[i]], &cs[i], &ls[i]); }
/* in place is fine (parents are written before their children) */
static void ps_to_comp(const InXf *ls, const int *par, int n, InXf *cs) { for (int i = 0; i < n; i++) if (par[i] < 0) cs[i] = ls[i]; else ps_compose(&cs[par[i]], &ls[i], &cs[i]); }

/* 1 = ready; 0 = too many bones or a parent that is not before its child */
static int ps_init(PsStage *S, const int *par, int n) {
    memset(S, 0, sizeof *S);
    if (n <= 0 || n > PS_MAX_BONES) return 0;
    for (int i = 0; i < n; i++) { if (par[i] >= i) return 0; S->par[i] = par[i]; }
    S->n = n; S->I.n = n; S->I.b = S->b; return 1;
}
static void ps_handoff(PsStage *S, float decay) { S->pending = decay > 0.01f ? decay : 0.01f; }
static int ps_active(const PsStage *S) { return S->I.active || S->pending > 0.f; }

/* one frame: cs = the engine's component-space pose for this frame (n bones), changed in place while a hand-off runs. Returns 1 if cs was changed. */
static int ps_frame(PsStage *S, InXf *cs, float dt) {
    if (S->n <= 0) return 0;
    int n = S->n, changed = 0;
    ps_to_local(cs, S->par, n, S->loc);
    if (S->pending > 0.f) {
        if (S->hist_n >= 1) {   /* source = what was shown (and its motion), target = the new clip at full weight from this frame */
            in_begin(&S->I, S->hist_n >= 2 ? S->hist[0] : NULL, S->hist[1], NULL, S->loc, S->hist_n >= 2 ? S->hist_dt : 0.f, S->pending);
            S->switches++;
        } else S->ignored++;
        S->pending = 0.f;
    }
    if (S->I.active) {
        in_step(&S->I, dt);
        in_apply(&S->I, S->loc, S->loc);
        if (in_done(&S->I)) in_end(&S->I);
        ps_to_comp(S->loc, S->par, n, cs); changed = 1;
    }
    memcpy(S->hist[0], S->hist[1], sizeof(InXf) * (size_t)n); memcpy(S->hist[1], S->loc, sizeof(InXf) * (size_t)n);
    S->hist_dt = dt; if (S->hist_n < 2) S->hist_n++;
    return changed;
}
/* forget the shown poses (e.g. the mesh was rebound or skating stopped long ago): the next hand-off waits for one frame of history */
static void ps_reset(PsStage *S) { S->hist_n = 0; S->pending = 0.f; in_end(&S->I); }
#endif

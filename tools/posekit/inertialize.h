/* inertialize.h - per-bone inertialization for pose handovers (pure C, header only, no engine calls). Own code.
   Use: the loader's pose stage service and any plugin that switches poses (clip -> clip, combat <-> ride).

   Method (values / maths from the public literature, no copied code):
     * Inertialization (D. Bollo, "Inertialization: High-Performance Animation Transitions in Gears of War", GDC 2018): at a switch, store the
       per-bone OFFSET between the old (source) pose and the new (target) pose plus the offset's velocity, then show target + decaying offset.
       The new pose is evaluated at full weight from the first frame; only the offset is blended away. Bollo decays it with a quintic; here:
     * Critically damped decay (D. Holden, "Spring-It-On: The Game Developer's Spring-Roll-Call", theorangeduck.com): per component
         x(t) = (x0 + (v0 + y x0) t) e^(-y t)
       evaluated in CLOSED FORM from the time since the switch, so the result does not depend on the frame rate or how dt is split.
     * Overshoot guard (Bollo's idea of limiting the start velocity so the curve does not cross the target): the start velocity's part along the
       offset is clamped to >= -y |x0|. Then the offset never crosses zero along its start direction (proof: its projection is
       (|x0| + (v_par + y |x0|) t) e^(-y t) with a non-negative bracket). Sideways velocity decays as v t e^(-y t) (never crosses zero either).
     * Rotations: offset quaternion q_off = q_src * conj(q_dst) (shortest arc), turned into a rotation vector (log map) and decayed like a vector;
       applied as exp(r(t)) * q_dst. Angular velocity from the previous two source frames: log(q_src * conj(q_src_prev)) / dt.

   Space: bones are whatever space the caller blends in (local / parent space recommended: offsets of a parent then carry the children).
   Transform layout = UE4 FTransform (q x y z w, t x y z pad, s x y z pad; 16 bytes each) = a plugin's IK transform type, so a mesh's bone buffers can be
   passed as they are. Scale is not inertialized (the target's scale is used).
   Parameter: decay = seconds after which a still offset (v0 = 0) is down to 1 % (y = 6.6384 / decay). 0.2-0.3 s. */
#ifndef POSEKIT_INERTIALIZE_H
#define POSEKIT_INERTIALIZE_H
#include <math.h>
#include <string.h>

typedef struct InXf { float q[4], t[4], s[4]; } InXf;
typedef struct InBone { float x0[3], v0[3], r0[3], w0[3]; } InBone;   /* translation offset + velocity (cm, cm/s), rotation vector + rate (rad, rad/s) */
typedef struct Inert {
    int n;            /* bones in b[] */
    double t;         /* seconds since in_begin (double: sums of many small dt stay exact enough for the rate-independence test) */
    float y;          /* decay rate (1/s) */
    int active;       /* 0 = no offset (in_apply copies the target) */
    int clamped;      /* bones whose start velocity was clamped by the overshoot guard at the last in_begin (log this) */
    InBone *b;        /* caller-owned, n entries */
} Inert;

#define IN_Y_PER_DECAY 6.6384f   /* (1 + u) e^-u = 0.01 at u = 6.6384 */

static void in_qmul(const float *a, const float *b, float *o) {
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1], y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3], w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void in_qconj(const float *q, float *o) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
static void in_qlog(const float *qi, float *r) {            /* unit quaternion -> rotation vector (axis * angle), shortest arc */
    float q[4]; memcpy(q, qi, sizeof q); if (q[3] < 0.f) for (int i = 0; i < 4; i++) q[i] = -q[i];
    float s = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2]);
    float k = s < 1e-7f ? 2.f / (q[3] > 1e-7f ? q[3] : 1.f) : 2.f * atan2f(s, q[3]) / s;
    for (int i = 0; i < 3; i++) r[i] = q[i] * k;
}
static void in_qexp(const float *r, float *q) {             /* rotation vector -> unit quaternion */
    float a = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (a < 1e-7f) { q[0] = r[0] * .5f; q[1] = r[1] * .5f; q[2] = r[2] * .5f; q[3] = 1.f;
        float l = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + 1.f); for (int i = 0; i < 4; i++) q[i] /= l; return; }
    float k = sinf(a * .5f) / a; q[0] = r[0] * k; q[1] = r[1] * k; q[2] = r[2] * k; q[3] = cosf(a * .5f);
}
static void in_guard(const float *x0, float *v0, float y, int *hit) {   /* overshoot guard: v0 along x0 >= -y |x0| */
    float l = sqrtf(x0[0]*x0[0] + x0[1]*x0[1] + x0[2]*x0[2]); if (l < 1e-9f) return;
    float d[3] = { x0[0] / l, x0[1] / l, x0[2] / l }, vp = v0[0]*d[0] + v0[1]*d[1] + v0[2]*d[2], lo = -y * l;
    if (vp < lo) { for (int i = 0; i < 3; i++) v0[i] += d[i] * (lo - vp); (*hit)++; }
}

/* Start (or restart) a handover. src / src_prev: the pose shown now and one frame before (dt_src apart) - pass the previous OUTPUTS of in_apply to
   chain a new switch onto a running one. dst / dst_prev: the new pose now and one frame before (dst_prev NULL = treat the new pose as still).
   dt_src <= 0 = no velocity (offset only). */
static void in_begin(Inert *I, const InXf *src_prev, const InXf *src, const InXf *dst_prev, const InXf *dst, float dt_src, float decay) {
    I->y = IN_Y_PER_DECAY / (decay > 1e-4f ? decay : 1e-4f); I->t = 0.0; I->active = 1; I->clamped = 0;
    float inv = dt_src > 1e-6f ? 1.f / dt_src : 0.f;
    for (int k = 0; k < I->n; k++) {
        InBone *B = &I->b[k]; float c[4], d[4], vs[3], vd[3] = { 0, 0, 0 };
        for (int i = 0; i < 3; i++) {
            B->x0[i] = src[k].t[i] - dst[k].t[i];
            vs[i] = src_prev ? (src[k].t[i] - src_prev[k].t[i]) * inv : 0.f;
            vd[i] = dst_prev ? (dst[k].t[i] - dst_prev[k].t[i]) * inv : 0.f;
            B->v0[i] = vs[i] - vd[i];
        }
        in_qconj(dst[k].q, c); in_qmul(src[k].q, c, d); in_qlog(d, B->r0);
        if (src_prev) { in_qconj(src_prev[k].q, c); in_qmul(src[k].q, c, d); in_qlog(d, vs); } else vs[0] = vs[1] = vs[2] = 0.f;
        if (dst_prev) { in_qconj(dst_prev[k].q, c); in_qmul(dst[k].q, c, d); in_qlog(d, vd); } else vd[0] = vd[1] = vd[2] = 0.f;
        for (int i = 0; i < 3; i++) B->w0[i] = (vs[i] - vd[i]) * inv;
        in_guard(B->x0, B->v0, I->y, &I->clamped); in_guard(B->r0, B->w0, I->y, &I->clamped);
    }
}
static void in_step(Inert *I, float dt) { if (I->active && dt > 0.f) I->t += dt; }
static int in_done(const Inert *I) { return !I->active || I->y * I->t > 12.0; }   /* (1 + 12) e^-12 < 1e-4 of the start offset */
static void in_end(Inert *I) { I->active = 0; }

/* out[k] = dst[k] with the decayed offset at time I->t (out may alias dst) */
static void in_apply(const Inert *I, const InXf *dst, InXf *out) {
    if (!I->active || in_done(I)) { if (out != dst) memcpy(out, dst, sizeof(InXf) * (size_t)I->n); return; }
    float t = (float)I->t, e = expf(-I->y * t);
    for (int k = 0; k < I->n; k++) {
        const InBone *B = &I->b[k]; float r[3], qo[4], q[4];
        for (int i = 0; i < 3; i++) {
            out[k].t[i] = dst[k].t[i] + (B->x0[i] + (B->v0[i] + I->y * B->x0[i]) * t) * e;
            r[i] = (B->r0[i] + (B->w0[i] + I->y * B->r0[i]) * t) * e;
        }
        out[k].t[3] = dst[k].t[3];
        in_qexp(r, qo); in_qmul(qo, dst[k].q, q); memcpy(out[k].q, q, sizeof q);
        if (out != dst) memcpy(out[k].s, dst[k].s, sizeof out[k].s);
    }
}
/* for logs: largest current translation offset (cm) and rotation offset (deg) over all bones */
static void in_stats(const Inert *I, float *max_cm, float *max_deg) {
    *max_cm = 0.f; *max_deg = 0.f; if (!I->active) return;
    float t = (float)I->t, e = expf(-I->y * t);
    for (int k = 0; k < I->n; k++) { const InBone *B = &I->b[k]; float a = 0.f, b = 0.f;
        for (int i = 0; i < 3; i++) { float x = (B->x0[i] + (B->v0[i] + I->y * B->x0[i]) * t) * e, r = (B->r0[i] + (B->w0[i] + I->y * B->r0[i]) * t) * e; a += x * x; b += r * r; }
        a = sqrtf(a); b = sqrtf(b) * 57.29578f; if (a > *max_cm) *max_cm = a; if (b > *max_deg) *max_deg = b; }
}
#endif

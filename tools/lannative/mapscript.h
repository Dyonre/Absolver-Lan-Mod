// mapscript.h - pure logic of the map script (mapscript.inc): key-frame curves, phase flags and conditions, scene-to-world placement, trigger boxes and edges, timed mesh moves.
// Offline tests: test\mapscript_test.c. No engine calls in here.
#ifndef LANNATIVE_MAPSCRIPT_H
#define LANNATIVE_MAPSCRIPT_H
#include <math.h>
#include <string.h>
#include <stdio.h>
#define MS_KEYS 16
#define MS_PHASES 24
// ----- curves: linear key-frames, optionally looping. dims 1 = float (v[0]), 3 = linear RGB
typedef struct { char id[32]; int dims, n; float loop, t[MS_KEYS], v[MS_KEYS][3]; } MsCurve;
static int ms_curve_add(MsCurve *c, float t, const float *v) {         // keys must be added with ascending t; 0 = full / not ascending
    if (c->n >= MS_KEYS || (c->n && t < c->t[c->n - 1])) return 0; c->t[c->n] = t; for (int i = 0; i < 3; i++) c->v[c->n][i] = (i < c->dims) ? v[i] : 0; c->n++; return 1;
}
static void ms_curve_eval(const MsCurve *c, double time, float out[3]) {
    out[0] = out[1] = out[2] = 0; if (c->n <= 0) return;
    double t = time; if (c->loop > 0) { t = fmod(time, (double)c->loop); if (t < 0) t += c->loop; }
    if (t <= c->t[0] || c->n == 1) { memcpy(out, c->v[0], sizeof(float) * 3); return; }
    if (t >= c->t[c->n - 1]) { memcpy(out, c->v[c->n - 1], sizeof(float) * 3); return; }
    for (int k = 1; k < c->n; k++) if (t <= c->t[k]) {
        double span = (double)c->t[k] - c->t[k - 1], f = span > 1e-9 ? (t - c->t[k - 1]) / span : 1.0;
        for (int i = 0; i < 3; i++) out[i] = (float)(c->v[k - 1][i] + (c->v[k][i] - c->v[k - 1][i]) * f);
        return;
    }
}
// ----- calibration: per scene look knobs so authored (neon) colours sit in the vanilla game's range. cal = { gamma, intensity_scale, saturation }, identity = { 1, 1, 1 }
static void ms_cal_default(float cal[3]) { cal[0] = cal[1] = cal[2] = 1.0f; }
static int ms_cal_ok(const float cal[3]) { return cal[0] >= 0.2f && cal[0] <= 5.0f && cal[1] >= 0.0f && cal[1] <= 10.0f && cal[2] >= 0.0f && cal[2] <= 2.0f; }
static void ms_calibrate(const float cal[3], const float c[3], float out[3]) {   // saturation first (lerp from Rec.709 luma, clamped at 0), then gamma (out = c^gamma, 1 = unchanged)
    float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    for (int i = 0; i < 3; i++) { float v = y + (c[i] - y) * cal[2]; if (v < 0) v = 0; out[i] = (cal[0] == 1.0f || v <= 0) ? v : powf(v, cal[0]); }
}
// ----- phases: named on/off flags that lights and actions test
typedef struct { char name[24]; int on; } MsPhase;
typedef struct { MsPhase p[MS_PHASES]; int n; } MsPhases;
static int ms_phase_set(MsPhases *ph, const char *name, int on) {      // 1 = ok (created on first use), 0 = table full / bad name
    if (!name || !*name || strlen(name) >= sizeof ph->p[0].name) return 0;
    for (int i = 0; i < ph->n; i++) if (!strcmp(ph->p[i].name, name)) { ph->p[i].on = on != 0; return 1; }
    if (ph->n >= MS_PHASES) return 0; snprintf(ph->p[ph->n].name, sizeof ph->p[0].name, "%s", name); ph->p[ph->n].on = on != 0; ph->n++; return 1;
}
static int ms_phase_get(const MsPhases *ph, const char *name) { for (int i = 0; i < ph->n; i++) if (!strcmp(ph->p[i].name, name)) return ph->p[i].on; return 0; }
// condition text: "always" | "never" | "phase:<name>" | "!phase:<name>"; NULL or "" = always. Unknown text = never
static int ms_cond(const MsPhases *ph, const char *cond) {
    if (!cond || !*cond || !strcmp(cond, "always")) return 1; if (!strcmp(cond, "never")) return 0;
    if (!strncmp(cond, "phase:", 6)) return ms_phase_get(ph, cond + 6); if (!strncmp(cond, "!phase:", 7)) return !ms_phase_get(ph, cond + 7); return 0;
}
// ----- placement: relative scenes use the player's position and yaw as the origin (x forward, y right, z up); absolute scenes add a fixed offset
static void ms_world_pos(int relative, const float base[3], float yaw_deg, const float local[3], float out[3]) {
    if (!relative) { for (int i = 0; i < 3; i++) out[i] = base[i] + local[i]; return; }
    double a = yaw_deg * 3.14159265358979 / 180.0, c = cos(a), s = sin(a);
    out[0] = (float)(base[0] + local[0] * c - local[1] * s); out[1] = (float)(base[1] + local[0] * s + local[1] * c); out[2] = base[2] + local[2];
}
static int ms_in_box(const float p[3], const float center[3], const float half[3]) { for (int i = 0; i < 3; i++) if (fabsf(p[i] - center[i]) > half[i]) return 0; return 1; }
static int ms_edge(int *inside, int now) { int e = now && !*inside ? 1 : (!now && *inside ? -1 : 0); *inside = now != 0; return e; }   // +1 entered, -1 left
// ----- timed move of a mesh between two positions (smoothstep)
typedef struct { float from[3], to[3], t, dur; int active; } MsMove;
static void ms_move_start(MsMove *m, const float from[3], const float to[3], float dur) {
    memcpy(m->from, from, sizeof m->from); memcpy(m->to, to, sizeof m->to); m->t = 0; m->dur = dur; m->active = 1;
}
static int ms_move_step(MsMove *m, float dt, float out[3]) {            // 1 = still moving (out = new position), 0 = finished or idle (out = target when it just finished)
    if (!m->active) return 0; m->t += dt; float f = m->dur > 1e-4f ? m->t / m->dur : 1.0f; int done = f >= 1.0f; if (done) f = 1.0f;
    float e = f * f * (3 - 2 * f); for (int i = 0; i < 3; i++) out[i] = m->from[i] + (m->to[i] - m->from[i]) * e;
    if (done) { m->active = 0; return 0; } return 1;
}
// ----- grids (animated floor panels): time offset of cell (ix, iy) in an nx x ny grid. pattern: 0 none, 1 checker, 2 wave (diagonal), 3 rows, 4 columns, 5 ring (from the centre)
static int ms_grid_pattern(const char *s) {
    static const char *n[] = { "none", "checker", "wave", "rows", "columns", "ring" };
    if (!s || !*s) return 0; for (int i = 0; i < 6; i++) if (!strcmp(s, n[i])) return i; return -1;
}
static float ms_grid_phase(int pattern, int ix, int iy, int nx, int ny, float step) {
    switch (pattern) {
    case 1: return ((ix + iy) & 1) * step;
    case 2: return (ix + iy) * step;
    case 3: return iy * step;
    case 4: return ix * step;
    case 5: { float dx = ix - (nx - 1) * 0.5f, dy = iy - (ny - 1) * 0.5f; float d = fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy); return d * step; }   // square rings
    default: return 0;
    }
}
// ----- "cycle" action: csv = "a,b,c". Turns the phase after the first one that is on ON and every other listed phase OFF (none on: the first one). Returns the index now on, -1 = bad list
static int ms_cycle(MsPhases *ph, const char *csv) {
    char names[8][24]; int n = 0; const char *p = csv; if (!csv || !*csv) return -1;
    while (*p && n < 8) { const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p); if (!l || l >= sizeof names[0]) return -1; memcpy(names[n], p, l); names[n][l] = 0; n++; if (!e) break; p = e + 1; }
    if (!n) return -1; int cur = -1; for (int i = 0; i < n && cur < 0; i++) if (ms_phase_get(ph, names[i])) cur = i;
    int next = (cur + 1) % n; for (int i = 0; i < n; i++) ms_phase_set(ph, names[i], i == next); return next;
}
// ----- "every" events: accumulate dt, 1 when a period is complete (at most once per call; a long stall does not fire a burst)
static int ms_every(float *acc, float dt, float period) {
    if (!(period > 0)) return 0; *acc += dt; if (*acc < period) return 0; *acc -= period; if (*acc >= period) *acc = 0; return 1;
}
// ----- light budget: keep[i] = 1 for the `cap` entries with the highest score (stable: equal scores keep file order). Returns how many are kept
static int ms_rank_keep(const float *score, int n, int cap, unsigned char *keep) {
    int kept = 0; for (int i = 0; i < n; i++) keep[i] = 0; if (cap <= 0) return 0;
    for (int i = 0; i < n; i++) { int better = 0; for (int j = 0; j < n; j++) if (j != i && (score[j] > score[i] || (score[j] == score[i] && j < i))) better++; if (better < cap) { keep[i] = 1; kept++; } }
    return kept;
}
// ----- runtime navmesh ("navmesh": {center, half}): the box must be sane, and it reaches the engine as the ImportText of a BodySetup AggGeom (one box, FULL extents = 2 x half)
static int ms_nav_half_ok(const float h[3]) { for (int i = 0; i < 3; i++) if (!(h[i] >= 100 && h[i] <= 50000)) return 0; return 1; }
static int ms_nav_aggeom_text(const float h[3], char *out, int cap) {
    int n = snprintf(out, (size_t)cap, "(BoxElems=((Center=(X=0,Y=0,Z=0),X=%.1f,Y=%.1f,Z=%.1f)))", 2 * h[0], 2 * h[1], 2 * h[2]); return n > 0 && n < cap;
}
// a follower that moved more than `max_step` uu between two samples did not walk: a teleport / re-spawn (bot nav runs must have none)
static int ms_nav_is_jump(const float a[3], const float b[3], float max_step) { float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2]; return dx * dx + dy * dy + dz * dz > max_step * max_step; }
#endif

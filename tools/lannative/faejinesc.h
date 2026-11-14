// faejinesc.h - pure decision logic of the Faejin gold-LOOP counter shared by the two brakes (loader glue: faejinesc.inc; offline tests: test\faejinesc_test.c, test\faejinws_test.c).
// No game access, no allocation, no clock: the caller passes `now` in seconds.
//
// n counts the gold links that involve a style-ability row (into or out of an ability) of one Faejin character:
//   - it goes up by one per such link (fe_link), it is NOT cleared by a plain launch in between: "strafe -> gold -> plain attack -> strafe -> gold" is the same loop;
//   - hard reset: no link for t_reset seconds (a pause) or the character is hit (fe_reset)  -> n_eff = 0;
//   - optional leak: tau > 0 lets one link "cool" every tau seconds (default 0 = off: the loop is bounded, a pause resets it).
// Brake 1 (stamina escalation, optional second knob): cost multiplier mult(n) = min(cap, 1 + a * max(0, n_eff - n0)), only for style cfg.style (5 = Faejin), default OFF.
// Brake 2 (window shrink, faejinws.h) reads the same n_eff.
#ifndef FAEJINESC_H
#define FAEJINESC_H

typedef struct { int enabled; int style; double n0, a, cap, tau, t_reset; } FeCfg;
typedef struct { double n, t_last; int last_chain; int last_ability; } FeState;

static FeCfg fe_cfg_default(void) { FeCfg c; c.enabled = 0; c.style = 5; c.n0 = 2.0; c.a = 0.35; c.cap = 3.0; c.tau = 0.0; c.t_reset = 1.5; return c; }

// effective link count now (optional leak + hard reset after a pause); never negative
static double fe_n_eff(const FeCfg *c, const FeState *s, double now) {
    double gap = now - s->t_last; if (gap < 0) gap = 0;                       // clock went back (world change): no leak, no reset
    if (c->t_reset > 0 && gap > c->t_reset) return 0.0;
    double n = s->n - (c->tau > 0 ? gap / c->tau : 0.0);
    return n > 0 ? n : 0.0;
}
// one more gold link that involves an ability row, at time `now`
static void fe_link(const FeCfg *c, FeState *s, double now) { s->n = fe_n_eff(c, s, now) + 1.0; s->t_last = now; }
// the character was hit (or any other hard reset)
static void fe_reset(FeState *s) { s->n = 0; s->t_last = 0; }
// 1 when the engine's perfect-link chain counter rose with this launch (a new gold link), 0 for a plain launch (chain 0) or a replay of the same launch
static int fe_chain_rose(int prev_chain, int chain) { return chain > 0 && chain > prev_chain; }
// stamina cost multiplier for this character right now (1.0 = unchanged)
static double fe_mult(const FeCfg *c, const FeState *s, int style, double now) {
    if (!c->enabled || style != c->style) return 1.0;
    double over = fe_n_eff(c, s, now) - c->n0; if (over < 0) over = 0;
    double m = 1.0 + c->a * over;
    return m > c->cap ? c->cap : m < 1.0 ? 1.0 : m;
}
#endif

// faejinws.h - pure decision logic of the Faejin gold-link PRESS WINDOW SHRINK (loader glue: faejinesc.inc).
// No game access, no allocation, no clock: the caller passes times in seconds.
//
// Why: in the stock game a Faejin style-ability press is accepted as a gold link from ~8 frames BEFORE the window opens
// (the ability input lives 0.3 s in the input stack) up to the end of the 2 F "Perfect release" notify, so a perfectly timed press is not needed at all.
// This rule narrows the accepted press time per consecutive gold link:
//   n          = gold links already chained (leaky counter, faejinesc.h: fe_n_eff; 0 for the first link out of a plain attack)
//   width(n)   = schedule[n] frames (default 2, 1.5, 1, 0.5); past the end of the list: 0 = no gold link, the press falls back to the plain link
//   accepted   = center - width/2 <= (t_press - t_open) * 30 <= center + width/2        (t_open = the frame the PerfectAttackOnly layer was pushed; center default 1 F = middle of the 2 F notify)
// So link 1 needs the press inside the real 2 F window (no early buffer), link 2 inside the middle 1.5 F of it, ... link 4 inside 0.5 F (16 ms), link 5 never.
// Active only when enabled and the character's style == cfg.style (5 = Faejin) and the character is the local player. Default OFF.
#ifndef FAEJINWS_H
#define FAEJINWS_H
#define WS_MAX 8
typedef struct { int enabled; int style; double center; int nw; double w[WS_MAX]; } WsCfg;

static WsCfg ws_cfg_default(void) { WsCfg c; c.enabled = 0; c.style = 5; c.center = 1.0; c.nw = 4; c.w[0] = 2.0; c.w[1] = 1.5; c.w[2] = 1.0; c.w[3] = 0.5; for (int i = 4; i < WS_MAX; i++) c.w[i] = 0; return c; }

// accepted width in frames for a counter value (rounded to the nearest whole link); 0 = no gold link
static double ws_width(const WsCfg *c, double n_eff) {
    int i = (int)(n_eff + 0.5); if (i < 0) i = 0;
    return i < c->nw && i < WS_MAX ? c->w[i] : 0.0;
}
// 1 = the gold link may proceed, 0 = veto. press_minus_open_frames: (press time - window open time) in 1/30 s frames, negative = pressed before the window opened.
// have_times = 0 (press or window time unknown): fail open (1), never block a link on missing data.
static int ws_accept(const WsCfg *c, int style, double n_eff, int have_times, double press_minus_open_frames) {
    if (!c->enabled || style != c->style || !have_times) return 1;
    double w = ws_width(c, n_eff); if (w <= 0) return 0;
    double lo = c->center - w / 2.0, hi = c->center + w / 2.0;
    return press_minus_open_frames >= lo - 1e-9 && press_minus_open_frames <= hi + 1e-9;
}
#endif

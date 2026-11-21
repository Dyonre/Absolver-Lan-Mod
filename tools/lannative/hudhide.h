// hudhide.h - bookkeeping behind API 2.30 hud_hide / hud_show (docs/API-REFERENCE.md). Pure code. hudhide.inc does the engine side (object scan, widget visibility, the AddToViewport / SetVisibility wrappers).
// The HUD is hidden while at least one plugin holds a hide; each widget the loader hid keeps the visibility it had, which comes back at the last show.
#ifndef LANNATIVE_HUDHIDE_H
#define LANNATIVE_HUDHIDE_H
#include <stdint.h>
#define HH_MAX 512
typedef struct { void *w[HH_MAX]; unsigned char vis[HH_MAX]; int n; uint64_t owners; } HhState;

static int hh_hidden(const HhState *s) { return s->owners != 0; }
// owner p takes / drops its hide; returns 1 when the HUD's hidden state flips (first hide, last show)
static int hh_owner(HhState *s, int p, int hide) {
    if (p < 0 || p > 63) return 0; int was = hh_hidden(s); uint64_t b = 1ull << p;
    if (hide) s->owners |= b; else s->owners &= ~b;
    return was != hh_hidden(s);
}
static int hh_find(const HhState *s, const void *w) { for (int i = 0; i < s->n; i++) if (s->w[i] == w) return i; return -1; }
// ESlateVisibility: 0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible, 4 SelfHitTestInvisible. Only shown widgets are hidden (and remembered).
static int hh_shown(unsigned char vis) { return vis != 1 && vis != 2; }
// remember a widget the loader is about to hide with its visibility; 0 = list full or already there
static int hh_add(HhState *s, void *w, unsigned char vis) { if (!w || s->n >= HH_MAX || hh_find(s, w) >= 0) return 0; s->w[s->n] = w; s->vis[s->n] = vis; s->n++; return 1; }
// the game made a hidden widget visible again: that is the visibility to give back at the show (the loader hides it again)
static int hh_update(HhState *s, const void *w, unsigned char vis) { int i = hh_find(s, w); if (i < 0 || !hh_shown(vis)) return 0; s->vis[i] = vis; return 1; }
#endif

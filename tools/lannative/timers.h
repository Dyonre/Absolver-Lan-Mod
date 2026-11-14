// timers.h - pure timer table of the script extender.
// 256 slots shared by all plugins, at most TM_PER_PLUGIN per plugin, ids are never reused (> 0), one-shot slots are freed when they fire, cancel is owner-checked.
#ifndef LANNATIVE_TIMERS_H
#define LANNATIVE_TIMERS_H
#include <string.h>
#define TM_MAX 256
#define TM_PER_PLUGIN 128
#define TM_PLUGINS 64
typedef struct { int id, plugin, repeat; float left, interval; void (*cb)(void *); void *user; } TmSlot;
typedef struct { TmSlot s[TM_MAX]; int next_id, used, per[TM_PLUGINS]; } TmTable;
enum { TM_OK = 0, TM_BAD = 1, TM_FULL = 2, TM_PLUGIN_FULL = 3 };
// returns the id (> 0) or 0 with *why set to TM_BAD / TM_FULL / TM_PLUGIN_FULL
static int tm_add(TmTable *t, int plugin, float seconds, int repeat, void (*cb)(void *), void *user, int *why) {
    int w = TM_OK; if (!why) why = &w;
    if (plugin < 0 || plugin >= TM_PLUGINS || !cb || !(seconds > 0)) { *why = TM_BAD; return 0; }
    if (t->per[plugin] >= TM_PER_PLUGIN) { *why = TM_PLUGIN_FULL; return 0; }
    for (int i = 0; i < TM_MAX; i++) if (!t->s[i].id) {
        TmSlot *s = &t->s[i]; s->id = ++t->next_id; s->plugin = plugin; s->left = s->interval = seconds; s->repeat = repeat != 0; s->cb = cb; s->user = user;
        t->used++; t->per[plugin]++; *why = TM_OK; return s->id;
    }
    *why = TM_FULL; return 0;
}
static void tm_free(TmTable *t, TmSlot *s) { t->per[s->plugin]--; t->used--; memset(s, 0, sizeof *s); }
// 1 if the timer existed, belonged to `plugin` and was removed (also safe from inside its own callback)
static int tm_cancel(TmTable *t, int plugin, int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < TM_MAX; i++) if (t->s[i].id == id) { if (t->s[i].plugin != plugin) return 0; tm_free(t, &t->s[i]); return 1; }
    return 0;
}
static int tm_cancel_plugin(TmTable *t, int plugin) { int n = 0; for (int i = 0; i < TM_MAX; i++) if (t->s[i].id && t->s[i].plugin == plugin) { tm_free(t, &t->s[i]); n++; } return n; }
// advance by dt; fire(ctx, plugin, cb, user) for every due timer. Timers added from inside a callback start counting next step. Returns the number fired.
static int tm_step(TmTable *t, float dt, void (*fire)(void *ctx, int plugin, void (*cb)(void *), void *user), void *ctx) {
    int fired = 0, last = t->next_id;
    for (int i = 0; i < TM_MAX; i++) {
        TmSlot *s = &t->s[i]; if (!s->id || s->id > last) continue;
        s->left -= dt; if (s->left > 0) continue;
        void (*cb)(void *) = s->cb; void *u = s->user; int p = s->plugin;
        if (s->repeat) { s->left += s->interval; if (s->left <= 0) s->left = s->interval; } else tm_free(t, s);
        fire(ctx, p, cb, u); fired++;
    }
    return fired;
}
#endif

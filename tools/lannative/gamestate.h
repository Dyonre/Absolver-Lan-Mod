// gamestate.h - the rules behind API 2.29 game_state() and on_world_ready (docs/API-REFERENCE.md). Pure code. gamestate.inc reads the engine and feeds these.
#ifndef LANNATIVE_GAMESTATE_H
#define LANNATIVE_GAMESTATE_H
#include <string.h>
#ifndef ABSOLVER_SE_H                         // the same values as AbsolverSE.h's enum (the offline test has no AbsolverSE.h)
#define ABS_GS_WORLD       0x001u
#define ABS_GS_MENU_WORLD  0x002u
#define ABS_GS_READY       0x004u
#define ABS_GS_LOADING     0x008u
#define ABS_GS_PAUSED      0x010u
#define ABS_GS_FOCUSED     0x020u
#define ABS_GS_GAME_MENU   0x040u
#define ABS_GS_LOADER_MENU 0x080u
#define ABS_GS_STALLED     0x100u
#endif
#define GS_READY_STEADY_S 1.0f                // the pawn must stay for this long (s) before a gameplay world counts as ready
#define GS_STALL_MS 300u                      // no game frame for this long = stalled (what a plugin's watchdog used)

// the title screen / menu worlds: no gameplay there, though the title screen has a pawn too
static int gs_menu_world(const char *path) { return path && (strstr(path, "/StartMap/") || strstr(path, "/Menu")); }

// once per world generation: waits for a pawn that stays GS_READY_STEADY_S in a gameplay world. Returns 1 in the step it becomes ready.
typedef struct { unsigned gen; int menu, ready; float steady; } GsWorld;
static int gs_world_step(GsWorld *w, unsigned gen, const char *path, int has_world, int has_pawn, float dt) {
    if (gen != w->gen) { w->gen = gen; w->ready = 0; w->steady = 0.f; w->menu = gs_menu_world(path); }
    if (w->ready || w->menu || !has_world) return 0;
    if (!has_pawn) { w->steady = 0.f; return 0; }
    w->steady += dt > 0.f && dt < 1.f ? dt : 0.f;
    if (w->steady < GS_READY_STEADY_S) return 0;
    w->ready = 1; return 1;
}
// the flag word game_state() returns
static unsigned gs_flags(const GsWorld *w, int has_world, int paused, int focused, int game_menu, int loader_menu, unsigned ms_since_tick, int on_game_thread) {
    unsigned f = 0;
    if (has_world) f |= ABS_GS_WORLD;
    if (has_world && w->menu) f |= ABS_GS_MENU_WORLD;
    if (has_world && w->ready) f |= ABS_GS_READY;
    if (!has_world || (!w->menu && !w->ready)) f |= ABS_GS_LOADING;
    if (paused) f |= ABS_GS_PAUSED;
    if (focused) f |= ABS_GS_FOCUSED;
    if (game_menu) f |= ABS_GS_GAME_MENU;
    if (loader_menu) f |= ABS_GS_LOADER_MENU;
    if (!on_game_thread && ms_since_tick >= GS_STALL_MS) f |= ABS_GS_STALLED;
    return f;
}
#endif

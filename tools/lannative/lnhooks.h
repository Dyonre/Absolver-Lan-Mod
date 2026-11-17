// ---------------------------------------------------------------------------------------------------------------------
// lnhooks.h - the loader's registration tables: every point where a unit plugs into the core (mechanism: lnreg.h,
// guide: README.md "Units and registration"). A unit registers next to its own code, e.g.
//     LN_INIT(0500, hostmods) { hostmods_install(); }
// and the core runs the table (LN_RUN(init)) without naming the unit. Lower keys run first; a key is used once per table.
// The log lists what each table holds, in order, right after the version line ("registry: <table> (<n>): name(key) ...").
// ---------------------------------------------------------------------------------------------------------------------
#ifndef LNHOOKS_H
#define LNHOOKS_H
#include "lnreg.h"

// every table of the loader, in the order the boot log lists them (a new table: add its name here, then its descriptor and macro below)
#define LN_TABLES(X) X(preinit) X(init) X(pretick) X(setick) X(tick) X(draw) X(api) X(content) X(modsrows) X(hsverb)
LN_TABLES(LN_TABLE)

// ---- start-up, in init() (DllMain of the proxy DLL: loader lock held, no SEH; only for the game exe started with -NoEAC)
// LN_PREINIT(key, name) { ... }   right after config.txt is read, before any hook is installed
// LN_INIT(key, name) { ... }      after the game-tick hook is installed (not reached when that hook fails)
#define LN_PREINIT(key, name) LN_HOOK(preinit, key, name)
#define LN_INIT(key, name)    LN_HOOK(init, key, name)

// ---- every game frame, on the game thread (the UGameEngine::Tick hook, before the engine's own tick)
// LN_PRETICK(key, name) { ... }   first, outside the loader's guard: also runs after the loader switched itself off (g_disabled); bring your own __try
// LN_TICK(key, name) { ... }      inside the loader's guarded frame work, after its own LAN / duel steps; an exception here switches the loader off
//                                 for the run and the log names the hook, so a unit that can fault keeps its own __try and fault counter
// g_ln_dt = the frame's delta time (s), g_ln_focus = the game window has the keyboard focus (sampled once, before the tick table)
static float g_ln_dt;
static int g_ln_focus;
#define LN_PRETICK(key, name) LN_HOOK(pretick, key, name)
#define LN_TICK(key, name)    LN_HOOK(tick, key, name)
// LN_SE_TICK(key, name) { ... }   the script extender's frame (secore.inc se_tick, inside the pretick hook "plugins"): only while at least one plugin or
//                                 content mod is loaded, after the world-change check and the timers, before the plugins' own on_tick callbacks
#define LN_SE_TICK(key, name) LN_HOOK(setick, key, name)

// ---- text on the game's canvas (the UGameViewportClient::PostRender hook, game thread, inside the loader's draw guard: 2 faults switch all drawing off)
// LN_DRAW(key, name, active, draw)   int active(void): 1 while there is something to show (cheap, called every frame);
//                                    void draw(void *canvas): called for every hook, in key order, in each frame in which the F1 menu or any hook is active
typedef struct { const char *name; const char *key; int (*active)(void); void (*draw)(void *canvas); } LnDraw;
#define LN_DRAW(key, name, active_fn, draw_fn) \
    static const LnDraw lnd_draw_##name = { #name, #key, active_fn, draw_fn }; LN_ENTRY(draw, key, name, &lnd_draw_##name)

// ---- the plugin API (AbsolverSE.h): a unit fills the AbsApi fields it implements
// LN_API(name) { a->my_call = api_p_my_call; }   `a` is the AbsApi being filled, once per plugin (plugins.inc plug_fill_api -> secore.inc se_fill_api).
// The field itself is appended to struct AbsApi in AbsolverSE.h (2.x: appended only); that header stays the one shared file of an API addition.
struct AbsApi;
typedef struct { const char *name; const char *key; void (*fn)(struct AbsApi *a); } LnApi;
#define LN_API(name) \
    static void lnh_api_##name(struct AbsApi *a); \
    static const LnApi lnd_api_##name = { #name, "", lnh_api_##name }; \
    LN_ENTRY_BY_NAME(api, name, &lnd_api_##name) \
    static void lnh_api_##name(struct AbsApi *a)

// ---- the content registry (content.inc content_init: the loader's built-in "Content" plugin). Runs once, after the paks are mounted and before the
// first tick, and only when at least one enabled mod ships Mods\<mod>\content\<kind>\*.json.
// LN_CONTENT(key, name) { ck_add(plug_of(a), "mykind", my_validate, my_apply, NULL); MY_API = a; }
//   `a` = the built-in plugin's AbsApi: keep it to call the plugin API from the unit. A kind's entries are validated / applied in the order the
//   kinds were added, so the key decides that order (at most 16 kinds, plugin kinds included).
typedef struct { const char *name; const char *key; void (*fn)(const struct AbsApi *a); } LnContent;
#define LN_CONTENT(key, name) \
    static void lnh_content_##name(const struct AbsApi *a); \
    static const LnContent lnd_content_##name = { #name, #key, lnh_content_##name }; \
    LN_ENTRY(content, key, name, &lnd_content_##name) \
    static void lnh_content_##name(const struct AbsApi *a)

// ---- rows on the F2 Mods page, after the plugin sections (modsui.inc; game thread; the page asks again every 0.5 s while it is open)
// LN_MODS_ROWS(key, name, count, label, action)   int count(void): how many rows now (build them here);  const char *label(int i): row i's text
//   (valid until the next count);  void action(int i): row i was chosen (Enter or a click). The units' rows follow each other in key order.
typedef struct { const char *name; const char *key; int (*count)(void); const char *(*label)(int i); void (*action)(int i); } LnRows;
#define LN_MODS_ROWS(key, name, count_fn, label_fn, action_fn) \
    static const LnRows lnd_modsrows_##name = { #name, #key, count_fn, label_fn, action_fn }; LN_ENTRY(modsrows, key, name, &lnd_modsrows_##name)

// ---- verbs of the host-mods side channel (hostmods.inc hs_serve: the host's TCP listener that answers joiners; not in the minimal build)
// LN_HS_VERB(name, "VERB", fn)   void fn(SOCKET c, const char *masked): a joiner sent the one-word request line "VERB"; answer on c (hs_send_all),
//   `masked` is the peer address for the log. Runs on the host's SERVER THREAD (not the game thread), one request at a time, inside its __try, and only
//   after the built-in checks passed: allow list, the address has joined this game, moddownload is not off. VERB: at most 7 characters; LIST and
//   GET are built in. A loader without the verb answers "ERR bad request".
typedef struct { const char *name; const char *key; const char *verb; void (*fn)(UINT_PTR socket, const char *masked); } LnVerb;
#define LN_HS_VERB(name, verb, fn) \
    typedef char lnv_hsverb_##name[sizeof(verb) <= 8 ? 1 : -1];     /* the request word is read with %7s */ \
    static const LnVerb lnd_hsverb_##name = { #name, "", verb, fn }; LN_ENTRY_BY_NAME(hsverb, name, &lnd_hsverb_##name)

#endif

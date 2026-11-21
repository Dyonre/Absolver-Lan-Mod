/* Cheats: the contract between plugin.c (game thread: the cheats, the item catalog, the inventory) and cheatsui.cpp (render thread: the ImGui window).
 * The window never touches engine objects: it reads plain values and queues requests, plugin.c runs them on the game thread. */
#ifndef CHEATSUI_H
#define CHEATSUI_H
#include "../../tools/lannative/AbsolverSE.h"
#include "catalog.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { CU_BUILD_NONE = 0, CU_BUILD_RUNNING = 1, CU_BUILD_READY = 2 };
/* implemented by plugin.c */
int          cheats_count(void);                          /* number of toggles (shards, god, stamina, onehit, fly) */
const char  *cheats_label(int i);
int          cheats_is_on(int i);
void         cheats_request_toggle(int i);                /* same code as the F1 menu entry */
void         cheats_request_heal(void);
float        cheats_speed(void);                          /* fly speed in uu/s */
void         cheats_set_speed(float v);
const char  *cheats_fly_key_name(void);
const char  *cheats_ui_key_name(void);
int          cheats_default_count(void);
int          cheats_default_equip(void);
void         cheats_request_catalog(void);                /* builds / refreshes the item list on the game thread (a scan of the loaded objects: only on request) */
int          cheats_catalog_state(void);                  /* CU_BUILD_* */
unsigned     cheats_catalog_serial(void);                 /* changes whenever the list changed */
Catalog     *cheats_catalog_lock(void);                   /* the list, locked: always pair with cheats_catalog_unlock */
void         cheats_catalog_unlock(void);
int          cheats_test_ui(char *query, int cap, int *select_first);   /* test only (plugin.cfg test_tab / test_query / test_select): 1 = open on the Add items tab */
void         cheats_request_add(const char *path, int count, int variant, int equip);
void         cheats_status(char *out, int cap, unsigned *serial);   /* the last add result / hint */
/* World tab (0.5.0): rows. Rows marked host only refuse on a joined client ("<row>: host only" in the status) */
enum { W_TPAIM, W_GOTO, W_BRING, W_BRINGALL, W_SAVESPOT, W_LOADSPOT, W_DOOR, W_ALTAR, W_SPAWN, W_AGGRO, W_KILL, W_CLEAR, W_SPEED, W_DEBUG, W_EMOTES, W_N };
#define WV_PLAYERS 8
typedef struct { int have_pawn, host, nplayers, is_me[WV_PLAYERS], npcs_near, spawned, have_spot; char player[WV_PLAYERS][48]; float pos[3], yaw; } WorldView;
void         cheats_world(int w);                         /* queue one World row (W_*) on the game thread */
const char  *cheats_world_label(int w);
int          cheats_world_host_only(int w);
int          cheats_npc_count(void);
const char  *cheats_npc_label(int i);
int          cheats_get_int(int which);                   /* 0 player # (1..8), 1 NPC type, 2 max NPCs near me */
void         cheats_set_int(int which, int v);
float        cheats_dilation(void);                       /* the Game speed value (applied by W_SPEED) */
void         cheats_set_dilation(float v);
void         cheats_world_view(WorldView *out);           /* snapshot, refreshed every 0.25 s */
/* implemented by cheatsui.cpp */
int          cui_start(const AbsApi *api);                /* registers the draw callback; 0 = ok, otherwise the window is not available */
void         cui_set_open(int open);                      /* the toggle key: the window stays up (and takes the mouse) outside the loader menus */
int          cui_is_open(void);
int          cheats_mp(int *nplayers);
int          cheats_mp_allowed(void);                   /* 1 = the setting keeps cheats on with other players (joiners are told) */
void         cheats_npcl_refresh(void);                 /* re-read the loader's NPC list on the game thread (tab opened) */
int          cheats_npcl_get(int i, char *group, int gcap, char *label, int lcap);   /* entry i of the NPC list; 0 = past the end */                  /* 1 = another player is in this game: every cheat is off (0.8.0) */
#ifdef __cplusplus
}
#endif
#endif

// overlay.h - the loader's Dear ImGui overlay (overlay.cpp): data the menus publish, commands the overlay hands back, state queries. Plain C types only.
// Threads: the game thread builds an OvSnap and calls ov_publish; the render thread (IDXGISwapChain::Present) draws the newest one and queues commands
// (a click on a row, a button) that the game thread takes with ov_poll. The render thread never touches engine objects.
// Nothing is hooked until ov_request() is called (the first time a menu is opened); a closed menu costs one flag test per Present.
#ifndef LANNATIVE_OVERLAY_H
#define LANNATIVE_OVERLAY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { OV_NONE = 0, OV_MENU, OV_MODS, OV_LINES };                       // what the snapshot shows: nothing / an F1 list page / the Mods page / plain text lines
enum { OV_OFF = 0, OV_STARTING, OV_READY, OV_DEAD };                    // not requested / hooked, first frame pending / drawing / switched off (reason in the log)
enum { OVC_NONE = 0, OVC_MENU_SELECT, OVC_MENU_BACK, OVC_TOGGLE, OVC_MODS_ACT,             // OVC_MENU_SELECT / OVC_MODS_ACT: index = row / action line
       OVC_MODS_ENABLE,        // Mods page: index = row in OvSnap.mods, arg 1 = switch on, 0 = off
       OVC_MODS_MOVE,          // index = row, arg -1 = earlier in the load order, +1 = later
       OVC_MODS_RESTART,       // quit the game cleanly so it can be started again
       OVC_MODS_FORM,          // index = row whose settings form opens; -1 closes the form
       OVC_MODS_APPLY,         // index = row, text = "key<TAB>value<LF>" for every setting of the form
       OVC_MENU_HOVER };       // the mouse moved onto row `index` of an F1 list page / the Mods page rows: it becomes the selected row (one highlight for mouse and keys)

#define OV_LINE 96
#define OV_ROWS 192                                                     // rows of a list page
#define OV_MODS_MAX 256
#define OV_WARN 16
#define OV_CCONF 24
#define OV_PLUG 16
#define OV_ACTIONS 80

typedef struct OvMod {
    char name[96], id[96], version[24], reason[180];
    int status;                       // at boot: 0 loaded, 1 switched off, 2 skipped (reason), 3 empty
    int paks_ok, paks_total, rows_ops, content_files, plugin, affects_gameplay;   // plugin: 0 none, 1 allowed, 2 blocked
    int enabled;                      // the box as it is now (loadorder.txt, possibly changed on this page)
    int effect;                       // 0 nothing pending, 1 switches on after a restart, 2 switches off after a restart, 3 switched on live
    int has_pak, has_lua, has_rows, has_content, has_plugin, hot_ok, nsettings, nconf;
    char conf[3][112];                // the first conflicts with other mods (tooltip); nconf counts all
} OvMod;
#define OV_SETTINGS 24
typedef struct OvSetting {
    char key[48], label[64], help[160], def[128], value[128];
    int type;                         // 0 bool, 1 int, 2 float, 3 choice, 4 text (modsettings.h)
    int has_min, has_max, nchoice; double min, max; char choice[16][48];
} OvSetting;

typedef struct OvSnap {
    int kind;                         // OV_*
    char title[OV_LINE];
    int sel;                          // selected row (keyboard cursor); -1 = none
    int has_back;                     // a "Back" button makes sense (not the main page)
    int count;                        // OV_MENU / OV_LINES: rows in label[]
    char label[OV_ROWS][OV_LINE];
    // OV_MODS: the same data the text page is built from
    int safe_mode; char safe_why[160];
    char order[400];
    int nmods; OvMod mods[OV_MODS_MAX];
    int pak_conflicts, pak_conflicts_real, row_conflicts, obj_conflicts, split_assets;
    int nwarn; char warn[OV_WARN][240];
    char info[240];                   // plain info line under "Load order:" (not a warning); "" = none
    int c_files, c_entries, c_invalid, ncconf; char cconf[OV_CCONF][160];
    int nplug; char plug_name[OV_PLUG][64]; int plug_faults[OV_PLUG], plug_loaded[OV_PLUG];
    char reload_status[240];
    int nactions; char action[OV_ACTIONS][OV_LINE]; int action_kind[OV_ACTIONS];   // kind: 0 reload, 1 plugin section header, 2 plugin item
    int pending_n; char pending_text[160];           // load order changes that wait for a restart
    int form_mod; unsigned form_serial;              // row whose settings form is open (-1 = none); the serial changes whenever the values below were reloaded
    char form_title[96], form_msg[200]; int form_ok, form_live, nform; OvSetting form[OV_SETTINGS];   // form_live: the mod's plugin takes changes without a restart
    // OV_MENU: type-to-search state of a list / category page; text shown instead of rows when the page has none
    int searchable; char filter[32]; char empty[96];
} OvSnap;

typedef struct OvCmd { int cmd, index, arg; char text[1536]; } OvCmd;

// Plugin side (AbsApi overlay_*). owner = the plugin's index (0..63). Draw callbacks run on the render thread inside a frame, between NewFrame and Render, after the
// loader's own windows. Modes are per owner and combined: frames run while any owner has a mode set (or a loader menu is open).
enum { OVM_DRAW = 1, OVM_MOUSE = 2, OVM_KEYS = 4, OVM_RAW = 8, OVM_BLOCK = 16, OVM_HIDECURSOR = 32 };   // same values as ABS_OV_DRAW / _MOUSE / _KEYS / _RAWMOUSE / _BLOCK_GAME / _HIDECURSOR
typedef struct OvInfo {
    int state;                                                           // 0 starting, 1 ready, -1 dead
    void *context; const char *version; int version_num;
    void *(*alloc)(size_t, void *); void (*free)(void *, void *); void *alloc_user;
    int width, height; float scale; void *device, *device_context;
    int sizeof_io, sizeof_style, sizeof_vert, sizeof_idx, sizeof_wchar;  // layout of the loader's ImGui build
    unsigned caller_mode; int caller_callbacks;                          // the asking plugin's current mode flags and registered callbacks
} OvInfo;

void ov_set_guard(int (*alive)(int owner), void (*fault)(int owner, unsigned long code), const char *(*name)(int owner));
int  ov_register(int owner, void (*draw)(void *user), void *user);     // 0 = ok, 1 = bad arguments / duplicate / table full (16 callbacks in total) / overlay not installed
int  ov_unregister(int owner, void (*draw)(void *user), void *user);   // 0 = removed, 1 = not found
int  ov_set_mode(int owner, unsigned flags);                            // 0 = ok, 1 = bad owner / overlay not installed
int  ov_raw_mouse(int *dx, int *dy);                                    // returns and clears the raw movement collected while a mode has OVM_RAW; 0 = ok, 1 = not installed
void ov_release_owner(int owner);                                       // drops the owner's modes and callbacks (3 faults, unload, owner no longer alive)
void ov_info(int owner, OvInfo *out);                                              // any thread: a snapshot; the context pointer is only to be bound inside a draw callback
void ov_set_log(void (*fn)(const char *fmt, ...));                      // log sink (the loader's logf_)
void ov_set_scale(float min_scale);                                     // lower bound of the UI scale (1 = 1080p size)
int  ov_request(void);                                                  // installs the hooks once (game thread, first menu open); returns ov_state()
int  ov_state(void);
int  ov_shows_menu(void);                                               // 1 = the overlay draws (or is about to): the loader's text menu stays hidden; 0 = draw the text menu
void ov_publish(const OvSnap *s);                                       // s == NULL or kind OV_NONE hides the overlay. Copies s.
int  ov_poll(OvCmd *c);                                                 // game thread: next queued command, 0 = none
// input focus and the cursor
int  ov_focus_owner(void);                                              // who has ImGui's focus (keyboard / pad): -2 nobody, -1 a loader window, >= 0 that plugin's window
void ov_pad_chord_guard(int on);                                        // 1: the game does not see Start while Back is held (the Back + Start menu chord); installs the gamepad hook
void ov_keep_key(int vk, int on);                                       // 1: key presses of vk (an F key a mod uses) are kept from the game's window procedure (the release too)
int  ov_cursor_mode(void);                                              // 0 none drawn, 1 the overlay's virtual cursor, 2 the game's own OS cursor is used (one cursor)

#ifdef __cplusplus
}
#endif
#endif

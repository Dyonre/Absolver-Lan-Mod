// Server mode (included by lannative.c after plugins.inc): an unattended host. The game has no dedicated server (a "Game" target ignores -server), but it runs
// without rendering (-nullrhi -nosound), and everything the title screen does can be done from code:
//   UThePlainesGameInstance::BPF_LoadProfile(controller)  then  BPF_RequestLoadSave(saveName, controller)   -> the game loads the character and travels to the main world
//   then the same travel as menu Host ("<map>?listen") opens the listen server. Joiners need the join key (joinguard.h) like on any host.
// -LNClient makes the same unattended start but joins the address in ip.txt instead (a test bot: lets one PC test a join with two headless instances).
// Turn on with  servermode = on  in LanNative\config.txt or -LNServer on the command line.  servermode_save = <character save name without .sav>  (required)
//   servermode_map = <map URL>  (default: the co-op main world, as menu Host).  servermode_delay = seconds to wait before each step (default 12).
// Start it with:  Absolver-Win64-Shipping.exe -NoEAC -nullrhi -nosound -NoSplash -LNServer     (see docs\HOSTING.md). Use a character that exists only for this: the game saves it.
// Reflection layer (find_function / call_function with a parameter block) is the plugin API's; parameter layouts:
//   BPF_LoadProfile(ObjectProperty _controller @0)   size 8;   BPF_RequestLoadSave(StrProperty _saveName @0, ObjectProperty _controller @16)   size 0x18
typedef struct { int on, join; char save[64]; wchar_t map[MENU_URL_MAX]; int delay_s; int stage; uint64_t due; int faults; uint64_t started; } ServerMode;
static ServerMode g_srv;
static int servermode_world_is(const char *needle) { char path[260]; void *w = world(); return w && api_object_path(w, path, (int)sizeof path) && strstr(path, needle) != NULL; }
static void servermode_install(void) {
    char v[128] = ""; wchar_t path[MAX_PATH];
    int cfg = setting_get("servermode", L"servermode.txt", 0, v, sizeof v, path) && !_strnicmp(v, "on", 2);
    g_srv.on = cfg || cmdline_has_flag(GetCommandLineW(), L"lnserver");
    if (cmdline_has_flag(GetCommandLineW(), L"lnclient")) g_srv.on = g_srv.join = 1;                       // test bot: same unattended start, then joins ip.txt like menu Join
    if (!g_srv.on) return;
    g_srv.delay_s = 12; wcscpy(g_srv.map, MAP_URL);
    if (setting_get("servermode_save", L"servermode_save.txt", 0, v, sizeof v, path)) snprintf(g_srv.save, sizeof g_srv.save, "%s", v);
    if (setting_get("servermode_delay", L"servermode_delay.txt", 0, v, sizeof v, path)) { int n = atoi(v); if (n >= 3 && n <= 300) g_srv.delay_s = n; }
    if (setting_get("servermode_map", L"servermode_map.txt", 0, v, sizeof v, path) && v[0] == '/') { int n = MultiByteToWideChar(CP_UTF8, 0, v, -1, g_srv.map, MENU_URL_MAX); if (n <= 0) wcscpy(g_srv.map, MAP_URL); }
    { const wchar_t *c = wcsstr(GetCommandLineW(), L"-LNSave="); if (c) { c += 8; char tmp[64]; int n = 0; while (*c && *c != L' ' && *c != L'"' && n < 63) tmp[n++] = *c < 128 ? (char)*c : '?', c++; tmp[n] = 0; if (n) snprintf(g_srv.save, sizeof g_srv.save, "%s", tmp); } }   // -LNSave=<name> overrides servermode_save for this process
    for (const char *c = g_srv.save; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) { g_srv.save[0] = 0; break; }
    if (!g_srv.save[0]) { logf_("server mode: ON but servermode_save is missing or has characters other than A-Z a-z 0-9 _ - : nothing will be loaded, only the world check runs"); }
    else if (g_srv.join) logf_("server mode: CLIENT bot - will load character '%s', then join the address in ip.txt (steps %d s apart)", g_srv.save, g_srv.delay_s);
    else logf_("server mode: ON - will load character '%s', then host %ls (steps %d s apart)", g_srv.save, g_srv.map, g_srv.delay_s);
}
static void *servermode_gi(void) {
    void *w = world(), *gi = w ? *(void **)((uint8_t *)w + OFF_W_GAMEINST) : NULL;
    return gi && object_is_a(gi, api_cls_gi()) ? gi : NULL;
}
static void servermode_call(const char *fn, uint8_t *params, const char *what) {
    void *gi = servermode_gi(); void *f = gi ? api_p_find_function(NULL, gi, fn) : NULL;
    if (!f) { logf_("server mode: %s: function %s not found - stopping", what, fn); g_srv.stage = 9; return; }
    int ok = api_p_call(NULL, gi, f, params);
    logf_("server mode: %s -> %s", what, ok ? "called" : "FAILED"); if (!ok) g_srv.stage = 9;
}
static void servermode_tick(void) {
    if (!g_srv.on || g_srv.stage >= 9 || g_srv.faults >= 2 || !g_game_tid) return;
    uint64_t now = GetTickCount64(); if (!g_srv.started) g_srv.started = now;
    if (now < g_srv.due) return;
    uint64_t step = (uint64_t)g_srv.delay_s * 1000;
    __try {
        void *w = world(); if (!w || !engine()) return;
        if (!plug_guard_ok()) { g_srv.stage = 9; return; }                                  // the engine functions used below are byte-checked
        if (g_srv.stage == 0) {                                                             // title screen: load the profile
            if (servermode_world_is("/Game/Maps/MainMap/MainMap_Main")) { g_srv.stage = 3; g_srv.due = now + step; logf_("server mode: already in the main world"); return; }
            if (!servermode_world_is("/Game/Maps/StartMap/")) { if (now - g_srv.started > 180000) { logf_("server mode: no start map after 3 minutes - stopping"); g_srv.stage = 9; } return; }
            void *pc = L_local_pc(); if (!pc || !servermode_gi()) return;
            if (!g_srv.save[0]) { g_srv.stage = 9; return; }
            uint8_t p[8] = { 0 }; *(void **)p = pc; servermode_call("BPF_LoadProfile", p, "load the profile");
            if (g_srv.stage == 0) { g_srv.stage = 1; g_srv.due = now + step; }
        } else if (g_srv.stage == 1) {                                                      // load the character: the game travels to the main world itself
            void *pc = L_local_pc(); if (!pc) return;
            static wchar_t name[80]; widen(g_srv.save, name, 80);
            uint8_t p[0x18] = { 0 }; struct { wchar_t *d; int n, m; } *s = (void *)p; s->d = name; s->n = s->m = (int)wcslen(name) + 1; *(void **)(p + 16) = pc;
            servermode_call("BPF_RequestLoadSave", p, "load the character");
            if (g_srv.stage == 1) { g_srv.stage = 2; g_srv.due = now + step; }
        } else if (g_srv.stage == 2) {                                                      // wait for the main world, then give it time to settle
            if (servermode_world_is("/Game/Maps/MainMap/MainMap_Main")) { g_srv.stage = 3; g_srv.due = now + step; logf_("server mode: the character is in the main world"); }
            else if (now - g_srv.started > 300000) { logf_("server mode: the main world did not load within 5 minutes - stopping"); g_srv.stage = 9; }
        } else if (g_srv.stage == 3) {                                                      // host (same travel as menu Host) or, with -LNClient, join ip.txt (same travel as menu Join)
            g_srv.stage = 4;
            if (g_srv.join) { wchar_t url[900]; if (read_join_url(url, 900)) { logf_("server mode: joining (same travel as F7)"); do_travel(url); } else logf_("server mode: no usable address in ip.txt"); }
            else { logf_("server mode: starting the listen server (%ls)", g_srv.map); jg_announce_host(); do_travel(g_srv.map); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_srv.faults++; logf_("server mode: EXCEPTION 0x%08lX at step %d (fault %d of 2)", GetExceptionCode(), g_srv.stage, g_srv.faults); }
}

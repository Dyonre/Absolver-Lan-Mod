// LuaHost - Lua 5.4 scripting for Absolver mods (Lua API: docs\sdk\LUA.md). A plugin of the script extender: it only runs when
// "LuaHost" is listed in Mods\plugins.txt (OFF by default, like every plugin). Lua source: third_party\lua (MIT, 5.4.9).
// Scripts: Mods\<mod>\scripts\*.lua of every ENABLED mod, in the loader's load order (loadorder.txt + mod.json requires / after, manifest.h), files by name.
// Each mod runs in its own environment (globals are per mod; the libraries and `abs` are shared). Sandbox, memory cap and instruction budget: luasandbox.h.
// Event bus: abs.on(name, fn) / abs.emit(name, ...). Engine events (registered with the loader on first use): tick, world, hit, hit_pre, attack, death, defense, weapon,
// style, menu; plus "ready" after all scripts loaded and "character" when the player's character appears. A handler that errors 3 times is removed (logged). Hot reload: script files are polled every second; any change
// restarts the Lua state and reloads every script (timers and handlers of the old state are dropped). REPL: write Lua into <LuaHost folder>\repl_in.txt; the host runs it
// within a second, logs the result and appends it to repl_out.txt. F1 > Mods page: "reload scripts", "status".
// plugin.cfg: budget = instructions per call (default 2000000), memory_mb = 64, reload = on, repl = on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../lannative/AbsolverSE.h"
#include "../lannative/manifest.h"
#include "luasandbox.h"
#include "keybinds.h"

static const AbsApi *A;
static LsBox g_box; static lua_State *L; static int g_gen;                 // g_gen: bumped at every (re)load; timers of an older state are dead
static long long g_budget = 2000000; static size_t g_mem = 64u << 20; static int g_reload = 1, g_repl = 1;
static char g_mods[MAX_PATH], g_repl_in[MAX_PATH], g_repl_out[MAX_PATH];
#define MAXS 256
typedef struct { char mod[MOD_NAME_MAX], path[MAX_PATH]; FILETIME mt; DWORD size; } Script;
static Script g_s[MAXS]; static int g_ns;
static int g_errors, g_loaded, g_reloads;

#include "luabus.h"
#include "luasave.h"

// ---- helpers
static void *ud_arg(lua_State *S, int i) { return lua_islightuserdata(S, i) ? lua_touserdata(S, i) : NULL; }
static void push_ptr(lua_State *S, void *p) { if (p) lua_pushlightuserdata(S, p); else lua_pushnil(S); }
static void *player(void) { void *pc = A->get_local_player_controller(A), *pawn = NULL, *f = pc ? A->find_function(A, pc, "K2_GetPawn") : NULL; if (f) A->call_function_params(A, pc, f, NULL, &pawn); return pawn; }
static void *char_arg(lua_State *S, int i) { return lua_isnoneornil(S, i) ? player() : ud_arg(S, i); }
static const char *outcome_name(int o) { static const char *n[] = { "hit", "guarded", "guard_broken", "absorbed", "parried", "dodged", "no_effect" }; return o >= 0 && o < 7 ? n[o] : "?"; }
static const char *defense_name(int t) { static const char *n[] = { "guard", "guard_break", "absorb", "parry", "deflect", "dodge", "avoid" }; return t >= 0 && t < 7 ? n[t] : "?"; }

// ---- abs.* (C)
static int l_log(lua_State *S) {                                            // abs._log(mod, ...)
    int n = lua_gettop(S); luaL_Buffer b; luaL_buffinit(S, &b);             // gettop first: buffinit may push a placeholder
    for (int i = 2; i <= n; i++) { if (i > 2) luaL_addchar(&b, ' '); luaL_tolstring(S, i, NULL); luaL_addvalue(&b); }
    luaL_pushresult(&b); A->log(A, "lua %s: %s", luaL_optstring(S, 1, "?"), lua_tostring(S, -1)); return 0;
}
static int l_error(lua_State *S) { g_errors++; A->log(A, "lua %s: ERROR in '%s' handler (%d of 3%s): %s", luaL_optstring(S, 1, "?"), luaL_optstring(S, 2, "?"), (int)luaL_optinteger(S, 4, 1), luaL_optinteger(S, 4, 1) >= 3 ? ", handler REMOVED" : "", luaL_optstring(S, 3, "")); return 0; }
static int l_player(lua_State *S) { push_ptr(S, player()); return 1; }
static int l_char_info(lua_State *S) {
    void *c = char_arg(S, 1); AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci;
    if (!c || !A->char_info(A, c, &ci)) { lua_pushnil(S); return 1; }
    lua_newtable(S);
    lua_pushstring(S, ci.name); lua_setfield(S, -2, "name"); lua_pushnumber(S, ci.health); lua_setfield(S, -2, "health"); lua_pushnumber(S, ci.max_health); lua_setfield(S, -2, "max_health");
    lua_pushnumber(S, ci.guard); lua_setfield(S, -2, "guard"); lua_pushinteger(S, ci.stance); lua_setfield(S, -2, "stance"); lua_pushinteger(S, ci.style); lua_setfield(S, -2, "style");
    lua_pushboolean(S, ci.is_player); lua_setfield(S, -2, "is_player"); lua_pushboolean(S, ci.is_dead); lua_setfield(S, -2, "is_dead"); return 1;
}
static int l_find(lua_State *S) { push_ptr(S, A->find_object(A, luaL_checkstring(S, 1))); return 1; }
static int l_get(lua_State *S) { void *o = ud_arg(S, 1); char out[2048]; int n = o ? A->get_property_text(A, o, luaL_checkstring(S, 2), out, sizeof out) : -1; if (n < 0) lua_pushnil(S); else lua_pushstring(S, out); return 1; }
static int l_set(lua_State *S) { void *o = ud_arg(S, 1); lua_pushboolean(S, o && A->set_property_text(A, o, luaL_checkstring(S, 2), luaL_checkstring(S, 3)) == 1); return 1; }
static int l_row_set(lua_State *S) { lua_pushboolean(S, A->row_set_text(A, luaL_checkstring(S, 1), luaL_checkstring(S, 2), luaL_checkstring(S, 3), luaL_checkstring(S, 4)) == 1); return 1; }
static int l_row_float(lua_State *S) { uint8_t *r = A->table_row(A, luaL_checkstring(S, 1), luaL_checkstring(S, 2)); lua_Integer off = luaL_checkinteger(S, 3); if (!r || off < 0 || off > 0x4000) lua_pushnil(S); else { float f; memcpy(&f, r + off, 4); lua_pushnumber(S, f); } return 1; }
static int l_damage_mod(lua_State *S) { void *c = char_arg(S, 1); lua_pushboolean(S, c && A->damage_mod(A, c, luaL_checkstring(S, 2), (float)luaL_checknumber(S, 3)) == 1); return 1; }
static int l_console(lua_State *S) { lua_pushboolean(S, A->console_cmd(A, luaL_checkstring(S, 1)) == 1); return 1; }
static int l_message(lua_State *S) { lua_pushboolean(S, A->show_message(A, luaL_checkstring(S, 1), (float)luaL_optnumber(S, 2, 4)) == 1); return 1; }
static int l_banner(lua_State *S) { lua_pushboolean(S, A->show_banner(A, luaL_checkstring(S, 1), (float)luaL_optnumber(S, 2, 3)) == 1); return 1; }
static int l_clock(lua_State *S) { lua_pushnumber(S, (lua_Number)GetTickCount64() / 1000.0); return 1; }
static int l_kv_get(lua_State *S) { char out[1024]; if (A->kv_get(A, luaL_checkstring(S, 1), out, sizeof out)) lua_pushstring(S, out); else lua_pushnil(S); return 1; }
static int l_kv_set(lua_State *S) { const char *v = lua_isnoneornil(S, 2) ? NULL : luaL_checkstring(S, 2); if (v && (strchr(v, '\n') || strchr(v, '\r'))) return luaL_error(S, "kv_set: no line breaks in values"); lua_pushboolean(S, A->kv_set(A, luaL_checkstring(S, 1), v) == 0); return 1; }

// ---- timers: slot table; a slot belongs to one generation (state); a timer of an older state cancels itself when it fires
#define MAXT 128
typedef struct { int used, id, ref, gen, repeat, errors; char mod[MOD_NAME_MAX]; } LTimer;
static LTimer g_t[MAXT];
static void timer_free(int i) { if (g_t[i].used && L && g_t[i].gen == g_gen) luaL_unref(L, LUA_REGISTRYINDEX, g_t[i].ref); g_t[i].used = 0; }
static void timer_cb(void *u) {
    int i = (int)(intptr_t)u; if (i < 0 || i >= MAXT || !g_t[i].used) return;
    if (g_t[i].gen != g_gen || !L) { A->cancel_timer(A, g_t[i].id); g_t[i].used = 0; return; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_t[i].ref);
    if (ls_call(L, 0, 0, g_budget)) { g_errors++; g_t[i].errors++; A->log(A, "lua %s: ERROR in timer (%d of 3%s): %s", g_t[i].mod, g_t[i].errors, g_t[i].repeat && g_t[i].errors >= 3 ? ", repeating timer CANCELLED" : "", lua_tostring(L, -1)); lua_pop(L, 1);
        if (g_t[i].repeat && g_t[i].errors >= 3) { A->cancel_timer(A, g_t[i].id); timer_free(i); return; } }
    if (!g_t[i].repeat && g_t[i].used) timer_free(i);
}
static int l_timer(lua_State *S) {                                          // abs._timer(mod, seconds, repeat, fn) -> id
    const char *mod = luaL_checkstring(S, 1); float sec = (float)luaL_checknumber(S, 2); int rep = lua_toboolean(S, 3); luaL_checktype(S, 4, LUA_TFUNCTION);
    if (sec < 0.05f) sec = 0.05f;
    int i = 0; while (i < MAXT && g_t[i].used) i++; if (i == MAXT) return luaL_error(S, "too many timers (%d)", MAXT);
    lua_pushvalue(S, 4); g_t[i].ref = luaL_ref(S, LUA_REGISTRYINDEX); g_t[i].gen = g_gen; g_t[i].repeat = rep; g_t[i].errors = 0; g_t[i].used = 1; snprintf(g_t[i].mod, sizeof g_t[i].mod, "%s", mod);
    g_t[i].id = A->set_timer_id(A, sec, rep, timer_cb, (void *)(intptr_t)i);
    if (g_t[i].id <= 0) { timer_free(i); return luaL_error(S, "timer not created (loader timer table full?)"); }
    lua_pushinteger(S, i + 1); return 1;
}
static int l_cancel(lua_State *S) { int i = (int)luaL_checkinteger(S, 1) - 1; int ok = i >= 0 && i < MAXT && g_t[i].used && g_t[i].gen == g_gen; if (ok) { A->cancel_timer(A, g_t[i].id); timer_free(i); } lua_pushboolean(S, ok); return 1; }

// ---- keybinds (keybinds.h): abs.bind(action, key, fn[, label]); user remap file <LuaHost folder>\keybinds.txt; report keybinds_report.txt.
// The loader's on_key cannot be undone, so each distinct key is registered once per process and dispatched to the CURRENT state's bindings.
static KbTable g_kb; static int g_kb_ref[KB_MAX], g_kb_gen[KB_MAX]; static unsigned char g_kb_hooked[256]; static char g_kb_file[MAX_PATH], g_kb_rep[MAX_PATH];
static void key_cb(int vk, void *u) {
    (void)u; if (!L) return;
    for (int i = 0; i < g_kb.n; i++) if (g_kb.b[i].vk == vk && g_kb_gen[i] == g_gen) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_kb_ref[i]);
        if (ls_call(L, 0, 0, g_budget)) { g_errors++; A->log(A, "lua %s: ERROR in key %s.%s: %s", g_kb.b[i].owner, g_kb.b[i].owner, g_kb.b[i].action, lua_tostring(L, -1)); lua_pop(L, 1); }
    }
}
static char *slurp(const char *p, DWORD *sz);
static void kb_refresh(int report) {                                        // remap file -> effective keys, register new keys with the loader, optional report
    char *txt = slurp(g_kb_file, NULL), why[200]; int bad = kb_apply_remap(&g_kb, txt ? txt : "", why, sizeof why); free(txt);
    for (int i = 0; i < g_kb.n; i++) { int vk = g_kb.b[i].vk; if (vk > 0 && vk < 256 && !g_kb_hooked[vk]) { int rc = A->on_key(A, vk, key_cb, NULL); g_kb_hooked[vk] = 1; if (rc) A->log(A, "luahost: on_key for 0x%02X FAILED (the loader has 32 key slots for all plugins)", vk); } }
    if (!report) return;
    static char rep[8192]; size_t w = 0; char k[16];
    w += (size_t)snprintf(rep + w, sizeof rep - w, "LuaHost keybinds (%d). Change one in keybinds.txt next to this file:  <mod>.<action> = <key> | off\r\n", g_kb.n);
    for (int i = 0; i < g_kb.n && w < sizeof rep - 200; i++) { kb_key_name(g_kb.b[i].vk, k, sizeof k); w += (size_t)snprintf(rep + w, sizeof rep - w, "  %s.%s = %s%s   (%s)\r\n", g_kb.b[i].owner, g_kb.b[i].action, k, g_kb.b[i].remapped ? "  [remapped]" : "", g_kb.b[i].label); }
    char con[4096]; int nc = kb_conflicts(&g_kb, con, sizeof con);
    w += (size_t)snprintf(rep + w, sizeof rep - w, "Conflicts: %d\r\n%s", nc, con);
    if (bad) w += (size_t)snprintf(rep + w, sizeof rep - w, "keybinds.txt: %d line(s) not applied, first: %s\r\n", bad, why);
    FILE *f = fopen(g_kb_rep, "wb"); if (f) { fwrite(rep, 1, w, f); fclose(f); }
    A->log(A, "luahost: %d keybind(s), %d conflict(s)%s%s", g_kb.n, nc, bad ? ", keybinds.txt problem: " : "", bad ? why : "");
    for (char *s = con, *e; *s; s = e) { e = strchr(s, '\r'); if (!e) break; *e = 0; A->log(A, "luahost: key conflict %s", s); e += 2; }
}
static int l_bind(lua_State *S) {                                           // abs._bind(mod, action, key, fn, label) -> effective key name
    const char *mod = luaL_checkstring(S, 1), *action = luaL_checkstring(S, 2), *key = luaL_checkstring(S, 3); luaL_checktype(S, 4, LUA_TFUNCTION);
    int vk = kb_parse_key(key); if (!vk) return luaL_error(S, "abs.bind: unknown key '%s' (F1..F24, A..Z, 0..9, NUMPAD0..9, 0x..)", key);
    int i = kb_add(&g_kb, mod, action, luaL_optstring(S, 5, action), vk); if (i < 0) return luaL_error(S, "abs.bind: action '%s' already bound, bad id or table full", action);
    lua_pushvalue(S, 4); g_kb_ref[i] = luaL_ref(S, LUA_REGISTRYINDEX); g_kb_gen[i] = g_gen;
    kb_refresh(0); char k[16]; kb_key_name(g_kb.b[i].vk, k, sizeof k); lua_pushstring(S, k); return 1;
}

// ---- engine events -> bus (registered once per process on first use; they dispatch into the CURRENT state)
static int dispatch_begin(const char *name) {                            // pushes __dispatch + name; 0 if nobody listens
    if (!L) return 0; lua_getglobal(L, "__count"); lua_pushstring(L, name);
    if (ls_call(L, 1, 1, 10000)) { lua_pop(L, 1); return 0; } int n = (int)lua_tointeger(L, -1); lua_pop(L, 1); if (!n) return 0;
    lua_getglobal(L, "__dispatch"); lua_pushstring(L, name); return 1;
}
// a failure here is the budget (handler errors are caught per handler inside __dispatch): __blame charges it to the handler that was running
static void dispatch_end(const char *name, int nargs) {
    if (!ls_call(L, nargs + 1, 0, g_budget)) return;
    g_errors++; lua_getglobal(L, "__blame"); lua_insert(L, -2);
    if (ls_call(L, 1, 0, 100000)) { A->log(A, "luahost: dispatch '%s' failed and could not be charged to a handler: %s", name, lua_tostring(L, -1)); lua_pop(L, 1); }
}
static void ev_tick(float dt, void *u) { (void)u; if (dispatch_begin("tick")) { lua_pushnumber(L, dt); dispatch_end("tick", 1); } }
static void ev_world(const char *map, void *u) { (void)u; if (dispatch_begin("world")) { lua_pushstring(L, map ? map : ""); dispatch_end("world", 1); } }
static void ev_hit(const AbsHitInfo *h, void *u) {
    (void)u; if (!dispatch_begin("hit")) return;
    lua_newtable(L); push_ptr(L, h->victim); lua_setfield(L, -2, "victim"); push_ptr(L, h->attacker); lua_setfield(L, -2, "attacker"); lua_pushstring(L, h->attack); lua_setfield(L, -2, "attack");
    lua_pushinteger(L, h->attack_id); lua_setfield(L, -2, "attack_id"); lua_pushnumber(L, h->damage); lua_setfield(L, -2, "damage"); lua_pushstring(L, outcome_name(h->outcome)); lua_setfield(L, -2, "outcome");
    lua_pushboolean(L, h->perfect_link); lua_setfield(L, -2, "perfect_link"); dispatch_end("hit", 1);
}
static float tfield(int t, const char *k, float def) { lua_getfield(L, t, k); float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def; lua_pop(L, 1); return v; }
static void ev_hit_pre(AbsHitPre *h, void *u) {
    (void)u; if (!dispatch_begin("hit_pre")) return;
    lua_newtable(L); int t = lua_gettop(L); lua_pushvalue(L, t); lua_insert(L, t - 2);                   // keep a copy below __dispatch to read the scales back
    push_ptr(L, h->victim); lua_setfield(L, -2, "victim"); push_ptr(L, h->attacker); lua_setfield(L, -2, "attacker"); lua_pushstring(L, h->attack); lua_setfield(L, -2, "attack");
    lua_pushinteger(L, h->attack_id); lua_setfield(L, -2, "attack_id"); lua_pushboolean(L, h->perfect_link); lua_setfield(L, -2, "perfect_link");
    lua_pushnumber(L, h->damage_scale); lua_setfield(L, -2, "damage_scale"); lua_pushnumber(L, h->stun_scale); lua_setfield(L, -2, "stun_scale");
    lua_pushnumber(L, h->knockback_scale); lua_setfield(L, -2, "knockback_scale"); lua_pushnumber(L, h->special_ratio_scale); lua_setfield(L, -2, "special_ratio_scale");
    lua_pushnumber(L, h->special_ratio); lua_setfield(L, -2, "special_ratio");
    dispatch_end("hit_pre", 1);
    int c = lua_gettop(L);                                                  // the copy
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
    h->damage_scale = CLAMP(tfield(c, "damage_scale", h->damage_scale), 0.f, 100.f); h->stun_scale = CLAMP(tfield(c, "stun_scale", h->stun_scale), 0.f, 100.f);
    h->knockback_scale = CLAMP(tfield(c, "knockback_scale", h->knockback_scale), 0.f, 100.f); h->special_ratio_scale = CLAMP(tfield(c, "special_ratio_scale", h->special_ratio_scale), 0.f, 100.f);
    { float sr = tfield(c, "special_ratio", h->special_ratio); h->special_ratio = sr < 0 ? -1.f : CLAMP(sr, 0.f, 1.f); }
    lua_pop(L, 1);
}
static void ev_attack(void *ch, int id, const char *atk, void *u) { (void)u; if (dispatch_begin("attack")) { push_ptr(L, ch); lua_pushinteger(L, id); lua_pushstring(L, atk ? atk : ""); dispatch_end("attack", 3); } }
static void ev_death(void *v, void *k, int id, const char *atk, void *u) { (void)u; if (dispatch_begin("death")) { push_ptr(L, v); push_ptr(L, k); lua_pushinteger(L, id); lua_pushstring(L, atk ? atk : ""); dispatch_end("death", 4); } }
static void ev_defense(const AbsDefenseInfo *d, void *u) {
    (void)u; if (!dispatch_begin("defense")) return;
    lua_newtable(L); lua_pushstring(L, defense_name(d->type)); lua_setfield(L, -2, "type"); push_ptr(L, d->defender); lua_setfield(L, -2, "defender"); push_ptr(L, d->attacker); lua_setfield(L, -2, "attacker");
    lua_pushstring(L, d->attack); lua_setfield(L, -2, "attack"); lua_pushnumber(L, d->frame_in_attack); lua_setfield(L, -2, "frame_in_attack"); lua_pushinteger(L, d->startup); lua_setfield(L, -2, "startup");
    dispatch_end("defense", 1);
}
static void ev_weapon(void *ch, void *o, void *n, int h, void *u) { (void)h; (void)u; if (dispatch_begin("weapon")) { push_ptr(L, ch); push_ptr(L, o); push_ptr(L, n); dispatch_end("weapon", 3); } }
static void ev_style(void *ch, int o, int n, void *u) { (void)u; if (dispatch_begin("style")) { push_ptr(L, ch); lua_pushinteger(L, o); lua_pushinteger(L, n); dispatch_end("style", 3); } }
static void ev_menu(void *m, const char *cls, const char *st, void *u) { (void)m; (void)u; if (dispatch_begin("menu")) { lua_pushstring(L, cls ? cls : ""); lua_pushstring(L, st ? st : ""); dispatch_end("menu", 2); } }
static int l_want(lua_State *S) {                                           // abs._want(name): register the engine event the first time any script listens
    const char *n = luaL_checkstring(S, 1); static unsigned done; int bit = -1, rc = 0;
    static const char *const names[] = { "tick", "world", "hit", "hit_pre", "attack", "death", "defense", "weapon", "style", "menu", NULL };
    for (int i = 0; names[i]; i++) if (!strcmp(n, names[i])) bit = i;
    if (bit < 0 || (done & (1u << bit))) return 0;                          // a script event (abs.emit) or already registered
    switch (bit) {
    case 0: rc = A->on_tick(A, ev_tick, NULL); break;            case 1: rc = A->on_world_loaded(A, ev_world, NULL); break;
    case 2: rc = A->on_hit(A, ev_hit, NULL); break;              case 3: rc = A->on_hit_pre(A, ev_hit_pre, NULL); break;
    case 4: rc = A->on_attack_start(A, ev_attack, NULL); break;  case 5: rc = A->on_death(A, ev_death, NULL); break;
    case 6: rc = A->on_defense(A, ev_defense, NULL); break;      case 7: rc = A->on_weapon_changed(A, ev_weapon, NULL); break;
    case 8: rc = A->on_style_changed(A, ev_style, NULL); break;  case 9: rc = A->on_menu(A, ev_menu, NULL); break;
    }
    done |= 1u << bit; A->log(A, "luahost: engine event '%s' registered (%s)", n, rc == 0 ? "ok" : "FAILED"); return 0;
}

// ---- scripts: discovery in the loader's order, (re)load
static char *slurp(const char *p, DWORD *sz) { HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL); if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), r = 0; char *t = n < (4u << 20) ? malloc(n + 1) : NULL; if (t && (!ReadFile(h, t, n, &r, NULL) || r != n)) { free(t); t = NULL; } CloseHandle(h); if (t) t[n] = 0; if (sz) *sz = n; return t; }
static int cmpname(const void *a, const void *b) { return _stricmp(((const Script *)a)->path, ((const Script *)b)->path); }
static int scan(Script *out, int cap) {
    static ModList list; char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\loadorder.txt", g_mods); char *lo = slurp(p, NULL); modlist_parse(&list, lo ? lo : ""); free(lo);
    static Manifest mfs[MODS_MAX]; int en[MODS_MAX]; static MfResult res;
    for (int m = 0; m < list.count; m++) { snprintf(p, sizeof p, "%s\\%s\\mod.json", g_mods, list.mods[m].name); char *t = slurp(p, NULL), err[200]; manifest_parse(t, list.mods[m].name, &mfs[m], err, sizeof err); free(t); en[m] = list.mods[m].enabled; }
    mf_resolve(mfs, en, list.count, ABS_API_VERSION, &res);
    int n = 0;
    for (int k = 0; k < res.norder; k++) {
        const char *mod = list.mods[res.order[k]].name; int first = n;
        snprintf(p, sizeof p, "%s\\%s\\scripts\\*.lua", g_mods, mod); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(p, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < cap) { snprintf(out[n].mod, sizeof out[n].mod, "%s", mod); snprintf(out[n].path, sizeof out[n].path, "%s\\%s\\scripts\\%s", g_mods, mod, fd.cFileName); out[n].mt = fd.ftLastWriteTime; out[n].size = fd.nFileSizeLow; n++; } while (FindNextFileA(h, &fd));
        FindClose(h); qsort(out + first, (size_t)(n - first), sizeof *out, cmpname);
    }
    return n;
}
// ---- save sections (luasave.h): stored in the character's sidecar (never in the game's save), key lua.<mod>.save
static int l_save_get(lua_State *S) { static char buf[65536]; char key[128]; snprintf(key, sizeof key, "lua.%s.save", luaL_checkstring(S, 1)); if (A->sidecar_get(A, NULL, key, buf, sizeof buf) == 1) lua_pushstring(S, buf); else lua_pushnil(S); return 1; }
static int l_save_set(lua_State *S) { char key[128]; snprintf(key, sizeof key, "lua.%s.save", luaL_checkstring(S, 1)); const char *v = luaL_checkstring(S, 2); if (strchr(v, '\n') || strchr(v, '\r')) return luaL_error(S, "save text has a line break"); lua_pushboolean(S, A->sidecar_set(A, NULL, key, v) == 1); return 1; }
// a data-only chunk: "return <text>" with an EMPTY environment (no functions reachable), inside the caller's budget
static int l_parse(lua_State *S) {
    size_t n; const char *s = luaL_checklstring(S, 1, &n); luaL_Buffer b; luaL_buffinit(S, &b); luaL_addstring(&b, "return "); luaL_addlstring(&b, s, n); luaL_pushresult(&b);
    size_t cl; const char *code = lua_tolstring(S, -1, &cl);
    if (ls_load(S, code, cl, "=save")) { lua_pushnil(S); return 1; }
    lua_newtable(S); lua_setupvalue(S, -2, 1);
    if (lua_pcall(S, 0, 1, 0)) { lua_pushnil(S); return 1; } return 1;
}
static void commit_all(const char *why) { if (!L) return; lua_getglobal(L, "__commit_all"); if (ls_call(L, 0, 0, g_budget)) { A->log(A, "luahost: save commit (%s) failed: %s", why, lua_tostring(L, -1)); lua_pop(L, 1); } }
static void ev_save_written(void *save, void *u) { (void)save; (void)u; commit_all("game save"); }
static void *g_char_seen;                                                  // the "character" event: the local player's character appeared (its sidecar is readable)
static int g_save_seen, g_char_pending;                                     // the title screen has a pawn but no save (its sidecar is not writable); a load can show several pawns
static void ev_save_loading(void *save, void *u) { (void)save; (void)u; g_save_seen = 1; g_char_pending = 1; }
// "character" fires ONCE per loaded character save, when the player's pawn exists (and again after a script reload while one is loaded)
static void char_check(int force) { void *p = g_save_seen ? player() : NULL; if (p && (g_char_pending || force)) { g_char_pending = 0; g_char_seen = p; if (dispatch_begin("character")) { push_ptr(L, p); dispatch_end("character", 1); } } }

static void close_state(void) { if (!L) return; commit_all("reload / exit"); for (int i = 0; i < MAXT; i++) if (g_t[i].used) { A->cancel_timer(A, g_t[i].id); g_t[i].used = 0; } lua_close(L); L = NULL; }
static void reg(const char *name, lua_CFunction f) { lua_pushcfunction(L, f); lua_setfield(L, -2, name); }
static void load_all(const char *why) {
    close_state(); g_gen++; g_loaded = 0;
    memset(&g_kb, 0, sizeof g_kb);
    L = ls_new(&g_box, g_mem); if (!L) { A->log(A, "luahost: could not create the Lua state (%s)", why); return; }
    lua_newtable(L);
    reg("_log", l_log); reg("_error", l_error); reg("_want", l_want); reg("_timer", l_timer); reg("_kv_get", l_kv_get); reg("_kv_set", l_kv_set); reg("_bind", l_bind); reg("_save_get", l_save_get); reg("_save_set", l_save_set); reg("_parse", l_parse);
    reg("cancel", l_cancel); reg("player", l_player); reg("char_info", l_char_info); reg("find", l_find); reg("get", l_get); reg("set", l_set); reg("row_set", l_row_set); reg("row_float", l_row_float);
    reg("damage_mod", l_damage_mod); reg("console", l_console); reg("message", l_message); reg("banner", l_banner); reg("clock", l_clock);
    lua_pushstring(L, LUA_RELEASE); lua_setfield(L, -2, "lua_version"); lua_pushinteger(L, g_gen); lua_setfield(L, -2, "generation");
    lua_setglobal(L, "abs");
    if (ls_load(L, PRELUDE, sizeof PRELUDE - 1, "=prelude") || ls_call(L, 0, 0, 0)) { A->log(A, "luahost: prelude FAILED: %s", lua_tostring(L, -1)); close_state(); return; }
    if (ls_load(L, PRELUDE_SAVE, sizeof PRELUDE_SAVE - 1, "=prelude_save") || ls_call(L, 0, 0, 0)) { A->log(A, "luahost: save prelude FAILED: %s", lua_tostring(L, -1)); close_state(); return; }
    g_ns = scan(g_s, MAXS);
    for (int i = 0; i < g_ns; i++) {
        DWORD sz; char *t = slurp(g_s[i].path, &sz); const char *fn = strrchr(g_s[i].path, '\\'); fn = fn ? fn + 1 : g_s[i].path; char chunk[160]; snprintf(chunk, sizeof chunk, "@%s/scripts/%s", g_s[i].mod, fn);
        if (!t) { A->log(A, "luahost: %s/scripts/%s: cannot read", g_s[i].mod, fn); continue; }
        int bad = ls_load(L, t, sz, chunk); free(t);
        if (!bad) { lua_getglobal(L, "__make_env"); lua_pushstring(L, g_s[i].mod); if (ls_call(L, 1, 1, 0) == 0) { lua_setupvalue(L, -2, 1); bad = ls_call(L, 0, 0, g_budget * 5); } else bad = 1; }
        if (bad) { g_errors++; A->log(A, "luahost: %s/scripts/%s FAILED: %s", g_s[i].mod, fn, lua_tostring(L, -1)); lua_pop(L, 1); }
        else { g_loaded++; A->log(A, "luahost: %s/scripts/%s loaded", g_s[i].mod, fn); }
        lua_settop(L, 0);
    }
    A->log(A, "luahost: %s: %d of %d script(s) loaded, %s, memory %zu KB of %zu MB, budget %lld instructions per call (generation %d)", why, g_loaded, g_ns, LUA_RELEASE, g_box.used / 1024, g_mem >> 20, g_budget, g_gen);
    kb_refresh(1);
    if (dispatch_begin("ready")) dispatch_end("ready", 0);
    char_check(1);                                                          // a reload while a character is loaded: "character" again
}

// ---- hot reload + REPL (1 s poll on the game thread)
static void repl_run(void) {
    DWORD sz; char *t = slurp(g_repl_in, &sz); if (!t) return; DeleteFileA(g_repl_in);
    while (sz && (t[sz - 1] == '\n' || t[sz - 1] == '\r' || t[sz - 1] == ' ')) t[--sz] = 0;            // one log line
    char out[4096]; size_t w = 0; out[0] = 0;
    if (!L) snprintf(out, sizeof out, "no Lua state");
    else {
        size_t cl = strlen(t) + 8; char *ret = malloc(cl); snprintf(ret, cl, "return %s", t);
        int top = lua_gettop(L), rc = ls_load(L, ret, strlen(ret), "=repl"); free(ret);
        if (rc) { lua_pop(L, 1); rc = ls_load(L, t, sz, "=repl"); }
        if (!rc) { lua_getglobal(L, "__make_env"); lua_pushstring(L, "repl"); if (ls_call(L, 1, 1, 0) == 0) { lua_setupvalue(L, -2, 1); rc = ls_call(L, 0, LUA_MULTRET, g_budget); } else rc = 1; }
        if (rc) w += (size_t)snprintf(out + w, sizeof out - w, "error: %s", lua_tostring(L, -1));
        else for (int i = top + 1; i <= lua_gettop(L) && w < sizeof out - 1; i++) { w += (size_t)snprintf(out + w, sizeof out - w, "%s%s", i > top + 1 ? "\t" : "", luaL_tolstring(L, i, NULL)); lua_pop(L, 1); }
        if (!w && !rc) snprintf(out, sizeof out, "(no value)");
        lua_settop(L, top);
    }
    A->log(A, "luahost: repl> %.200s => %.400s", t, out);
    FILE *f = fopen(g_repl_out, "ab"); if (f) { fprintf(f, "> %s\r\n%s\r\n", t, out); fclose(f); }
    free(t);
}
static void poll_cb(void *u) {
    (void)u;
    if (g_reload) {
        static Script now[MAXS]; int n = scan(now, MAXS), changed = n != g_ns;
        for (int i = 0; !changed && i < n; i++) changed = strcmp(now[i].path, g_s[i].path) || CompareFileTime(&now[i].mt, &g_s[i].mt) || now[i].size != g_s[i].size;
        { static FILETIME kbt; WIN32_FILE_ATTRIBUTE_DATA fa; FILETIME now_t = {0}; if (GetFileAttributesExA(g_kb_file, GetFileExInfoStandard, &fa)) now_t = fa.ftLastWriteTime;
          if (CompareFileTime(&now_t, &kbt)) { kbt = now_t; if (!changed && L) { kb_refresh(1); A->log(A, "luahost: keybinds.txt read"); } } }
        if (changed) { g_reloads++; load_all("scripts changed: reload"); }
    }
    char_check(0);
    if (g_repl) repl_run();
}
static void menu_reload(void *u) { (void)u; g_reloads++; load_all("reload from the Mods page"); }
static void menu_status(void *u) { (void)u; char m[200]; snprintf(m, sizeof m, "LuaHost: %d of %d scripts, %d errors, %d reloads, %zu KB", g_loaded, g_ns, g_errors, g_reloads, g_box.used / 1024); A->show_message(A, m, 5); A->log(A, "luahost: status: %s", m); }
static void engine_ready(void *u) { (void)u; A->on_save_written(A, ev_save_written, NULL); A->on_save_loading(A, ev_save_loading, NULL); load_all("start"); A->set_timer_id(A, 1.0f, 1, poll_cb, NULL); }

__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    A = api; if (api->version != ABS_API_VERSION || api->size < offsetof(AbsApi, on_defense) + sizeof(void *)) { api->log(api, "luahost: needs loader API 2.20 or newer - off"); return 1; }
    char v[64];
    if (A->config_get(A, "budget", v, sizeof v)) { long long b = _atoi64(v); if (b >= 10000) g_budget = b; }
    if (A->config_get(A, "memory_mb", v, sizeof v)) { int m = atoi(v); if (m >= 4 && m <= 1024) g_mem = (size_t)m << 20; }
    if (A->config_get(A, "reload", v, sizeof v)) g_reload = _strnicmp(v, "off", 3) != 0;
    if (A->config_get(A, "repl", v, sizeof v)) g_repl = _strnicmp(v, "off", 3) != 0;
    snprintf(g_mods, sizeof g_mods, "%s", api->plugin_dir); { char *s = strrchr(g_mods, '\\'); if (s) *s = 0; }
    snprintf(g_kb_file, sizeof g_kb_file, "%s\\keybinds.txt", api->plugin_dir); snprintf(g_kb_rep, sizeof g_kb_rep, "%s\\keybinds_report.txt", api->plugin_dir);
    snprintf(g_repl_in, sizeof g_repl_in, "%s\\repl_in.txt", api->plugin_dir); snprintf(g_repl_out, sizeof g_repl_out, "%s\\repl_out.txt", api->plugin_dir);
    A->add_menu_item(A, "reload scripts", menu_reload, NULL); A->add_menu_item(A, "status", menu_status, NULL);
    A->log(A, "luahost: %s host loaded (mods %s; budget %lld, memory %zu MB, reload %s, repl %s)", LUA_RELEASE, g_mods, g_budget, g_mem >> 20, g_reload ? "on" : "off", g_repl ? "on" : "off");
    return A->on_engine_ready(A, engine_ready, NULL);
}
__declspec(dllexport) void AbsPlugin_Unload(void) { close_state(); }

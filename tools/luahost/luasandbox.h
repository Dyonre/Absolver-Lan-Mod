// luasandbox.h - the Lua state of LuaHost: safe library set, memory cap, instruction budget per call, text-only chunks.
// No game access (pure code). Lua 5.4 source: third_party\lua (MIT).
//   Libraries: base (without dofile / loadfile / load), table, string (without string.dump), math, utf8, coroutine. NOT opened: io, os, package (require), debug.
//   Memory: an allocator that refuses to grow past the cap (Lua then raises "not enough memory" inside the script, the host keeps running).
//   Budget: a count hook every LS_HOOK_STEP instructions; a call (ls_call) that runs more than its budget fails with "instruction budget exceeded".
#ifndef LUAHOST_SANDBOX_H
#define LUAHOST_SANDBOX_H
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define LS_HOOK_STEP 1000
typedef struct { size_t used, limit, peak; long long count, budget; int over_mem; } LsBox;

static void *ls_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    LsBox *b = (LsBox *)ud; size_t old = ptr ? osize : 0;
    if (nsize == 0) { free(ptr); b->used -= old; return NULL; }
    if (nsize > old && b->used - old + nsize > b->limit) { b->over_mem++; return NULL; }
    void *p = realloc(ptr, nsize); if (!p) return NULL;
    b->used = b->used - old + nsize; if (b->used > b->peak) b->peak = b->used; return p;
}
static LsBox *ls_box(lua_State *L) { return *(LsBox **)lua_getextraspace(L); }
static void ls_hook(lua_State *L, lua_Debug *ar) {
    (void)ar; LsBox *b = ls_box(L); b->count += LS_HOOK_STEP;
    if (b->budget > 0 && b->count > b->budget) {                       // over: raise at EVERY instruction from now on, so a script's own pcall cannot swallow it for long
        lua_sethook(L, ls_hook, LUA_MASKCOUNT, 1); luaL_error(L, "instruction budget exceeded (%I instructions)", (lua_Integer)b->budget);
    }
}
static int ls_traceback(lua_State *L) { const char *m = lua_tostring(L, 1); luaL_traceback(L, L, m ? m : "(error object is not a string)", 1); return 1; }

// new sandboxed state; NULL if out of memory
static lua_State *ls_new(LsBox *b, size_t mem_limit) {
    memset(b, 0, sizeof *b); b->limit = mem_limit;
    lua_State *L = lua_newstate(ls_alloc, b); if (!L) return NULL;
    *(LsBox **)lua_getextraspace(L) = b;
    static const luaL_Reg libs[] = { { LUA_GNAME, luaopen_base }, { LUA_TABLIBNAME, luaopen_table }, { LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math },
                                     { LUA_UTF8LIBNAME, luaopen_utf8 }, { LUA_COLIBNAME, luaopen_coroutine }, { NULL, NULL } };
    for (const luaL_Reg *l = libs; l->name; l++) { luaL_requiref(L, l->name, l->func, 1); lua_pop(L, 1); }
    static const char *const drop[] = { "dofile", "loadfile", "load", NULL };
    for (int i = 0; drop[i]; i++) { lua_pushnil(L); lua_setglobal(L, drop[i]); }
    lua_getglobal(L, "string"); lua_pushnil(L); lua_setfield(L, -2, "dump"); lua_pop(L, 1);
    lua_sethook(L, ls_hook, LUA_MASKCOUNT, LS_HOOK_STEP);
    return L;
}
// load a TEXT chunk (binary chunks are refused: they can break the VM); 0 = ok (function on the stack), else the error message is on the stack
static int ls_load(lua_State *L, const char *buf, size_t n, const char *chunkname) { return luaL_loadbufferx(L, buf, n, chunkname, "t"); }
// call the function below nargs arguments with a fresh budget; 0 = ok, else the error message (with traceback) is on the stack
static int ls_call(lua_State *L, int nargs, int nres, long long budget) {
    LsBox *b = ls_box(L); b->count = 0; b->budget = budget; lua_sethook(L, ls_hook, LUA_MASKCOUNT, LS_HOOK_STEP);
    int base = lua_gettop(L) - nargs; lua_pushcfunction(L, ls_traceback); lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nres, base); lua_remove(L, base); b->budget = 0; return rc;
}
#endif

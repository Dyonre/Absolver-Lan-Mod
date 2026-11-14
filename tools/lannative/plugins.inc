// Plugin API v2 (included by lannative.c after rowedit.inc; its entry points are forward-declared at the top of lannative.c).
// A mod folder with plugin.dll is loaded when its name is listed in Mods\plugins.txt (the loader appends "#Name" for new ones) and the mod is enabled in loadorder.txt.
// The plugin gets one AbsApi (AbsolverSE.h) with logging, events, stackable hooks with byte checks, and reflection (objects, properties, functions).
// v2: every API call carries the plugin's own AbsApi pointer ("self"), so identity survives hook detours and plugin threads.
// Every call into a plugin is wrapped in SEH; after 3 faults its callbacks are switched off. Guide: docs\PLUGIN-API.md.
#include "AbsolverSE.h"
#include <tlhelp32.h>
#define RVA_PROCESSEVENT   0x8ADC30u     // UObject::ProcessEvent(this, UFunction *, void *params)
#define RVA_FINDFUNCTION   0x8A8280u     // UFunction *UObject::FindFunction(this, FName)
#define RVA_STATICLOADOBJ  0x8D1C10u     // StaticLoadObject(cls, outer, name, filename, loadFlags, sandbox, bAllowReconciliation)
#define PLUG_MAX 64                 // timers.h TM_PLUGINS is 64 too
#define PLUG_SUBS 192

typedef struct { char name[MOD_NAME_MAX]; char dir[MAX_PATH * 2]; wchar_t dll[MAX_PATH]; HMODULE module; int loaded, faults, builtin; AbsApi api; AbsPluginUnload_t unload; } Plugin;
static Plugin g_plugins[PLUG_MAX]; static int g_plugins_n;
typedef struct { int plugin; void *cb; void *user; } PlugSub;
static PlugSub g_ps_tick[PLUG_SUBS], g_ps_load[PLUG_SUBS], g_ps_ready[PLUG_SUBS]; static int g_ps_tick_n, g_ps_load_n, g_ps_ready_n;
typedef struct { int plugin; void (*fn)(void *); void *user; } PlugJob;
static PlugJob g_pjobs[128]; static int g_pjobs_n;
static int g_plug_ready_fired;
static void se_fill_api(AbsApi *a);            // secore.inc: the v2.1 additions
static void se_tick(float dt);               // secore.inc: timers, world events
static void content_init(const AbsApi *a);   // content.inc: the loader's own "Content" plugin (built-in kind handlers, content scan at engine ready)
static int content_file_count(void);
static int api_p_register_content_kind(const AbsApi *self, const char *kind, int (*validate)(const char *, char *, int, void *), int (*apply)(const char *, const char *, const char *, void *), void *user);

static int plug_guard(int p) { return p >= 0 && p < g_plugins_n && g_plugins[p].faults < 3; }
static void plug_fault(int p, const char *what, unsigned long code) {
    if (p < 0 || p >= g_plugins_n) return;
    g_plugins[p].faults++;
    logf_("plugin %s: EXCEPTION 0x%08lX in %s (fault %d of 3)%s", g_plugins[p].name, code, what, g_plugins[p].faults, g_plugins[p].faults >= 3 ? " - its callbacks are now OFF" : "");
#ifdef LN_OVERLAY
    if (g_plugins[p].faults >= 3) ov_release_owner(p);                       // no overlay input or callbacks for a switched-off plugin
#endif
}
// -1 = not one of ours
static int plug_of(const AbsApi *self) {
    for (int i = 0; i < g_plugins_n; i++) if (self == &g_plugins[i].api) return i;
    return -1;
}
static const char *plug_name_of(int p) { return p >= 0 && p < g_plugins_n ? g_plugins[p].name : "?"; }

// ----- the API functions
static void api_p_log(const AbsApi *self, const char *fmt, ...) {
    char msg[900]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    logf_("plugin %s: %s", plug_name_of(plug_of(self)), msg);
}
static int api_p_config_get(const AbsApi *self, const char *key, char *out, int cap) {
    int p = plug_of(self); if (p < 0 || cap <= 0) return 0;
    out[0] = 0; wchar_t wd[MAX_PATH], path[MAX_PATH]; u8_to_w(g_plugins[p].dir, wd, MAX_PATH); swprintf(path, MAX_PATH, L"%ls\\plugin.cfg", wd);
    char *t = mods_read_text(path, 65536); if (!t) return 0; int found = 0; size_t kn = strlen(key);
    for (char *ln = strtok(t, "\r\n"); ln && !found; ln = strtok(NULL, "\r\n")) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (*ln == '#' || *ln == ';' || strncmp(ln, key, kn)) continue;
        const char *q = ln + kn; while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue; q++; while (*q == ' ' || *q == '\t') q++;
        snprintf(out, (size_t)cap, "%s", q); size_t n = strlen(out); while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0; found = 1;
    }
    free(t); return found;
}
static int plug_sub_add(int p, PlugSub *a, int *n, void *cb, void *user) {
    if (!cb || p < 0 || *n >= PLUG_SUBS) return 1;
    EnterCriticalSection(&g_lock); a[*n].plugin = p; a[*n].cb = cb; a[*n].user = user; (*n)++; LeaveCriticalSection(&g_lock); return 0;
}
static int api_p_on_tick(const AbsApi *self, void (*cb)(float, void *), void *user) { return plug_sub_add(plug_of(self), g_ps_tick, &g_ps_tick_n, (void *)cb, user); }
static int api_p_on_loaded(const AbsApi *self, void (*cb)(void *, void *), void *user) { return plug_sub_add(plug_of(self), g_ps_load, &g_ps_load_n, (void *)cb, user); }
static int api_p_on_ready(const AbsApi *self, void (*cb)(void *), void *user) {
    int p = plug_of(self);
    if (g_plug_ready_fired) { if (!cb || !plug_guard(p)) return 1; __try { cb(user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_engine_ready", GetExceptionCode()); } return 0; }
    return plug_sub_add(p, g_ps_ready, &g_ps_ready_n, (void *)cb, user);
}
static int api_p_run_game(const AbsApi *self, void (*fn)(void *), void *user) {
    int p = plug_of(self); if (!fn || p < 0) return 1; int rc = 1;
    EnterCriticalSection(&g_lock); if (g_pjobs_n < 128) { g_pjobs[g_pjobs_n].plugin = p; g_pjobs[g_pjobs_n].fn = fn; g_pjobs[g_pjobs_n].user = user; g_pjobs_n++; rc = 0; } LeaveCriticalSection(&g_lock);
    return rc;
}
static int api_p_is_game_thread(const AbsApi *self) { (void)self; return g_game_tid && GetCurrentThreadId() == g_game_tid; }

// ----- safe code patching: all other threads are suspended while the bytes change; a thread whose instruction pointer is inside the patched range
// is let run for a moment and the attempt repeated. No logging or heap use between SuspendThread and ResumeThread (a suspended thread may hold those locks).
static int safe_patch(uint8_t *dst, const uint8_t *src, size_t n) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId(), old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    HANDLE th[256];
    for (int attempt = 0; attempt < 200; attempt++) {
        int nth = 0, inside = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me && nth < 256) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (h) { if (SuspendThread(h) != (DWORD)-1) th[nth++] = h; else CloseHandle(h); }
            }
        } while (Thread32Next(snap, &te));
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        for (int i = 0; i < nth && !inside; i++) { CONTEXT c; c.ContextFlags = CONTEXT_CONTROL; if (GetThreadContext(th[i], &c) && c.Rip >= (DWORD64)dst && c.Rip < (DWORD64)(dst + n)) inside = 1; }
        if (!inside) { memcpy(dst, src, n); FlushInstructionCache(GetCurrentProcess(), dst, n); }
        for (int i = 0; i < nth; i++) { ResumeThread(th[i]); CloseHandle(th[i]); }
        if (!inside) { DWORD o2; VirtualProtect(dst, n, old, &o2); return 1; }
        Sleep(1);
    }
    DWORD o2; VirtualProtect(dst, n, old, &o2); return 0;
}

// ----- hooks: first hook on a function patches the game code (jmp [rip+0] to the detour, original bytes moved to a trampoline); further hooks, by plugins or
// by the loader's own tables, stack on top by rewriting the jmp's target address. The returned "original" is always the next function in the chain.
static void *api_p_hook_rva(const AbsApi *self, const char *name, uint32_t rva, const uint8_t *expected, uint32_t stolen, void *detour) {
    int p = plug_of(self); char tag[160]; snprintf(tag, sizeof tag, "%s:%s", plug_name_of(p), name ? name : "?");
    if (p < 0 || !expected || !detour || stolen < 14 || stolen > HR_STOLEN || rva < 0x1000 || rva >= g_exe_size) { logf_("plugin hook %s refused (bad arguments)", tag); return NULL; }
    uint8_t *target = g_base + rva;
    EnterCriticalSection(&g_lock);
    HookEntry *h = hr_find(&g_hreg, rva); void *ret = NULL;
    if (h) {                                                                                      // already patched by us: stack on top
        if (!hr_bytes_match(h, expected, stolen)) { logf_("plugin hook %s: the original bytes you pass differ from the game's - NOT stacked", tag); }
        else if (!(target[0] == 0xFF && target[1] == 0x25 && *(uint32_t *)(target + 2) == 0 && *(void **)(target + 6) == h->top)) { logf_("plugin hook %s: the jump at the target no longer matches the registry (someone patched it outside the loader) - NOT stacked", tag); }
        else {
            uint8_t addr[8]; memcpy(addr, &detour, 8);
            if (safe_patch(target + 6, addr, 8)) { ret = hr_stack(h, detour, tag); logf_("plugin hook %s: stacked on base+0x%X (depth %d)", tag, rva, h->depth); }
            else logf_("plugin hook %s: could not patch safely (a thread stayed inside the function) - NOT stacked", tag);
        }
    } else {
        if (memcmp(target, expected, stolen) != 0) {
            char got[80] = "", *g = got; for (unsigned i = 0; i < stolen && i < 16; i++) g += sprintf(g, "%02X ", target[i]);
            logf_("plugin hook %s: live bytes differ from what the plugin expects (%s) - NOT patched (patched by something else, or another game build)", tag, got);
        } else {
            uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (tramp) {
                memcpy(tramp, target, stolen); emit_abs_jmp(tramp + stolen, (uint64_t)(target + stolen));
                uint8_t patch[HR_STOLEN]; memset(patch, 0xCC, sizeof patch); emit_abs_jmp(patch, (uint64_t)detour);
                if (safe_patch(target, patch, stolen) && hr_add(&g_hreg, rva, stolen, expected, tramp, detour, tag)) { ret = tramp; logf_("plugin hook %s: installed at base+0x%X (%u bytes)", tag, rva, stolen); }
                else logf_("plugin hook %s: patching failed", tag);
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return ret;
}

static void *api_p_find_object(const AbsApi *self, const char *path) { (void)self; wchar_t w[512]; u8_to_w(path, w, 512); void *r = NULL; __try { r = obj_find_any(w); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r; }
static void *api_p_load_object(const AbsApi *self, const char *path) {
    (void)self; wchar_t w[512]; u8_to_w(path, w, 512); void *r = NULL;
    __try { void *cls = obj_find_any(L"/Script/CoreUObject.Object"); r = ((void *(*)(void *, void *, const wchar_t *, const wchar_t *, uint32_t, void *, int))(g_base + RVA_STATICLOADOBJ))(cls, NULL, w, NULL, 0, NULL, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r;
}
static void *api_p_object_class(const AbsApi *self, void *obj) { (void)self; return obj ? *(void **)((uint8_t *)obj + 0x10) : NULL; }
static int api_p_object_name(const AbsApi *self, void *obj, char *out, int cap) { (void)self; if (!obj || cap <= 0) return 0; fname_text(*(FNameRaw *)((uint8_t *)obj + 0x18), out, cap); return (int)strlen(out); }
static int api_p_class_name(const AbsApi *self, void *cls, char *out, int cap) { return api_p_object_name(self, cls, out, cap); }
static int api_p_is_a(const AbsApi *self, void *obj, const char *cname) {
    (void)self; if (!obj) return 0; char n[100];
    for (uint8_t *c = *(uint8_t **)((uint8_t *)obj + 0x10); c; c = *(uint8_t **)(c + 0x30)) { fname_text(*(FNameRaw *)(c + 0x18), n, sizeof n); if (!strcmp(n, cname)) return 1; }
    return 0;
}
static void *api_p_find_property(const AbsApi *self, void *cls, const char *name) {
    (void)self; wchar_t w[128]; u8_to_w(name, w, 128); FNameRaw f = fname_of(w); uint8_t *s = cls;
    for (; s; s = *(uint8_t **)(s + 0x30)) for (uint8_t *fl = *(uint8_t **)(s + 0x38); fl; fl = *(uint8_t **)(fl + 0x28)) if (*(uint64_t *)(fl + 0x18) == *(uint64_t *)&f && field_is_property(fl)) return fl;
    return NULL;
}
static int api_p_property_offset(const AbsApi *self, void *prop) { (void)self; return prop ? *(int *)((uint8_t *)prop + 0x44) : -1; }
// "name[N]" = element N of a static-array property (ArrayDim > 1, e.g. EquipmentData m_Mesh[1] = the female mesh); plain "name" = element 0.
// Returns the element's address, or NULL (unknown property, bad index).
static uint8_t *api_p_elem(const AbsApi *self, void *obj, const char *name, uint8_t **prop) {
    char base[128]; const char *lb = strchr(name, '['); int ix = 0;
    if (lb) { size_t n = (size_t)(lb - name); if (n >= sizeof base || lb[1] < '0' || lb[1] > '9') return NULL; memcpy(base, name, n); base[n] = 0; ix = atoi(lb + 1); name = base; }
    uint8_t *pr = api_p_find_property(self, *(void **)((uint8_t *)obj + 0x10), name); if (!pr || ix >= *(int *)(pr + 0x30)) return NULL;
    *prop = pr; return (uint8_t *)obj + *(int *)(pr + 0x44) + ix * *(int *)(pr + 0x34);
}
static int api_p_get_text(const AbsApi *self, void *obj, const char *name, char *out, int cap) {
    if (!obj || cap <= 0) return -1; out[0] = 0; int r = -1;
    __try {
        uint8_t *pr = NULL, *val = api_p_elem(self, obj, name, &pr); if (!val) return -1;
        wchar_t *t = prop_export_w(pr, val, obj);
        if (t) { int n = WideCharToMultiByte(CP_UTF8, 0, t, -1, out, cap, NULL, NULL); r = n > 0 ? n - 1 : -1; free(t); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
static int api_p_set_text(const AbsApi *self, void *obj, const char *name, const char *text) {
    if (!obj) return 0; int ok = 0; wchar_t *w = utf8_to_wide(text); if (!w) return 0;
    __try { uint8_t *pr = NULL, *val = api_p_elem(self, obj, name, &pr); if (val) ok = prop_import_w(pr, val, obj, w); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(w); return ok;
}
static void *api_p_find_function(const AbsApi *self, void *obj, const char *name) {
    (void)self; if (!obj) return NULL; wchar_t w[128]; u8_to_w(name, w, 128); FNameRaw f = fname_of(w); void *r = NULL;
    __try { r = ((void *(*)(void *, FNameRaw))(g_base + RVA_FINDFUNCTION))(obj, f); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r;
}
static int api_p_call(const AbsApi *self, void *obj, void *fn, void *params) {
    if (!obj || !fn) return 0; if (!api_p_is_game_thread(self)) { logf_("plugin %s: call_function refused - not on the game thread", plug_name_of(plug_of(self))); return 0; }
    __try { ((void (*)(void *, void *, void *))(g_base + RVA_PROCESSEVENT))(obj, fn, params); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: EXCEPTION 0x%08lX inside ProcessEvent", plug_name_of(plug_of(self)), GetExceptionCode()); return 0; }
}
static void (*g_pfe_cb)(void *, void *); static void *g_pfe_user; static int g_pfe_n;
static void pfe_visit(void *o, void *ctx) { (void)ctx; g_pfe_n++; g_pfe_cb(o, g_pfe_user); }
static int api_p_for_each(const AbsApi *self, void (*cb)(void *, void *), void *user) { (void)self; g_pfe_cb = cb; g_pfe_user = user; g_pfe_n = 0; api_scan_objects(pfe_visit, NULL); return g_pfe_n; }

static void plug_fill_api(Plugin *pl) {
    AbsApi *a = &pl->api; memset(a, 0, sizeof *a); a->size = sizeof *a; a->version = ABS_API_VERSION; a->game_base = g_base; a->plugin_name = pl->name; a->plugin_dir = pl->dir;
    a->log = api_p_log; a->config_get = api_p_config_get; a->on_tick = api_p_on_tick; a->on_object_loaded = api_p_on_loaded; a->on_engine_ready = api_p_on_ready;
    a->run_on_game_thread = api_p_run_game; a->is_game_thread = api_p_is_game_thread; a->hook_rva = api_p_hook_rva;
    a->find_object = api_p_find_object; a->load_object = api_p_load_object; a->object_class = api_p_object_class; a->object_name = api_p_object_name; a->class_name = api_p_class_name;
    a->is_a = api_p_is_a; a->find_property = api_p_find_property; a->property_offset = api_p_property_offset; a->get_property_text = api_p_get_text; a->set_property_text = api_p_set_text;
    a->find_function = api_p_find_function; a->call_function = api_p_call; a->for_each_object = api_p_for_each;
    se_fill_api(a);
}
static int plug_guard_ok(void) {                                    // the engine functions behind the API still match this build
    static int state; if (state) return state > 0; state = 1;
    static const struct { unsigned rva; uint8_t b[10]; int n; const char *name; } g[] = {
        { RVA_PROCESSEVENT, { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56 }, 10, "UObject::ProcessEvent" },
        { RVA_FINDFUNCTION, { 0x48,0x83,0xEC,0x28,0x48,0x8B,0x49,0x10,0x41,0xB8 }, 10, "UObject::FindFunction" },
        { RVA_STATICLOADOBJ, { 0x40,0x55,0x53,0x56,0x41,0x54,0x41,0x55,0x41,0x56 }, 10, "StaticLoadObject" } };
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {                  // a function the loader hooks itself (ProcessEvent) is checked against the game bytes the registry saved
        const HookEntry *he = hr_find(&g_hreg, g[i].rva); const uint8_t *live = he ? he->orig : g_base + g[i].rva;
        if (memcmp(live, g[i].b, (size_t)g[i].n) != 0) { logf_("plugins: %s bytes differ from this build - plugins are OFF", g[i].name); state = -1; }
    }
    return state > 0 && rows_guard();
}

// ----- registration (called by the mod loader for every enabled mod that has a plugin.dll)
// plugins.txt: one mod folder name per line; "#Name" = known but not allowed. Returns 1 if the plugin is allowed and registered.
static int plugin_register(const wchar_t *mods_dir, const char *name, const wchar_t *dll, const wchar_t *dirw) {
    wchar_t pf[MAX_PATH]; swprintf(pf, MAX_PATH, L"%ls\\plugins.txt", mods_dir); char *t = mods_read_text(pf, 65536); int allowed = 0, known = 0;
    if (t) { for (char *ln = strtok(t, "\r\n"); ln; ln = strtok(NULL, "\r\n")) { while (*ln == ' ' || *ln == '\t') ln++; int hash = *ln == '#'; if (hash) { ln++; while (*ln == ' ') ln++; } size_t n = strlen(ln); while (n && (ln[n - 1] == ' ' || ln[n - 1] == '\t')) ln[--n] = 0;
                if (!_stricmp(ln, name)) { known = 1; if (!hash) allowed = 1; } } free(t); }
    if (!known) {
        HANDLE h = CreateFileW(pf, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char head[] = "# Mods that contain a plugin.dll (code). A plugin only loads when its name is listed here without the #.\r\n"; char line[MOD_NAME_MAX + 8];
            if (!t && GetFileSize(h, NULL) == 0) { DWORD w; WriteFile(h, head, (DWORD)strlen(head), &w, NULL); }
            DWORD w; int n = snprintf(line, sizeof line, "#%s\r\n", name); WriteFile(h, line, (DWORD)n, &w, NULL); CloseHandle(h);
        }
    }
    if (!allowed) { logf_("mods: %s has a plugin.dll that is NOT allowed (code!): remove the # before %s in Mods\\plugins.txt to allow it", name, name); return 0; }
    if (g_plugins_n >= PLUG_MAX) { logf_("mods: too many plugins (%d)", PLUG_MAX); return 0; }
    Plugin *pl = &g_plugins[g_plugins_n]; memset(pl, 0, sizeof *pl); snprintf(pl->name, sizeof pl->name, "%s", name); wcscpy(pl->dll, dll); w_to_u8(dirw, pl->dir, sizeof pl->dir);
    g_plugins_n++; return 1;
}
// The loader's own plugin: carries the content registry (built-in kinds: content.inc) when at least one mod ships content\<kind>\*.json. Registered last, started with the others.
static void plugin_register_builtin(void) {
    if (!content_file_count() || g_plugins_n >= PLUG_MAX) return;
    Plugin *pl = &g_plugins[g_plugins_n]; memset(pl, 0, sizeof *pl); strcpy(pl->name, "Content"); pl->builtin = 1; g_plugins_n++;
    logf_("mods: content registry active (%d content file(s)); built-in kinds items, styles", content_file_count());
}
// load and start every registered plugin (after the pak mount, still before the game's first tick)
static void plugins_load_all(void) {
    if (!g_plugins_n) return;
    if (!plug_guard_ok()) { g_plugins_n = 0; return; }
    for (int i = 0; i < g_plugins_n; i++) {
        Plugin *pl = &g_plugins[i];
        if (pl->builtin) { plug_fill_api(pl); pl->loaded = 1; __try { content_init(&pl->api); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(i, "content_init", GetExceptionCode()); pl->faults = 3; } continue; }
        wchar_t dd[MAX_PATH]; wcscpy(dd, pl->dll); wchar_t *sl = wcsrchr(dd, L'\\'); if (sl) *sl = 0; SetDllDirectoryW(dd);
        pl->module = LoadLibraryExW(pl->dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH); SetDllDirectoryW(NULL);
        if (!pl->module) { logf_("plugin %s: LoadLibrary failed (error %lu)", pl->name, GetLastError()); pl->faults = 3; continue; }
        AbsPluginLoad_t load = (AbsPluginLoad_t)GetProcAddress(pl->module, "AbsPlugin_Load");
        if (!load) { logf_("plugin %s: no AbsPlugin_Load export - not a plugin", pl->name); pl->faults = 3; continue; }
        pl->unload = (AbsPluginUnload_t)GetProcAddress(pl->module, "AbsPlugin_Unload"); plug_fill_api(pl);
        int rc = -1;
        __try { rc = load(&pl->api); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(i, "AbsPlugin_Load", GetExceptionCode()); pl->faults = 3; }
        pl->loaded = rc == 0; if (rc != 0 && pl->faults < 3) { logf_("plugin %s: AbsPlugin_Load returned %d - its callbacks are OFF (a plugin built for API v1 refuses to load on v2: rebuild it against AbsolverSE.h v2)", pl->name, rc); pl->faults = 3; }
        else if (rc == 0) logf_("plugin %s: loaded (API %d)", pl->name, ABS_API_VERSION);
    }
}

// ----- events (called from the hooks in lannative.c / rowedit.inc)
static void plugins_tick(float dt) {
    if (!g_plugins_n) return;
    se_tick(dt);
    if (!g_plug_ready_fired) {
        g_plug_ready_fired = 1;
        for (int i = 0; i < g_ps_ready_n; i++) { int p = g_ps_ready[i].plugin; if (!plug_guard(p)) continue;
            __try { ((void (*)(void *))g_ps_ready[i].cb)(g_ps_ready[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_engine_ready", GetExceptionCode()); } }
    }
    PlugJob todo[128]; int n;
    EnterCriticalSection(&g_lock); n = g_pjobs_n; memcpy(todo, g_pjobs, (size_t)n * sizeof todo[0]); g_pjobs_n = 0; LeaveCriticalSection(&g_lock);
    for (int i = 0; i < n; i++) { int p = todo[i].plugin; if (!plug_guard(p)) continue;
        __try { todo[i].fn(todo[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "run_on_game_thread", GetExceptionCode()); } }
    for (int i = 0; i < g_ps_tick_n; i++) { int p = g_ps_tick[i].plugin; if (!plug_guard(p)) continue;
        __try { ((void (*)(float, void *))g_ps_tick[i].cb)(dt, g_ps_tick[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_tick", GetExceptionCode()); } }
}
static void plugins_postload(void *obj) {
    for (int i = 0; i < g_ps_load_n; i++) { int p = g_ps_load[i].plugin; if (!plug_guard(p)) continue;
        __try { ((void (*)(void *, void *))g_ps_load[i].cb)(obj, g_ps_load[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_object_loaded", GetExceptionCode()); } }
}
static void plugins_unload_all(void) {
#ifdef LN_OVERLAY
    for (int i = 0; i < g_plugins_n; i++) ov_release_owner(i);              // the overlay drops every plugin's modes and callbacks before any plugin DLL goes away
#endif
    for (int i = 0; i < g_plugins_n; i++) if (g_plugins[i].loaded && g_plugins[i].unload) { __try { g_plugins[i].unload(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
}

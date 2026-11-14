// secore.inc - script extender core (included by lannative.c after plugins.inc). API v2.1 additions to AbsolverSE.h:
//   symbols      : hook_symbol / find_symbol from LanNative\symbols.bin (tools\lannative\gen_symbols.py: name -> RVA + original bytes + stolen length, from the game's PDB)
//   ProcessEvent : one loader hook on UObject::ProcessEvent: on_call(class, function, pre, post) by name, and the call tracer (ring buffer, dumped to LanNative\trace\)
//   natives      : replace_native(class, function, thunk, &orig) swaps UFunction::Func (no byte patching); find_class / find_function_in_class
//   properties   : prop_ptr / get_bool / set_bool; call_function_params (parameter block built from the UFunction's own property list)
//   sdk dump     : dump_sdk writes every class / struct / enum / function the live engine knows (also Blueprint classes) to LanNative\sdk\ - game derived, never commit
//   events       : set_timer, on_world_loaded; v2.3: on_hit, on_attack_start, on_pawn_spawned/destroyed, on_death, on_menu, char_info
// UE 4.18 layout used here (the same offsets as the rest of the reflection code): UObject Class +0x10 Name +0x18 Outer +0x20; UStruct Super +0x30 Children +0x38 Size +0x40;
// UField Next +0x28; UProperty ArrayDim +0x30 ElementSize +0x34 Flags +0x38 Offset +0x44, type data at +0x70 (Struct/Class/Inner/Enum...), UEnum Names TArray at +0x40.
#define RVA_PROCESSEVENT_HOOK 0x8ADC30u
#define PE_STOLEN 19
#define CPF_PARM 0x80ull
#define CPF_OUTPARM 0x100ull
#define CPF_RETURNPARM 0x400ull

static int se_valid_ptr(const void *p) { return p && ((uintptr_t)p > 0x10000) && ((uintptr_t)p < 0x00007FFFFFFFFFFFull); }

// ----- names and paths
static void se_name_of(const void *obj, char *out, int cap) { out[0] = 0; if (!se_valid_ptr(obj)) return; fname_text(*(const FNameRaw *)((const uint8_t *)obj + 0x18), out, cap); }
static void se_path_of(const void *obj, char *out, int cap) {           // pkg.Name / pkg.Outer:Name, like UObject::GetPathName
    char parts[16][96]; int n = 0; const void *o = obj;
    for (int i = 0; i < 16 && se_valid_ptr(o); i++) { se_name_of(o, parts[n++], 96); o = *(void *const *)((const uint8_t *)o + 0x20); }
    int w = 0; out[0] = 0;
    for (int i = n - 1; i >= 0 && w < cap - 2; i--) { if (i != n - 1) out[w++] = (i == n - 2 || n == 1) ? '.' : ':'; w += snprintf(out + w, (size_t)(cap - w), "%s", parts[i]); }
}
static int se_class_kind_is(const void *cls, const char *name) {         // does the class or a parent class have this short name?
    char n[100]; for (const uint8_t *c = cls; se_valid_ptr(c); c = *(uint8_t *const *)(c + 0x30)) { se_name_of(c, n, sizeof n); if (!strcmp(n, name)) return 1; } return 0;
}

// ----- symbols
static uint8_t *g_sym; static uint32_t g_sym_n; static int g_sym_state;      // 0 not loaded, 1 loaded, -1 missing/bad
static uint64_t se_fnv64(const char *s) { uint64_t h = 0xcbf29ce484222325ull; for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x100000001b3ull; } return h; }
static int se_sym_load(void) {
    if (g_sym_state) return g_sym_state > 0;
    g_sym_state = -1; wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ssymbols.bin", g_dir);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (f == INVALID_HANDLE_VALUE) { logf_("symbols: %ls not found (hook_symbol / find_symbol are off; tools\\lannative\\gen_symbols.py makes it)", path); return 0; }
    DWORD sz = GetFileSize(f, NULL), rd = 0; uint8_t *b = sz > 12 ? malloc(sz) : NULL;
    if (b) ReadFile(f, b, sz, &rd, NULL); CloseHandle(f);
    if (!b || rd != sz || memcmp(b, "ASYM1\0\0\0", 8) != 0) { free(b); logf_("symbols: symbols.bin is not valid"); return 0; }
    uint32_t n; memcpy(&n, b + 8, 4); if ((uint64_t)n * 37 + 12 != sz) { free(b); logf_("symbols: symbols.bin size does not match its count"); return 0; }
    g_sym = b; g_sym_n = n; g_sym_state = 1; logf_("symbols: %u hookable symbols loaded", n); return 1;
}
static int se_sym_lookup(const char *name, uint32_t *rva, uint32_t *stolen, uint8_t *bytes) {
    if (!se_sym_load()) return 0; uint64_t h = se_fnv64(name); uint32_t lo = 0, hi = g_sym_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2; const uint8_t *e = g_sym + 12 + (size_t)mid * 37; uint64_t eh; memcpy(&eh, e, 8);
        if (eh == h) { memcpy(rva, e + 8, 4); *stolen = e[12]; memcpy(bytes, e + 13, 24); return 1; }
        if (eh < h) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static void *api_p_hook_symbol(const AbsApi *self, const char *name, void *detour) {
    uint32_t rva, st; uint8_t b[24];
    if (!name || !se_sym_lookup(name, &rva, &st, b) || st < 14) { logf_("plugin %s: hook_symbol '%s': not found or not hookable (too short / position dependent prologue)", plug_name_of(plug_of(self)), name ? name : "?"); return NULL; }
    return api_p_hook_rva(self, name, rva, b, st, detour);
}
static uint32_t api_p_find_symbol(const AbsApi *self, const char *name) { (void)self; uint32_t rva = 0, st; uint8_t b[24]; return name && se_sym_lookup(name, &rva, &st, b) ? rva : 0; }

// ----- classes and functions by name
static void *se_find_class(const char *name) {
    static struct { uint64_t fn; void *cls; } cache[64]; static int used;
    wchar_t w[128]; u8_to_w(name, w, 128); FNameRaw f = fname_of(w); uint64_t key = *(uint64_t *)&f;
    for (int i = 0; i < used; i++) if (cache[i].fn == key) return cache[i].cls;
    struct { uint64_t key; void *found; } ctx = { key, NULL };
    uint8_t *array = g_base + RVA_GUOBJECTARRAY; uint8_t *items = *(uint8_t **)(array + 0x10); int count = *(int32_t *)(array + 0x1C);
    for (int i = 0; i < count && !ctx.found; i++) {
        uint8_t *item = items + (size_t)i * 24; if (!object_item_is_live(*(uint32_t *)(item + 8)) || !*(void **)item) continue;
        uint8_t *o = *(uint8_t **)item; if (*(uint64_t *)(o + 0x18) != key) continue;
        if (se_class_kind_is(*(void **)(o + 0x10), "Class")) ctx.found = o;
    }
    if (ctx.found && used < 64) { cache[used].fn = key; cache[used].cls = ctx.found; used++; }
    return ctx.found;
}
static void *se_class_find_function(void *cls, const char *name) {          // UFunction by name in the class or a parent class
    static uint64_t fnFunction; if (!fnFunction) { FNameRaw f = fname_of(L"Function"); fnFunction = *(uint64_t *)&f; }
    wchar_t w[128]; u8_to_w(name, w, 128); FNameRaw nf = fname_of(w); uint64_t key = *(uint64_t *)&nf;
    for (uint8_t *s = cls; se_valid_ptr(s); s = *(uint8_t **)(s + 0x30))
        for (uint8_t *c = *(uint8_t **)(s + 0x38); se_valid_ptr(c); c = *(uint8_t **)(c + 0x28))
            if (*(uint64_t *)(c + 0x18) == key && *(uint64_t *)(*(uint8_t **)(c + 0x10) + 0x18) == fnFunction) return c;
    return NULL;
}
static void *api_p_find_class(const AbsApi *self, const char *name) { (void)self; void *r = NULL; __try { r = se_find_class(name); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r; }
static void *api_p_find_function_in_class(const AbsApi *self, void *cls, const char *name) { (void)self; void *r = NULL; __try { r = se_class_find_function(cls, name); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r; }

// ----- ProcessEvent: on_call and the tracer
typedef void (*PE_t)(void *obj, void *func, void *params);
static PE_t g_orig_pe;
typedef struct { int plugin; uint64_t fn, cls; int (*pre)(void *obj, void *func, void *params, void *user); void (*post)(void *obj, void *func, void *params, void *user); void *user; } OnCall;
static OnCall g_oncall[64]; static int g_oncall_n;
typedef struct { uint64_t t, fn, fcls, obj, ocls; } TraceRec;
#define TR_N 8192
static TraceRec g_tr[TR_N]; static volatile LONG g_tr_head; static volatile LONG g_tr_on; static uint64_t g_tr_filter[32]; static int g_tr_filter_n;
static volatile LONG g_pe_busy_any;                                          // fast path: nothing registered and not tracing
static int g_menu_n;                                                         // on_menu registrations (events v2.3 below); EnterState calls are flagged in se_enter
static int se_is_enter_state(uint64_t fn);
static void se_menu_fire(void *obj, void *func);
static void se_pe_flag(void) { g_pe_busy_any = (g_oncall_n > 0 || g_tr_on || g_menu_n > 0) ? 1 : 0; }

// shared by the two hooks: record the call in the tracer and run the matching on_call "pre" callbacks; returns nonzero if one asked to skip the original
static int se_enter(void *obj, uint8_t *f, void *params, int *hit, int *nh) {
    uint64_t fn = *(uint64_t *)(f + 0x18); void *outer = *(void **)(f + 0x20); uint64_t fcls = se_valid_ptr(outer) ? *(uint64_t *)((uint8_t *)outer + 0x18) : 0; int skip = 0; *nh = 0;
    if (g_tr_on) {
        int rec = g_tr_filter_n == 0;
        for (int i = 0; i < g_tr_filter_n && !rec; i++) if (g_tr_filter[i] == fn || g_tr_filter[i] == fcls) rec = 1;
        if (rec) {
            LONG i = InterlockedIncrement(&g_tr_head) - 1; TraceRec *r = &g_tr[i & (TR_N - 1)];
            r->t = GetTickCount64(); r->fn = fn; r->fcls = fcls; r->obj = se_valid_ptr(obj) ? *(uint64_t *)((uint8_t *)obj + 0x18) : 0;
            void *oc = se_valid_ptr(obj) ? *(void **)((uint8_t *)obj + 0x10) : NULL; r->ocls = se_valid_ptr(oc) ? *(uint64_t *)((uint8_t *)oc + 0x18) : 0;
        }
    }
    for (int i = 0; i < g_oncall_n && *nh < 16; i++) {
        const OnCall *c = &g_oncall[i]; if (c->fn != fn || (c->cls && c->cls != fcls) || !plug_guard(c->plugin)) continue;
        hit[(*nh)++] = i; if (c->pre) { __try { if (c->pre(obj, f, params, c->user)) skip = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(c->plugin, "on_call pre", GetExceptionCode()); } }
    }
    if (g_menu_n && *nh < 16 && se_is_enter_state(fn)) hit[(*nh)++] = -1;    // -1 = fire on_menu after the call
    return skip;
}
static void se_leave(void *obj, void *func, void *params, const int *hit, int nh) {
    for (int k = 0; k < nh; k++) { if (hit[k] < 0) { se_menu_fire(obj, func); continue; } const OnCall *c = &g_oncall[hit[k]]; if (c->post && plug_guard(c->plugin)) { __try { c->post(obj, func, params, c->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(c->plugin, "on_call post", GetExceptionCode()); } } }
}
static __declspec(thread) void *t_pe_func;                       // the function ProcessEvent is running (its ProcessInternal call must not be reported twice)
static void hk_pe(void *obj, void *func, void *params) {
    if (!g_pe_busy_any || !func) { g_orig_pe(obj, func, params); return; }
    int hit[16], nh = 0; int skip = se_enter(obj, func, params, hit, &nh);
    if (!skip) { void *prev = t_pe_func; t_pe_func = func; g_orig_pe(obj, func, params); t_pe_func = prev; se_leave(obj, func, params, hit, nh); }
}
// UObject::ProcessInternal(context, FFrame &stack, result) runs every Blueprint script function, also the ones called from other Blueprint code that never touch ProcessEvent.
// FFrame: vtable (8) + two bools (padded to 16), Node (UStruct *, the UFunction) at +0x10, Object +0x18, Code +0x20, Locals +0x28 (the parameter block).
typedef void (*PI_t)(void *ctx, void *stack, void *result);
static PI_t g_orig_pi;
static void hk_pi(void *ctx, void *stack, void *result) {
    if (!g_pe_busy_any) { g_orig_pi(ctx, stack, result); return; }
    uint8_t *node = se_valid_ptr(stack) ? *(uint8_t **)((uint8_t *)stack + 0x10) : NULL;
    if (!se_valid_ptr(node) || node == (uint8_t *)t_pe_func) { g_orig_pi(ctx, stack, result); return; }
    void *locals = *(void **)((uint8_t *)stack + 0x28); int hit[16], nh = 0; int skip = se_enter(ctx, node, locals, hit, &nh);
    if (!skip) { g_orig_pi(ctx, stack, result); se_leave(ctx, node, locals, hit, nh); }
}static int api_p_on_call(const AbsApi *self, const char *cls, const char *func, int (*pre)(void *, void *, void *, void *), void (*post)(void *, void *, void *, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !func || (!pre && !post) || g_oncall_n >= 64 || !g_orig_pe) return 1;
    wchar_t w[128]; u8_to_w(func, w, 128); FNameRaw f = fname_of(w); uint64_t c = 0; if (cls && *cls) { u8_to_w(cls, w, 128); FNameRaw fc = fname_of(w); c = *(uint64_t *)&fc; }
    EnterCriticalSection(&g_lock); OnCall *o = &g_oncall[g_oncall_n]; o->plugin = p; o->fn = *(uint64_t *)&f; o->cls = c; o->pre = pre; o->post = post; o->user = user; g_oncall_n++; se_pe_flag(); LeaveCriticalSection(&g_lock);
    logf_("plugin %s: on_call %s::%s registered", plug_name_of(p), cls && *cls ? cls : "*", func); return 0;
}
static void se_trace_dump(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH]; swprintf(dir, MAX_PATH, L"%strace", g_dir); CreateDirectoryW(dir, NULL);
    static int seq; swprintf(path, MAX_PATH, L"%ls\\trace_%03d.txt", dir, ++seq);
    FILE *out = _wfopen(path, L"wb"); if (!out) { logf_("trace: cannot write %ls", path); return; }
    LONG head = g_tr_head; LONG n = head < TR_N ? head : TR_N; uint64_t t0 = n ? g_tr[(head - n) & (TR_N - 1)].t : 0;
    fprintf(out, "# ProcessEvent trace: %ld record(s) (ring of %d), time in ms since the first record. Columns: time  Class::Function  <-  ObjectClass  Object\r\n", (long)n, TR_N);
    for (LONG i = head - n; i < head; i++) {
        const TraceRec *r = &g_tr[i & (TR_N - 1)]; char fn[100], fc[100], on[100], oc[100];
        fname_text(*(const FNameRaw *)&r->fn, fn, sizeof fn); fname_text(*(const FNameRaw *)&r->fcls, fc, sizeof fc); fname_text(*(const FNameRaw *)&r->obj, on, sizeof on); fname_text(*(const FNameRaw *)&r->ocls, oc, sizeof oc);
        fprintf(out, "%8llu  %s::%s  <-  %s  %s\r\n", (unsigned long long)(r->t - t0), fc, fn, oc, on);
    }
    fclose(out); logf_("trace: %ld record(s) written to trace\\trace_%03d.txt", (long)n, seq);
}
// trace ctl: "on" [names...], "off", "dump", "clear"; names are function names or class names (exact, no prefix letters), up to 32
static int api_p_trace(const AbsApi *self, const char *cmd, const char *names) {
    (void)self; if (!cmd) return 1;
    if (!strcmp(cmd, "on")) {
        g_tr_filter_n = 0; g_tr_head = 0;
        if (names) { char buf[600]; snprintf(buf, sizeof buf, "%s", names); for (char *t = strtok(buf, ","); t && g_tr_filter_n < 32; t = strtok(NULL, ",")) { while (*t == ' ') t++; size_t tl = strlen(t); while (tl && t[tl - 1] == ' ') t[--tl] = 0; wchar_t w[128]; u8_to_w(t, w, 128); FNameRaw f = fname_of(w); g_tr_filter[g_tr_filter_n++] = *(uint64_t *)&f; } }
        g_tr_on = 1; se_pe_flag(); logf_("trace: ON (%d filter name(s))", g_tr_filter_n); return 0;
    }
    if (!strcmp(cmd, "off")) { g_tr_on = 0; se_pe_flag(); logf_("trace: OFF"); return 0; }
    if (!strcmp(cmd, "clear")) { g_tr_head = 0; return 0; }
    if (!strcmp(cmd, "dump")) { se_trace_dump(); return 0; }
    return 1;
}

// ----- replace_native
static int g_func_off = -1;                                   // offset of UFunction::Func (found by looking for a known exec thunk)
static int se_func_off(void) {
    if (g_func_off != -1) return g_func_off > 0;
    g_func_off = 0; void *cls = se_find_class("FightingPlayerController"); uint8_t *fn = cls ? se_class_find_function(cls, "BPF_SetTrainingRequest") : NULL;
    if (!fn) { logf_("replace_native: probe function FightingPlayerController::BPF_SetTrainingRequest not found - replace_native is off"); return 0; }
    for (int off = 0x80; off < 0x140; off += 8) if (*(void **)(fn + off) == (void *)(g_base + 0x5B43B0u)) { g_func_off = off; logf_("replace_native: UFunction::Func is at +0x%X", off); return 1; }
    logf_("replace_native: could not locate UFunction::Func (probe thunk not found in the function object) - replace_native is off"); return 0;
}
static int api_p_replace_native(const AbsApi *self, const char *cls, const char *func, void *thunk, void **orig) {
    int p = plug_of(self); if (p < 0 || !cls || !func || !thunk || !se_func_off()) return 1;
    void *c = se_find_class(cls); uint8_t *fn = c ? se_class_find_function(c, func) : NULL;
    if (!fn) { logf_("plugin %s: replace_native %s::%s: function not found", plug_name_of(p), cls, func); return 2; }
    void **slot = (void **)(fn + g_func_off);
    if (orig) *orig = *slot;
    *slot = thunk;                                            // one aligned pointer write: atomic on x64
    logf_("plugin %s: replace_native %s::%s: %p -> %p", plug_name_of(p), cls, func, orig ? *orig : NULL, thunk); return 0;
}


// ----- trace_function: log calls that never pass through ProcessEvent (Blueprint -> Blueprint calls and Blueprint -> native BPF_ calls run the UFunction's Func pointer directly).
// Per traced function a tiny generated stub replaces UFunction::Func: it saves the argument registers, calls se_nat_log(id, context), restores them and jumps to the original pointer.
typedef struct { uint8_t *fn; void *orig; } NatTrace;
static NatTrace g_nt[256]; static int g_nt_n; static uint8_t *g_stub_mem; static size_t g_stub_used;
static void se_nat_log(uint64_t id, void *ctx) {
    if (!g_tr_on || id >= (uint64_t)g_nt_n) return;
    uint8_t *f = g_nt[id].fn; void *outer = *(void **)(f + 0x20); void *oc = se_valid_ptr(ctx) ? *(void **)((uint8_t *)ctx + 0x10) : NULL;
    LONG i = InterlockedIncrement(&g_tr_head) - 1; TraceRec *r = &g_tr[i & (TR_N - 1)];
    r->t = GetTickCount64(); r->fn = *(uint64_t *)(f + 0x18); r->fcls = se_valid_ptr(outer) ? *(uint64_t *)((uint8_t *)outer + 0x18) : 0;
    r->obj = se_valid_ptr(ctx) ? *(uint64_t *)((uint8_t *)ctx + 0x18) : 0; r->ocls = se_valid_ptr(oc) ? *(uint64_t *)((uint8_t *)oc + 0x18) : 0;
}
static int api_p_trace_function(const AbsApi *self, const char *cls, const char *func) {
    int p = plug_of(self); if (p < 0 || !cls || !func || g_nt_n >= 256 || !se_func_off()) return 1;
    void *c = se_find_class(cls); uint8_t *fn = c ? se_class_find_function(c, func) : NULL; if (!fn) { logf_("trace_function %s::%s: function not found", cls, func); return 2; }
    for (int i = 0; i < g_nt_n; i++) if (g_nt[i].fn == fn) return 0;
    if (!g_stub_mem) { g_stub_mem = VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); g_stub_used = 0; if (!g_stub_mem) return 3; }
    if (g_stub_used + 96 > 64 * 1024) return 4;
    void **slot = (void **)(fn + g_func_off); uint64_t id = (uint64_t)g_nt_n, orig = (uint64_t)(uintptr_t)*slot, logger = (uint64_t)(uintptr_t)se_nat_log;
    uint8_t *s = g_stub_mem + g_stub_used, *q = s;
    *q++ = 0x51; *q++ = 0x52; *q++ = 0x41; *q++ = 0x50; *q++ = 0x41; *q++ = 0x51;          // push rcx, rdx, r8, r9
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xEC; *q++ = 0x28;                                      // sub rsp, 0x28
    *q++ = 0x48; *q++ = 0xB9; memcpy(q, &id, 8); q += 8;                                     // mov rcx, id
    *q++ = 0x48; *q++ = 0x8B; *q++ = 0x54; *q++ = 0x24; *q++ = 0x40;                         // mov rdx, [rsp+0x40]  (the saved context)
    *q++ = 0x48; *q++ = 0xB8; memcpy(q, &logger, 8); q += 8;                                 // mov rax, logger
    *q++ = 0xFF; *q++ = 0xD0;                                                                // call rax
    *q++ = 0x48; *q++ = 0x83; *q++ = 0xC4; *q++ = 0x28;                                      // add rsp, 0x28
    *q++ = 0x41; *q++ = 0x59; *q++ = 0x41; *q++ = 0x58; *q++ = 0x5A; *q++ = 0x59;            // pop r9, r8, rdx, rcx
    *q++ = 0x48; *q++ = 0xB8; memcpy(q, &orig, 8); q += 8;                                   // mov rax, orig
    *q++ = 0xFF; *q++ = 0xE0;                                                                // jmp rax
    g_stub_used += (size_t)(q - s) + 8; FlushInstructionCache(GetCurrentProcess(), s, (size_t)(q - s));
    g_nt[g_nt_n].fn = fn; g_nt[g_nt_n].orig = (void *)(uintptr_t)orig; g_nt_n++;
    *slot = s;
    logf_("plugin %s: trace_function %s::%s (stub %p, original %p)", plug_name_of(p), cls, func, (void *)s, (void *)(uintptr_t)orig); return 0;
}
// frame_peek_int: the first argument of a native function called from Blueprint, read from the script bytecode WITHOUT consuming it (the native exec function reads the arguments from the
// FFrame's code stream). Understands integer constants and variable reads (the common forms); returns 0 if the expression is something else.
static int api_p_frame_peek_int(const AbsApi *self, void *frame, int *out) {
    (void)self; if (!frame || !out) return 0; int ok = 0;
    __try {
        const uint8_t *code = *(const uint8_t **)((uint8_t *)frame + 0x20); uint8_t *obj = *(uint8_t **)((uint8_t *)frame + 0x18), *locals = *(uint8_t **)((uint8_t *)frame + 0x28);
        if (!code) return 0;
        switch (code[0]) {
            case 0x1D: *out = *(const int32_t *)(code + 1); ok = 1; break;                                   // EX_IntConst
            case 0x25: *out = 0; ok = 1; break;                                                              // EX_IntZero
            case 0x26: *out = 1; ok = 1; break;                                                              // EX_IntOne
            case 0x2C: *out = code[1]; ok = 1; break;                                                        // EX_IntConstByte
            case 0x24: *out = code[1]; ok = 1; break;                                                        // EX_ByteConst
            case 0x00: case 0x48: { const uint8_t *prop = *(const uint8_t *const *)(code + 1); if (se_valid_ptr(prop) && locals) { *out = *(const int32_t *)(locals + *(int *)(prop + 0x44)); ok = 1; } break; }   // EX_LocalVariable / LocalOutVariable
            case 0x01: { const uint8_t *prop = *(const uint8_t *const *)(code + 1); if (se_valid_ptr(prop) && obj) { *out = *(const int32_t *)(obj + *(int *)(prop + 0x44)); ok = 1; } break; }                      // EX_InstanceVariable
            default: break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}
// ----- property helpers
static void *api_p_prop_ptr(const AbsApi *self, void *obj, const char *name) {
    if (!obj) return NULL; void *r = NULL;
    __try { uint8_t *pr = api_p_find_property(self, *(void **)((uint8_t *)obj + 0x10), name); if (pr) r = (uint8_t *)obj + *(int *)(pr + 0x44); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    return r;
}
static int api_p_get_bool(const AbsApi *self, void *obj, const char *name, int *out) {            // native bool or bitfield; returns 1 if found
    if (!obj || !out) return 0; int ok = 0;
    __try { uint8_t *pr = api_p_find_property(self, *(void **)((uint8_t *)obj + 0x10), name); if (pr && !strcmp(prop_kind(pr), "Bool")) { uint8_t mask = pr[0x73] ? pr[0x73] : 1; *out = (*((uint8_t *)obj + *(int *)(pr + 0x44) + pr[0x71]) & mask) != 0; ok = 1; } } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}
static int api_p_set_bool(const AbsApi *self, void *obj, const char *name, int v) {
    if (!obj) return 0; int ok = 0;
    __try { uint8_t *pr = api_p_find_property(self, *(void **)((uint8_t *)obj + 0x10), name); if (pr && !strcmp(prop_kind(pr), "Bool")) { uint8_t mask = pr[0x73] ? pr[0x73] : 1; uint8_t *b = (uint8_t *)obj + *(int *)(pr + 0x44) + pr[0x71]; if (v) *b |= mask; else *b &= (uint8_t)~mask; ok = 1; } } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

// ----- call_function_params: build the parameter block from the UFunction's own properties (inputs in declaration order = ascending offset), call ProcessEvent, copy outs / the return value back
typedef struct { uint8_t *prop; int off, size; uint64_t flags; } SeParam;
static int se_params_of(void *func, SeParam *out, int cap) {
    int n = 0;
    for (uint8_t *c = *(uint8_t **)((uint8_t *)func + 0x38); se_valid_ptr(c) && n < cap; c = *(uint8_t **)(c + 0x28)) {
        if (!field_is_property(c)) continue; uint64_t fl = *(uint64_t *)(c + 0x38); if (!(fl & CPF_PARM)) continue;
        out[n].prop = c; out[n].off = *(int *)(c + 0x44); out[n].size = *(int *)(c + 0x34) * *(int *)(c + 0x30); out[n].flags = fl; n++;
    }
    for (int i = 1; i < n; i++) { SeParam t = out[i]; int j = i - 1; while (j >= 0 && out[j].off > t.off) { out[j + 1] = out[j]; j--; } out[j + 1] = t; }
    return n;
}
static int api_p_call_params(const AbsApi *self, void *obj, void *func, void **args, void *ret) {
    if (!obj || !func) return 0; if (!api_p_is_game_thread(self)) { logf_("plugin %s: call_function_params refused - not on the game thread", plug_name_of(plug_of(self))); return 0; }
    int ok = 0; SeParam ps[32]; int n = 0; uint8_t *block = NULL;
    __try {
        n = se_params_of(func, ps, 32); int psize = *(int *)((uint8_t *)func + 0x40); if (psize < 0 || psize > 4096) return 0;
        block = calloc(1, (size_t)psize + 32); if (!block) return 0;
        int ai = 0;
        for (int i = 0; i < n; i++) { if (ps[i].flags & CPF_RETURNPARM) continue; if (args && args[ai] && ps[i].off + ps[i].size <= psize) memcpy(block + ps[i].off, args[ai], (size_t)ps[i].size); ai++; }
        ((void (*)(void *, void *, void *))(g_base + RVA_PROCESSEVENT))(obj, func, block);
        ai = 0;
        for (int i = 0; i < n; i++) {
            if (ps[i].flags & CPF_RETURNPARM) { if (ret) memcpy(ret, block + ps[i].off, (size_t)ps[i].size); continue; }
            if ((ps[i].flags & CPF_OUTPARM) && args && args[ai]) memcpy(args[ai], block + ps[i].off, (size_t)ps[i].size);
            ai++;
        }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: EXCEPTION 0x%08lX in call_function_params", plug_name_of(plug_of(self)), GetExceptionCode()); ok = 0; }
    free(block); return ok;
}

// ----- object creation
static void *api_p_construct_object(const AbsApi *self, void *cls, void *outer) {          // StaticConstructObject_Internal(cls, outer, NAME_None, flags 0, ...)
    (void)self; void *r = NULL; if (!cls) return NULL;
    __try { r = ((StaticConstructObject_t)(g_base + RVA_STATIC_CONSTRUCT))(cls, outer, 0, 0, 0, NULL, 0, NULL, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("construct_object: EXCEPTION 0x%08lX", GetExceptionCode()); r = NULL; }
    return r;
}
static void *api_p_spawn_actor(const AbsApi *self, void *cls, const float loc[3], const float rot[3]) {
    (void)self; void *w = world(); void *r = NULL; if (!w || !cls) return NULL; const float z[3] = { 0, 0, 0 };
    __try { r = npc_spawn_actor_rot(w, cls, loc ? loc : z, rot ? rot : z); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("spawn_actor: EXCEPTION 0x%08lX", GetExceptionCode()); r = NULL; }
    return r;
}
static void *api_p_get_world(const AbsApi *self) { (void)self; return world(); }


// ----- more object creation: duplicate, class default objects, widgets
#define RVA_STATIC_DUPLICATE 0x8CFC70u    // StaticDuplicateObject(src, outer, FName name, flag mask, UClass *dest class, mode, internal flag mask)
static void *api_p_duplicate_object(const AbsApi *self, void *src, void *outer) {
    (void)self; void *r = NULL; if (!src) return NULL;
    __try { r = ((void *(*)(void *, void *, uint64_t, uint32_t, void *, int, uint32_t))(g_base + RVA_STATIC_DUPLICATE))(src, outer, 0, 0xFFFFFFFFu, NULL, 0, 0xFFFFFFFFu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf_("duplicate_object: EXCEPTION 0x%08lX", GetExceptionCode()); r = NULL; }
    return r;
}
static void *se_find_named(const char *name) {                    // first live object with exactly this name (cached)
    static struct { uint64_t fn; void *o; } cache[32]; static int used;
    wchar_t w[160]; u8_to_w(name, w, 160); FNameRaw f = fname_of(w); uint64_t key = *(uint64_t *)&f;
    for (int i = 0; i < used; i++) if (cache[i].fn == key) return cache[i].o;
    uint8_t *array = g_base + RVA_GUOBJECTARRAY; uint8_t *items = *(uint8_t **)(array + 0x10); int count = *(int32_t *)(array + 0x1C); void *found = NULL;
    for (int i = 0; i < count && !found; i++) { uint8_t *item = items + (size_t)i * 24; if (!object_item_is_live(*(uint32_t *)(item + 8)) || !*(void **)item) continue; if (*(uint64_t *)((uint8_t *)*(void **)item + 0x18) == key) found = *(void **)item; }
    if (found && used < 32) { cache[used].fn = key; cache[used].o = found; used++; }
    return found;
}
static void *api_p_find_cdo(const AbsApi *self, const char *cls) { (void)self; char n[200]; snprintf(n, sizeof n, "Default__%s", cls); void *r = NULL; __try { r = se_find_named(n); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r; }
static void *api_p_create_widget(const AbsApi *self, void *widget_class, void *owner) {      // UWidgetBlueprintLibrary::Create(world context, class, owning player) through reflection
    void *lib = api_p_find_cdo(self, "WidgetBlueprintLibrary"), *lc = se_find_class("WidgetBlueprintLibrary"), *fn = lc ? se_class_find_function(lc, "Create") : NULL, *w = world(), *ret = NULL;
    if (!lib || !fn || !w || !widget_class) { logf_("create_widget: library / Create / world / class missing"); return NULL; }
    void *args[3] = { &w, &widget_class, &owner }; if (!api_p_call_params(self, lib, fn, args, &ret)) return NULL; return ret;
}
static int api_p_add_to_viewport(const AbsApi *self, void *widget, int zorder) {
    void *c = se_find_class("UserWidget"), *fn = c ? se_class_find_function(c, "AddToViewport") : NULL; if (!widget || !fn) return 0;
    void *args[1] = { &zorder }; return api_p_call_params(self, widget, fn, args, NULL);
}
static int api_p_remove_from_parent(const AbsApi *self, void *widget) {
    void *c = se_find_class("Widget"), *fn = c ? se_class_find_function(c, "RemoveFromParent") : NULL; if (!widget || !fn) return 0;
    return api_p_call_params(self, widget, fn, NULL, NULL);
}
static void *api_p_local_pc(const AbsApi *self) { (void)self; void *r = NULL; __try { r = L_local_pc(); } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r; }
static int api_p_object_path(const AbsApi *self, void *obj, char *out, int cap) { (void)self; out[0] = 0; if (!obj || cap <= 0) return 0; __try { se_path_of(obj, out, cap); } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; } return (int)strlen(out); }
static int api_p_dump_object(const AbsApi *self, void *obj) {      // every property of one live object as engine text -> LanNative\objdump_<name>.txt (the same file the F11 objdump writes)
    (void)self; if (!obj) return 0; char path[400]; int ok = 0;
    __try { static int seq; char nm[100], cn[100]; se_name_of(obj, nm, sizeof nm); se_name_of(*(void **)((uint8_t *)obj + 0x10), cn, sizeof cn); se_path_of(obj, path, sizeof path); char fake[400]; snprintf(fake, sizeof fake, "%s.%s_%s_%d", path, cn, nm, ++seq); for (char *p = strrchr(fake, '.') ? strrchr(fake, '.') + 1 : fake; *p; p++) if (*p == ':' || *p == '/' || *p == '\\' || *p == ' ') *p = '_'; obj_dump(fake, obj); ok = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("dump_object: EXCEPTION 0x%08lX", GetExceptionCode()); }
    return ok;
}
// ----- SDK dump
static const char *se_prop_type(uint8_t *prop, char *buf, int cap, int depth);
static void se_obj_short(void *o, char *buf, int cap) { if (se_valid_ptr(o)) se_name_of(o, buf, cap); else snprintf(buf, (size_t)cap, "?"); }
static const char *se_prop_type(uint8_t *prop, char *buf, int cap, int depth) {
    const char *k = prop_kind(prop); char a[160] = "", b[160] = "";
    if (!strcmp(k, "Object") || !strcmp(k, "WeakObject") || !strcmp(k, "LazyObject") || !strcmp(k, "SoftObject") || !strcmp(k, "Interface")) { se_obj_short(*(void **)(prop + 0x70), a, sizeof a); snprintf(buf, (size_t)cap, "%s<%s>", k, a); }
    else if (!strcmp(k, "Class") || !strcmp(k, "SoftClass")) { se_obj_short(*(void **)(prop + 0x78), a, sizeof a); snprintf(buf, (size_t)cap, "%s<%s>", k, a); }
    else if (!strcmp(k, "Struct")) { se_obj_short(*(void **)(prop + 0x70), a, sizeof a); snprintf(buf, (size_t)cap, "Struct<%s>", a); }
    else if (!strcmp(k, "Array") || !strcmp(k, "Set")) { uint8_t *in = *(uint8_t **)(prop + 0x70); if (depth < 3 && se_valid_ptr(in)) se_prop_type(in, a, sizeof a, depth + 1); else snprintf(a, sizeof a, "?"); snprintf(buf, (size_t)cap, "%s<%s>", k, a); }
    else if (!strcmp(k, "Map")) { uint8_t *kp = *(uint8_t **)(prop + 0x70), *vp = *(uint8_t **)(prop + 0x78); if (depth < 3 && se_valid_ptr(kp)) se_prop_type(kp, a, sizeof a, depth + 1); else snprintf(a, sizeof a, "?"); if (depth < 3 && se_valid_ptr(vp)) se_prop_type(vp, b, sizeof b, depth + 1); else snprintf(b, sizeof b, "?"); snprintf(buf, (size_t)cap, "Map<%s,%s>", a, b); }
    else if (!strcmp(k, "Byte")) { void *e = *(void **)(prop + 0x70); if (se_valid_ptr(e)) { se_obj_short(e, a, sizeof a); snprintf(buf, (size_t)cap, "Byte<enum %s>", a); } else snprintf(buf, (size_t)cap, "Byte"); }
    else if (!strcmp(k, "Enum")) { se_obj_short(*(void **)(prop + 0x78), a, sizeof a); snprintf(buf, (size_t)cap, "Enum<%s>", a); }
    else if (!strcmp(k, "Delegate") || !strcmp(k, "MulticastDelegate")) { se_obj_short(*(void **)(prop + 0x70), a, sizeof a); snprintf(buf, (size_t)cap, "%s<%s>", k, a); }
    else snprintf(buf, (size_t)cap, "%s", k);
    return buf;
}
static void se_dump_struct(FILE *out, uint8_t *s, const char *kind) {
    char path[300], sup[100] = "", nm[100]; se_path_of(s, path, sizeof path); uint8_t *su = *(uint8_t **)(s + 0x30); if (se_valid_ptr(su)) se_name_of(su, sup, sizeof sup);
    fprintf(out, "%s %s%s%s size=0x%X\r\n", kind, path, sup[0] ? " : " : "", sup, *(int *)(s + 0x40));
    static uint64_t fnFunction; if (!fnFunction) { FNameRaw f = fname_of(L"Function"); fnFunction = *(uint64_t *)&f; }
    for (uint8_t *c = *(uint8_t **)(s + 0x38); se_valid_ptr(c); c = *(uint8_t **)(c + 0x28)) {
        __try {
            se_name_of(c, nm, sizeof nm);
            if (*(uint64_t *)(*(uint8_t **)(c + 0x10) + 0x18) == fnFunction) {
                SeParam ps[32]; int n = se_params_of(c, ps, 32); char ty[200], args[900] = "", ret[200] = "void"; int w = 0;
                for (int i = 0; i < n; i++) {
                    se_prop_type(ps[i].prop, ty, sizeof ty, 0); char pn[100]; se_name_of(ps[i].prop, pn, sizeof pn);
                    if (ps[i].flags & CPF_RETURNPARM) snprintf(ret, sizeof ret, "%s", ty);
                    else w += snprintf(args + w, sizeof args - (size_t)w, "%s%s%s %s", w ? ", " : "", (ps[i].flags & CPF_OUTPARM) ? "out " : "", ty, pn);
                }
                void *fp = g_func_off > 0 ? *(void **)(c + g_func_off) : NULL; char nat[40] = "";
                if (fp && (uint8_t *)fp >= g_base && (uint8_t *)fp < g_base + g_exe_size) snprintf(nat, sizeof nat, " native=+0x%llX", (unsigned long long)((uint8_t *)fp - g_base));
                fprintf(out, "  func %s(%s) -> %s flags=0x%X%s\r\n", nm, args, ret, *(uint32_t *)(c + 0x88), nat);
            } else if (field_is_property(c)) {
                char ty[200]; se_prop_type(c, ty, sizeof ty, 0); int dim = *(int *)(c + 0x30);
                fprintf(out, "  +0x%04X %s %s%s", *(int *)(c + 0x44), ty, nm, "");
                if (dim > 1) fprintf(out, "[%d]", dim);
                fprintf(out, "  size=%d\r\n", *(int *)(c + 0x34) * dim);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { fprintf(out, "  ?? (unreadable member)\r\n"); }
    }
}
static void se_dump_enum(FILE *out, uint8_t *e) {
    char path[300], nm[100]; se_path_of(e, path, sizeof path); fprintf(out, "enum %s\r\n", path);
    uint8_t *data = *(uint8_t **)(e + 0x40); int n = *(int *)(e + 0x48);
    if (!se_valid_ptr(data) || n < 0 || n > 4096) { fprintf(out, "  ?? (names unreadable)\r\n"); return; }
    for (int i = 0; i < n; i++) { fname_text(*(FNameRaw *)(data + (size_t)i * 16), nm, sizeof nm); fprintf(out, "  %s = %lld\r\n", nm, (long long)*(int64_t *)(data + (size_t)i * 16 + 8)); }
}
typedef struct { FILE *out; int classes, structs, enums, faults; } SeDump;
static void se_dump_visit(void *obj, void *ctx) {
    SeDump *d = ctx; uint8_t *o = obj; void *oc = *(void **)(o + 0x10); if (!se_valid_ptr(oc)) return;
    char cn[100]; se_name_of(oc, cn, sizeof cn);
    __try {
        if (strstr(cn, "Class") && se_class_kind_is(oc, "Class") && !strstr(cn, "Property")) { se_dump_struct(d->out, o, "class"); d->classes++; }
        else if (!strcmp(cn, "ScriptStruct") || !strcmp(cn, "UserDefinedStruct")) { se_dump_struct(d->out, o, "struct"); d->structs++; }
        else if (!strcmp(cn, "Enum") || !strcmp(cn, "UserDefinedEnum")) { se_dump_enum(d->out, o); d->enums++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { d->faults++; }
}
static int api_p_dump_sdk(const AbsApi *self, const char *name) {
    if (!api_p_is_game_thread(self) && g_game_tid) { logf_("dump_sdk refused - call it on the game thread"); return -1; }
    se_func_off();                                                    // so the dump can show native function addresses
    wchar_t dir[MAX_PATH], path[MAX_PATH]; swprintf(dir, MAX_PATH, L"%ssdk", g_dir); CreateDirectoryW(dir, NULL);
    wchar_t wn[100]; u8_to_w(name && *name ? name : "sdk", wn, 100); swprintf(path, MAX_PATH, L"%ls\\%ls.txt", dir, wn);
    FILE *out = _wfopen(path, L"wb"); if (!out) { logf_("sdk dump: cannot write %ls", path); return -1; }
    SeDump d = { out, 0, 0, 0, 0 }; DWORD t0 = GetTickCount();
    fprintf(out, "# Absolver live SDK dump (UE 4.18 layout). Game derived: do not commit or share. class/struct blocks list +offset type name, func lines list parameters.\r\n");
    api_scan_objects(se_dump_visit, &d); fclose(out);
    logf_("sdk dump: %d classes, %d structs, %d enums, %d unreadable object(s) in %lu ms -> sdk\\%ls.txt", d.classes, d.structs, d.enums, d.faults, GetTickCount() - t0, wn);
    return d.classes + d.structs + d.enums;
}

// ----- events: timers, world loaded
#include "timers.h"
static TmTable g_tm; static int g_tm_full_logged;                  // timers.h
typedef struct { int plugin; void (*cb)(const char *map, void *user); void *user; } SeWorldCb;
static SeWorldCb g_worldcb[16]; static int g_worldcb_n; static void *g_se_world, *g_se_world_gs; static unsigned g_se_world_gen;   // gen: +1 per new world (0 = none yet); modes.inc / mapscript.inc compare it
static int se_timer_add(const AbsApi *self, float seconds, int repeat, void (*cb)(void *), void *user) {      // timer id (> 0) or 0
    int p = plug_of(self), why = TM_OK, id = 0; if (p >= 0) { EnterCriticalSection(&g_lock); id = tm_add(&g_tm, p, seconds, repeat, cb, user, &why); LeaveCriticalSection(&g_lock); }
    if (!id && g_tm_full_logged++ < 20) logf_("plugin %s: timer NOT created (%s; %d of %d slots used, this plugin %d of %d)", plug_name_of(p), p < 0 ? "unknown plugin" : why == TM_FULL ? "table full" : why == TM_PLUGIN_FULL ? "plugin limit" : "bad arguments (seconds > 0 and a callback needed)", g_tm.used, TM_MAX, p >= 0 && p < TM_PLUGINS ? g_tm.per[p] : 0, TM_PER_PLUGIN);
    return id;
}
static int api_p_set_timer(const AbsApi *self, float seconds, int repeat, void (*cb)(void *), void *user) { return se_timer_add(self, seconds, repeat, cb, user) ? 0 : 1; }   // legacy: 0 = ok, 1 = failed
static int api_p_set_timer_id(const AbsApi *self, float seconds, int repeat, void (*cb)(void *), void *user) { return se_timer_add(self, seconds, repeat, cb, user); }
static int api_p_cancel_timer(const AbsApi *self, int id) { int p = plug_of(self), r = 0; if (p >= 0) { EnterCriticalSection(&g_lock); r = tm_cancel(&g_tm, p, id); LeaveCriticalSection(&g_lock); } return r; }
static int api_p_on_world(const AbsApi *self, void (*cb)(const char *, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_worldcb_n >= 16) return 1;
    EnterCriticalSection(&g_lock); g_worldcb[g_worldcb_n].plugin = p; g_worldcb[g_worldcb_n].cb = cb; g_worldcb[g_worldcb_n].user = user; g_worldcb_n++; LeaveCriticalSection(&g_lock); return 0;
}
static void se_poll_keys(void); static void md_tick(float dt); static void ms_tick(float dt); static void as_tick(float dt);   // modes.inc, mapscript.inc, animstyle.inc
static void se_timer_fire(void *ctx, int p, void (*cb)(void *), void *u) {
    (void)ctx; if (!plug_guard(p)) return;
    __try { cb(u); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "timer", GetExceptionCode()); }
}
// New world = a different UWorld pointer OR, same pointer, a different non-null GameState. The engine can free the old world and allocate the next one at the
// SAME address (for example StartMap followed by MainMap_Main), so a pointer-only check would miss the world change and the item / power grants that wait
// for it. lan_telemetry.c compares the game state / game mode the same way.
static void se_world_check(void) {
    void *w = world(), *gs = NULL; if (!w) { g_se_world = NULL; g_se_world_gs = NULL; return; }
    __try { gs = *(void **)((uint8_t *)w + OFF_W_GAMESTATE); } __except (EXCEPTION_EXECUTE_HANDLER) { gs = NULL; }
    if (w == g_se_world && (!gs || gs == g_se_world_gs)) return;
    int reused = w == g_se_world; g_se_world = w; g_se_world_gs = gs; g_se_world_gen++;
    char path[300] = ""; __try { se_path_of(w, path, sizeof path); } __except (EXCEPTION_EXECUTE_HANDLER) { path[0] = 0; }
    logf_("events: world %u %s (world %p, game state %p%s) -> %d on_world_loaded callback(s)", g_se_world_gen, path, w, gs, reused ? ", same world address as before" : "", g_worldcb_n);
    for (int i = 0; i < g_worldcb_n; i++) { int p = g_worldcb[i].plugin; if (!plug_guard(p)) continue; __try { g_worldcb[i].cb(path, g_worldcb[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_world_loaded", GetExceptionCode()); } }
}
static const char *ws_active_kind(void);                          // weaponslot.inc (perf spike log)

// ----- perf recorder (config "perf_stats", default ON: per frame it costs one QueryPerformanceCounter + two array increments (~50 ns, 0.0003% of a 16.7 ms frame), no allocation, no I/O
// except one log block every 30 s). Wall-clock interval between two game ticks = the frame time the player sees (the engine's dt would hide a long present / vsync wait).
// Every 30 s of play and at exit: histogram bands, average and 1% / 0.1% lows, overall and per kind in hand. Pure logic: perfstats.h.
#include "perfstats.h"
#define PF_WINDOW_S 30.0
static int g_pf_on = -1; static PfRec g_pf_win, g_pf_ses; static LARGE_INTEGER g_pf_q, g_pf_last; static double g_pf_win_s, g_pf_kind_s; static unsigned g_pf_nwin; static char g_pf_kind[24] = "-";
static void pf_setup_log(void) {   // the settings that decide what "laggy" means: console_cmd is write-only, so the cvars are not readable live; the config file and the display are
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm; char disp[80] = "unknown";
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) snprintf(disp, sizeof disp, "%lux%lu @ %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    char ini[3000] = ""; wchar_t lad[MAX_PATH], path[MAX_PATH]; DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH); const char *vs = "?", *fl = "?", *fm = "?";
    char vsb[16] = "?", flb[24] = "?", fmb[8] = "?";
    if (got && got < MAX_PATH) { swprintf(path, MAX_PATH, L"%ls\\Absolver\\Saved\\Config\\WindowsNoEditor\\GameUserSettings.ini", lad);
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); DWORD n = 0;
        if (h != INVALID_HANDLE_VALUE) { ReadFile(h, ini, sizeof ini - 1, &n, NULL); CloseHandle(h); ini[n] = 0; }
        const struct { const char *key; char *dst; size_t cap; } want[3] = { { "bUseVSync=", vsb, sizeof vsb }, { "FrameRateLimit=", flb, sizeof flb }, { "FullscreenMode=", fmb, sizeof fmb } };
        for (int i = 0; i < 3; i++) { const char *p = strstr(ini, want[i].key); if (p) { p += strlen(want[i].key); size_t k = 0; while (p[k] && p[k] != '\r' && p[k] != '\n' && k + 1 < want[i].cap) { want[i].dst[k] = p[k]; k++; } want[i].dst[k] = 0; } }
        vs = vsb; fl = flb; fm = fmb; }
    logf_("perf: recorder ON (config perf_stats; 30 s windows + exit). Primary display %s. GameUserSettings.ini: bUseVSync=%s FrameRateLimit=%s FullscreenMode=%s (r.VSync / t.MaxFPS cannot be read back at run time: console_cmd only writes)", disp, vs, fl, fm);
}
static void pf_dump(PfRec *r, const char *head) {
    char line[700], lab[80]; snprintf(lab, sizeof lab, "perf: %s", head); pf_line(&r->all, lab, line, sizeof line); logf_("%s", line);
    for (int i = 0; i < r->nk; i++) if (r->kind[i].n) { snprintf(lab, sizeof lab, "perf:   kind %s", r->kname[i]); pf_line(&r->kind[i], lab, line, sizeof line); logf_("%s", line); }
    if (r->stalls) logf_("perf:   %u stall(s) of 1 s or more (map load / pause) = %.1f s, not counted as frames", r->stalls, r->stall_ms / 1000.0);
}
static void pf_flush(const char *why) {
    if (!g_pf_win.all.n && !g_pf_win.stalls) return; char head[80]; snprintf(head, sizeof head, "%s window #%u (%.0f s wall time)", why, ++g_pf_nwin, g_pf_win_s);
    pf_dump(&g_pf_win, head); pf_rec_merge(&g_pf_ses, &g_pf_win); memset(&g_pf_win, 0, sizeof g_pf_win); g_pf_win_s = 0;
}
static void pf_exit(void) { if (g_pf_on != 1) return; pf_flush("exit"); if (g_pf_ses.all.n) pf_dump(&g_pf_ses, "SESSION total"); }
static int md_console(const char *cmd);                             // modes.inc
static int g_pf_uncap;                                              // config perf_uncap = on (for measurements only, default off): r.VSync 0 + t.MaxFPS 0 once the console exists, so frame cost is measured, not the 60 Hz cap
static void pf_uncap_try(double wall_s) {
    static double next_s; static int tries; if (g_pf_uncap != 1 || wall_s < next_s) return; next_s = wall_s + 2.0;
    if (md_console("r.VSync 0") && md_console("t.MaxFPS 0")) { g_pf_uncap = 2; logf_("perf: perf_uncap done: r.VSync 0, t.MaxFPS 0 (lab runs only; the config file on disk is unchanged)"); }
    else if (++tries >= 30) { g_pf_uncap = -1; logf_("perf: perf_uncap gave up after 30 tries (no player controller)"); }
}
static void pf_frame_tick(void) {
    if (g_pf_on < 0) { char v[16] = ""; wchar_t src[MAX_PATH]; g_pf_on = !(setting_get("perf_stats", L"", 0, v, sizeof v, src) && !_strnicmp(v, "off", 3));
        if (!g_pf_on) { logf_("perf: recorder OFF (config perf_stats = off)"); return; } QueryPerformanceFrequency(&g_pf_q); pf_setup_log(); atexit(pf_exit);
        { char u[16] = ""; wchar_t us[MAX_PATH]; g_pf_uncap = setting_get("perf_uncap", L"", 0, u, sizeof u, us) && !_strnicmp(u, "on", 2); } }
    if (g_pf_on != 1) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now); if (g_pf_last.QuadPart) {
        double ms = (double)(now.QuadPart - g_pf_last.QuadPart) * 1000.0 / (double)g_pf_q.QuadPart; g_pf_win_s += ms / 1000.0; { static double wall; wall += ms / 1000.0; pf_uncap_try(wall); }
        if (g_pf_win_s - g_pf_kind_s > 0.25 || g_pf_win_s < g_pf_kind_s) { g_pf_kind_s = g_pf_win_s; const char *k = ws_active_kind(); snprintf(g_pf_kind, sizeof g_pf_kind, "%s", k ? k : "-"); }   // kind lookup 4x a second, not per frame
        pf_frame(&g_pf_win, ms, g_pf_kind); if (g_pf_win_s >= PF_WINDOW_S) { g_pf_kind_s = 0; pf_flush("30 s"); } }
    g_pf_last = now;
}
static void se_tick(float dt) {
    pf_frame_tick();
    {   // frame-time spikes: the first 60 frames over 40 ms, with the weapon kind in hand
        static int spikes; if (dt > 0.040f && spikes < 60) { spikes++; const char *k = ws_active_kind(); logf_("perf: frame %.0f ms (kind in hand: %s)%s", dt * 1000.f, k ? k : "-", spikes == 60 ? " - no more spike lines" : ""); }
    }
    se_world_check();                                                 // first: modes / mapscript below and timers see the new world generation
    tm_step(&g_tm, dt, se_timer_fire, NULL);
    md_tick(dt); ms_tick(dt); as_tick(dt);
    se_poll_keys();
}

// ----- events: keys, damage; sidecar key/value store
typedef struct { int plugin, vk, down; void (*cb)(int vk, void *); void *user; } SeKeyCb;
static SeKeyCb g_keycb[32]; static int g_keycb_n;
static int api_p_on_key(const AbsApi *self, int vk, void (*cb)(int, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || vk < 1 || vk > 254 || g_keycb_n >= 32) return 1;
    EnterCriticalSection(&g_lock); SeKeyCb *k = &g_keycb[g_keycb_n++]; k->plugin = p; k->vk = vk; k->down = 0; k->cb = cb; k->user = user; LeaveCriticalSection(&g_lock); return 0;
}
static void se_poll_keys(void) {                                  // key-down edges, only while the game window has the focus
    if (!g_keycb_n) return; DWORD pid = 0; HWND fg = GetForegroundWindow(); if (fg) GetWindowThreadProcessId(fg, &pid); int focus = pid == GetCurrentProcessId();
    for (int i = 0; i < g_keycb_n; i++) {
        SeKeyCb *k = &g_keycb[i]; int d = focus && (GetAsyncKeyState(k->vk) & 0x8000) ? 1 : 0; int edge = d && !k->down; k->down = d;
        if (edge && plug_guard(k->plugin)) { __try { k->cb(k->vk, k->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(k->plugin, "on_key", GetExceptionCode()); } }
    }
}
typedef struct { int plugin; void (*cb)(void *victim, float amount, int flag, void *user); void *user; } SeDmgCb;
static SeDmgCb g_dmgcb[16]; static int g_dmgcb_n; static void (*g_orig_dmg)(void *, float, unsigned char);
static void hk_apply_damage(void *th, float amount, unsigned char flag) {      // AFightingCharacter::ApplyDamage(float, bool): victim = this
    g_orig_dmg(th, amount, flag);
    for (int i = 0; i < g_dmgcb_n; i++) { int p = g_dmgcb[i].plugin; if (!plug_guard(p)) continue; __try { g_dmgcb[i].cb(th, amount, flag, g_dmgcb[i].user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(p, "on_damage", GetExceptionCode()); } }
}
static int api_p_on_damage(const AbsApi *self, void (*cb)(void *, float, int, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_dmgcb_n >= 16) return 1;
    if (!g_orig_dmg) { g_orig_dmg = (void (*)(void *, float, unsigned char))api_p_hook_symbol(self, "AFightingCharacter::ApplyDamage", (void *)hk_apply_damage); if (!g_orig_dmg) return 2; }
    EnterCriticalSection(&g_lock); g_dmgcb[g_dmgcb_n].plugin = p; g_dmgcb[g_dmgcb_n].cb = cb; g_dmgcb[g_dmgcb_n].user = user; g_dmgcb_n++; LeaveCriticalSection(&g_lock); return 0;
}
// kv store: LanNative\data\<plugin>.txt, lines "key=value" (value without newlines).
static int se_kv_path(const AbsApi *self, wchar_t *out) {
    int p = plug_of(self); if (p < 0) return 0; wchar_t dir[MAX_PATH], n[100]; swprintf(dir, MAX_PATH, L"%sdata", g_dir); CreateDirectoryW(dir, NULL);
    u8_to_w(plug_name_of(p), n, 100); swprintf(out, MAX_PATH, L"%ls\\%ls.txt", dir, n); return 1;
}
static int api_p_kv_get(const AbsApi *self, const char *key, char *out, int cap) {
    wchar_t path[MAX_PATH]; if (!key || !out || cap < 1 || !se_kv_path(self, path)) return 0; out[0] = 0; FILE *f = _wfopen(path, L"rb"); if (!f) return 0;
    char line[1200]; size_t kl = strlen(key); int found = 0;
    while (fgets(line, sizeof line, f)) { size_t L = strlen(line); while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0; if (!strncmp(line, key, kl) && line[kl] == '=') { snprintf(out, cap, "%s", line + kl + 1); found = 1; } }
    fclose(f); return found;
}
static int api_p_kv_set(const AbsApi *self, const char *key, const char *value) {
    wchar_t path[MAX_PATH]; if (!key || !*key || strpbrk(key, "=\r\n") || !se_kv_path(self, path)) return 1; char buf[16384]; size_t n = 0; buf[0] = 0; size_t kl = strlen(key);
    FILE *f = _wfopen(path, L"rb"); if (f) { char line[1200]; while (fgets(line, sizeof line, f)) { size_t L = strlen(line); while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (L && !(!strncmp(line, key, kl) && line[kl] == '=') && n + L + 3 < sizeof buf) n += snprintf(buf + n, sizeof buf - n, "%s\r\n", line); } fclose(f); }
    if (value) { char v[1100]; snprintf(v, sizeof v, "%s", value); for (char *c = v; *c; c++) if (*c == '\r' || *c == '\n') *c = ' '; n += snprintf(buf + n, sizeof buf - n, "%s=%s\r\n", key, v); }
    f = _wfopen(path, L"wb"); if (!f) return 2; fwrite(buf, 1, n, f); fclose(f); return 0;
}

// ----- text: FText from a UTF-8 string through the reflected UKismetTextLibrary::Conv_StringToText (no native FText constructor needed)
typedef struct { wchar_t *data; int num, max; } SeFString;
static int api_p_make_text(const AbsApi *self, const char *utf8, void *out_ftext24) {
    if (!utf8 || !out_ftext24) return 1; void *cdo = api_p_find_cdo(self, "KismetTextLibrary"), *cls = api_p_find_class(self, "KismetTextLibrary");
    void *fn = cls ? api_p_find_function_in_class(self, cls, "Conv_StringToText") : NULL; if (!cdo || !fn) return 2;
    wchar_t w[1024]; u8_to_w(utf8, w, 1024); SeFString fs = { w, (int)wcslen(w) + 1, 1024 }; void *args[1] = { &fs };
    return api_p_call_params(self, cdo, fn, args, out_ftext24) ? 0 : 3;
}
static int api_p_text_to_string(const AbsApi *self, const void *ftext24, char *out, int cap) {
    if (!ftext24 || !out || cap < 1) return 1; out[0] = 0; void *cdo = api_p_find_cdo(self, "KismetTextLibrary"), *cls = api_p_find_class(self, "KismetTextLibrary");
    void *fn = cls ? api_p_find_function_in_class(self, cls, "Conv_TextToString") : NULL; if (!cdo || !fn) return 2;
    unsigned char copy[24]; memcpy(copy, ftext24, 24); void *args[1] = { copy }; SeFString r = { 0, 0, 0 };
    if (!api_p_call_params(self, cdo, fn, args, &r)) return 3; if (r.data && r.num > 0) WideCharToMultiByte(CP_UTF8, 0, r.data, -1, out, cap, NULL, NULL); return 0;
}
// ----- items foundation (API 2.2): create_asset / add_to_root, DataTable row helpers, inventory, set_text, save events. Engine traps are noted inline.
#define RF_ROOTSET_FLAG 0x40000000u        // EInternalObjectFlags::RootSet in FUObjectItem::Flags (item +8)
static uint32_t se_rva_sym(const char *sym, uint32_t fallback) {       // a symbol from symbols.bin if present, else the 1.31 address
    uint32_t rva = 0, st; uint8_t b[24]; if (se_sym_lookup(sym, &rva, &st, b) && rva) return rva;
    return fallback;
}
static int se_root_flag(void *obj, int on) {
    if (!se_valid_ptr(obj)) return 0; int idx = *(int *)((uint8_t *)obj + 0xC); uint8_t *array = g_base + RVA_GUOBJECTARRAY, *items = *(uint8_t **)(array + 0x10); int count = *(int32_t *)(array + 0x1C);
    if (idx < 0 || idx >= count) return 0;
    volatile LONG *fl = (volatile LONG *)(items + (size_t)idx * 24 + 8); if (on) InterlockedOr(fl, (LONG)RF_ROOTSET_FLAG); else InterlockedAnd(fl, (LONG)~RF_ROOTSET_FLAG); return 1;
}
static int api_p_add_to_root(const AbsApi *self, void *obj, int root) {
    int ok = 0; __try { ok = se_root_flag(obj, root); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    logf_("plugin %s: add_to_root(%p, %d) -> %d", plug_name_of(plug_of(self)), obj, root, ok); return ok;
}
// create_asset: only /Game/Mods/ paths; rooted so the GC keeps it.
static void *api_p_create_asset(const AbsApi *self, void *src, const char *path) {
    const char *who = plug_name_of(plug_of(self)); void *result = NULL;
    if (!src || !path || strncmp(path, "/Game/Mods/", 11) != 0 || strlen(path) > 220) { logf_("plugin %s: create_asset refused: src %p path '%s' (must be under /Game/Mods/)", who, src, path ? path : "(null)"); return NULL; }
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: create_asset refused - not on the game thread", who); return NULL; }
    char pkg[240], nm[120]; const char *dot = strchr(path, '.'), *lastsl = strrchr(path, '/');
    if (dot) { size_t n = (size_t)(dot - path); memcpy(pkg, path, n); pkg[n] = 0; snprintf(nm, sizeof nm, "%s", dot + 1); } else { snprintf(pkg, sizeof pkg, "%s", path); snprintf(nm, sizeof nm, "%s", lastsl + 1); }
    for (const char *c = pkg; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_' || *c == '/' || *c == '-')) { logf_("plugin %s: create_asset refused: bad character in package '%s'", who, pkg); return NULL; }
    if (!*nm || strpbrk(nm, "/.:'\" ")) { logf_("plugin %s: create_asset refused: bad object name '%s'", who, nm); return NULL; }
    char full[400]; snprintf(full, sizeof full, "%s.%s", pkg, nm);
    __try {
        void *srcc = *(void **)((uint8_t *)src + 0x10), *ex = api_p_find_object(self, full);
        if (ex) {
            if (*(void **)((uint8_t *)ex + 0x10) == srcc || (se_class_kind_is(srcc, "Class") && *(void **)((uint8_t *)ex + 0x10) == src)) { se_root_flag(ex, 1); logf_("plugin %s: create_asset %s already exists (%p), reused", who, full, ex); return ex; }
            logf_("plugin %s: create_asset %s exists with another class - refused", who, full); return NULL;
        }
        wchar_t wpkg[240], wnm[120]; u8_to_w(pkg, wpkg, 240); u8_to_w(nm, wnm, 120);
        uint32_t rcp = se_rva_sym("CreatePackage", 0x8BFAC0u), rren = se_rva_sym("UObject::Rename", 0x898190u);
        void *pk = ((void *(*)(void *, const wchar_t *))(g_base + rcp))(NULL, wpkg);
        if (!pk) { logf_("plugin %s: create_asset %s: CreatePackage failed", who, full); return NULL; }
        char sname[100]; se_name_of(src, sname, sizeof sname);
        if (se_class_kind_is(srcc, "Class")) {                                    // src is a UClass: construct a fresh instance named nm inside the package
            FNameRaw f = fname_of(wnm);
            result = ((StaticConstructObject_t)(g_base + RVA_STATIC_CONSTRUCT))(src, pk, *(uint64_t *)&f, 0, 0, NULL, 0, NULL, 0);
        } else {
            void *tp = api_p_find_object(self, "/Engine/Transient"); void *d = api_p_duplicate_object(self, src, tp);
            if (!d) { logf_("plugin %s: create_asset %s: duplicate of %s failed", who, full, sname); return NULL; }
            int ok = ((unsigned char (*)(void *, const wchar_t *, void *, unsigned))(g_base + rren))(d, wnm, pk, 1 | 4 | 16 | 32) != 0;     // REN_ForceNoResetLoaders|DontCreateRedirectors|NonTransactional|DoNotDirty
            if (!ok) { logf_("plugin %s: create_asset %s: Rename into the package failed", who, full); return NULL; }
            result = d;
        }
        if (!result) { logf_("plugin %s: create_asset %s: construction failed", who, full); return NULL; }
        se_root_flag(pk, 1); se_root_flag(result, 1);
        char got[400]; se_path_of(result, got, sizeof got);
        logf_("plugin %s: create_asset %s <- %s: object %p, path now %s, find_object -> %p (rooted)", who, full, sname, result, got, api_p_find_object(self, full));
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: create_asset %s: EXCEPTION 0x%08lX", who, full, GetExceptionCode()); result = NULL; }
    return result;
}

// ----- DataTable rows at runtime (game thread). Same engine paths as the rows.json loader, so row_set_text rejects an object reference that would hang the game instead of loading it
static void *se_table(const char *table_path) { wchar_t w[400]; u8_to_w(table_path, w, 400); return table_find(w); }
static void *api_p_table_row(const AbsApi *self, const char *table_path, const char *row) {
    (void)self; void *r = NULL; if (!table_path || !row) return NULL;
    __try { void *t = se_table(table_path); if (t) { wchar_t w[100]; u8_to_w(row, w, 100); r = table_row(t, fname_of(w)); } } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    return r;
}
static int api_p_table_add_row(const AbsApi *self, const char *table_path, const char *row, const char *copy_of) {
    const char *who = plug_name_of(plug_of(self)); int ok = 0; char why[200] = "";
    if (!table_path || !row || !copy_of || !rows_name_ok(row, 64) || !rows_name_ok(copy_of, 64)) { logf_("plugin %s: table_add_row: bad arguments", who); return 0; }
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: table_add_row refused - not on the game thread", who); return 0; }
    __try { void *t = se_table(table_path); if (!t) snprintf(why, sizeof why, "table not loaded"); else { RowOp op; memset(&op, 0, sizeof op); op.is_add = 1; snprintf(op.row, sizeof op.row, "%s", row); snprintf(op.src, sizeof op.src, "%s", copy_of); ok = row_add_q(t, &op, why, sizeof why); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(why, sizeof why, "EXCEPTION 0x%08lX", GetExceptionCode()); ok = 0; }
    logf_("plugin %s: table_add_row %s %s (copy of %s) -> %d %s", who, table_path, row, copy_of, ok, ok ? "" : why); return ok;
}
static int api_p_row_set_text(const AbsApi *self, const char *table_path, const char *row, const char *prop, const char *text) {
    const char *who = plug_name_of(plug_of(self)); int ok = 0; char why[260] = "";
    if (!table_path || !row || !prop || !text || !rows_name_ok(row, 64) || !rows_name_ok(prop, 64) || strlen(text) > 390) { logf_("plugin %s: row_set_text: bad arguments", who); return 0; }
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: row_set_text refused - not on the game thread", who); return 0; }
    __try { void *t = se_table(table_path); if (!t) snprintf(why, sizeof why, "table not loaded"); else { RowOp op; memset(&op, 0, sizeof op); snprintf(op.row, sizeof op.row, "%s", row); snprintf(op.prop, sizeof op.prop, "%s", prop); op.text = (char *)text; ok = row_set_q(t, &op, why, sizeof why); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(why, sizeof why, "EXCEPTION 0x%08lX", GetExceptionCode()); ok = 0; }
    logf_("plugin %s: row_set_text %s %s.%s = '%s' -> %d %s", who, table_path, row, prop, text, ok, ok ? "" : why); return ok;
}

// ----- set_text: a Text property from a UTF-8 string. The old FText's reference is NOT released (a few bytes leak per call; releasing a text shared with the stock data would be worse).
static int api_p_set_text_prop(const AbsApi *self, void *obj, const char *prop, const char *utf8) {
    unsigned char t[24]; int rc = api_p_make_text(self, utf8, t); void *p = obj && prop ? api_p_prop_ptr(self, obj, prop) : NULL;
    if (rc || !p) { logf_("plugin %s: set_text %s: make_text %d, property %p - not set", plug_name_of(plug_of(self)), prop ? prop : "?", rc, p); return 0; }
    memcpy(p, t, 24); return 1;
}

// ----- inventory (the local player's UInventoryComponent). FEquipmentStruct (0x70): item +0, m_eSchoolItemType +8 (4 = None; 0 = Item1 makes the item vanish on the next load), material index +0xC.
typedef struct { void *data; int num, max; } SeTArr;
static void *se_inventory(const AbsApi *self) {                          // the local pawn (Controller::K2_GetPawn) -> BaseCharacter::m_InventoryComponent (+0xFC0)
    void *pc = api_p_local_pc(self), *pawn = NULL, *ic = NULL, *f = pc ? api_p_find_function(self, pc, "K2_GetPawn") : NULL;
    if (f) api_p_call_params(self, pc, f, NULL, &pawn);
    void **slot = pawn ? (void **)api_p_prop_ptr(self, pawn, "m_InventoryComponent") : NULL;
    __try { if (slot) ic = *slot; } __except (EXCEPTION_EXECUTE_HANDLER) { ic = NULL; }
    if (!ic) logf_("plugin %s: inventory component not available: player controller %p, pawn %p, m_InventoryComponent slot %p", plug_name_of(plug_of(self)), pc, pawn, (void *)slot);
    return ic;
}
#include "grantpoll.h"
static int se_inventory_probe(const AbsApi *self, int *count) {          // quiet (no log; polled): GP_OK + item count, or why the inventory is not reachable
    void *pc = api_p_local_pc(self), *pawn = NULL, *ic = NULL, *f = pc ? api_p_find_function(self, pc, "K2_GetPawn") : NULL; *count = 0;
    if (!pc || !f) return GP_NO_PC;
    api_p_call_params(self, pc, f, NULL, &pawn); if (!pawn) return GP_NO_PAWN;
    void **slot = (void **)api_p_prop_ptr(self, pawn, "m_InventoryComponent");
    __try { if (slot) ic = *slot; if (ic) *count = ((SeTArr *)((uint8_t *)ic + 0xA50))->num; } __except (EXCEPTION_EXECUTE_HANDLER) { ic = NULL; }
    return ic ? GP_OK : GP_NO_INV;
}
static int api_p_inventory_index(const AbsApi *self, void *item) {
    int r = -1; void *ic = se_inventory(self); if (!ic || !item) return -1;
    __try { SeTArr *inv = (SeTArr *)((uint8_t *)ic + 0xA50); for (int i = 0; i < inv->num; i++) if (*(void **)((uint8_t *)inv->data + (size_t)i * 0x88) == item) { r = i; break; } } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
static void se_menu_visit(void *o, void *ctx) {
    void **r = ctx; if (*r || !se_class_kind_is(*(void **)((uint8_t *)o + 0x10), "InventoryMenu")) return;
    char p[400]; se_path_of(o, p, sizeof p); if (strncmp(p, "/Game/", 6) && !strstr(p, "Default__")) *r = o;
}
static int api_p_inventory_equip(const AbsApi *self, int index) {
    const char *who = plug_name_of(plug_of(self)); void *ic = se_inventory(self); int ok = 0;
    if (!ic || index < 0) { logf_("plugin %s: inventory_equip(%d): no inventory component / bad index", who, index); return 0; }
    void *menu = NULL; api_scan_objects(se_menu_visit, &menu);
    if (menu) {                                                                // preferred route: InventoryMenu::BPF_Equip(index, slot 0) sets the equipped flags AND the preset (BPF_EquipOutfit alone desyncs the menu)
        void *mc = se_find_class("InventoryMenu"), *f = mc ? se_class_find_function(mc, "BPF_Equip") : NULL; int slot = 0; void *args[2] = { &index, &slot };
        ok = f && api_p_call_params(self, menu, f, args, NULL);
        logf_("plugin %s: inventory_equip(%d) via InventoryMenu::BPF_Equip on menu %p -> %d", who, index, menu, ok);
    } else {                                                                   // no menu object yet: the component's own native (index, slot -1 = automatic, bool, bool)
        uint32_t rva = se_rva_sym("UInventoryComponent::EquipFromInventory", 0x3FE310u);
        __try { ((void (*)(void *, int, int, unsigned char, unsigned char))(g_base + rva))(ic, index, -1, 1, 1); ok = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: inventory_equip: EXCEPTION 0x%08lX in EquipFromInventory", who, GetExceptionCode()); ok = 0; }
        logf_("plugin %s: inventory_equip(%d): no InventoryMenu object, used UInventoryComponent::EquipFromInventory(+0x%X) -> %d", who, index, rva, ok);
    }
    return ok;
}
static int api_p_inventory_add(const AbsApi *self, void *item, int material, int equip) {
    const char *who = plug_name_of(plug_of(self)); void *ic = se_inventory(self), *icls = se_find_class("InventoryComponent"); int ret = -1;
    if (!item || !ic || !icls) { logf_("plugin %s: inventory_add: item %p, inventory %p, class %p - not added", who, item, ic, icls); return -1; }
    unsigned char es[0x70]; memset(es, 0, sizeof es); *(void **)es = item; es[8] = 4; *(int *)(es + 0xC) = material < 0 ? 0 : material;
    unsigned char eq = 0, cs = 0; int cnt = 1; void *f = se_class_find_function(icls, "BPF_AddItemToInventory"), *args[4] = { es, &eq, &cnt, &cs };
    int ok = f && api_p_call_params(self, ic, f, args, &ret);
    char p[300]; se_path_of(item, p, sizeof p); logf_("plugin %s: inventory_add(%s, material %d, equip %d) -> call %d, index %d", who, p, material, equip, ok, ret);
    if (!ok) return -1;
    if (equip && ret >= 0) api_p_inventory_equip(self, ret);
    return ret;
}

// ----- save events: before the save object's item paths are resolved (item mods must have created their objects by now) and after the item paths were written (sidecars)
typedef struct { int plugin; int written; void (*cb)(void *save, void *user); void *user; } SeSaveCb;
static SeSaveCb g_savecb[16]; static int g_savecb_n; static void (*g_o_initinv)(void *); static void (*g_o_ser)(void *, void *, unsigned char);
static void se_fire_save(int written, void *save) {
    for (int i = 0; i < g_savecb_n; i++) { SeSaveCb *c = &g_savecb[i]; if (c->written != written || !plug_guard(c->plugin)) continue; __try { c->cb(save, c->user); } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(c->plugin, written ? "on_save_written" : "on_save_loading", GetExceptionCode()); } }
}
static void hk_initinv(void *save) { logf_("savecb: USaveThePlaines::InitializeInventory(%p)", save); se_fire_save(0, save); g_o_initinv(save); }
static void hk_serinv(void *save, void *arr, unsigned char dep) { g_o_ser(save, arr, dep); se_fire_save(1, save); }
static int se_save_reg(const AbsApi *self, int written, void (*cb)(void *, void *), void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_savecb_n >= 16) return 1;
    if (!written && !g_o_initinv) { g_o_initinv = (void (*)(void *))api_p_hook_symbol(self, "USaveThePlaines::InitializeInventory", (void *)hk_initinv); if (!g_o_initinv) return 2; }
    if (written && !g_o_ser) { g_o_ser = (void (*)(void *, void *, unsigned char))api_p_hook_symbol(self, "USaveThePlaines::InitInventoryDataToSerializeFromInventoryArray", (void *)hk_serinv); if (!g_o_ser) return 2; }
    EnterCriticalSection(&g_lock); g_savecb[g_savecb_n].plugin = p; g_savecb[g_savecb_n].written = written; g_savecb[g_savecb_n].cb = cb; g_savecb[g_savecb_n].user = user; g_savecb_n++; LeaveCriticalSection(&g_lock);
    logf_("plugin %s: on_save_%s registered", plug_name_of(p), written ? "written" : "loading"); return 0;
}
static int api_p_on_save_loading(const AbsApi *self, void (*cb)(void *, void *), void *user) { return se_save_reg(self, 0, cb, user); }
static int api_p_on_save_written(const AbsApi *self, void (*cb)(void *, void *), void *user) { return se_save_reg(self, 1, cb, user); }

// ----- events v2.3: on_hit, on_attack_start, on_pawn_spawned/destroyed, on_death, on_menu; char_info. Hook points (docs\PLUGIN-API.md "Events (API 2.3)").
//   UHitComponent::GenerateImpact(victim's hit component, attacker, FName attack, ?, bool perfectLink, FHitResult*, FHitRequest*, out float damage, out bool guarded, out bool guardBroken, out bool absorbed)
//     = the authority's hit resolution (applies the damage through AFightingCharacter::ApplyDamage); UDefenseComponent::NotifyParrySuccessful / NotifyAvoidSuccessful for parry / dodge
//   OrderAttack::OnStart(order): runs ~30x per attack (prediction re-sims on copies) -> deduped on (owner +0x30, attack FName at *(order +0x48) +0x10, m_iOriginalLaunchTime +0x20) (evdedupe.h)
//   AFightingCharacter::BeginPlay / EndPlay(reason), AFightingCharacter::ServerKilledBy(victim, killer, FName attack), menus: any "EnterState*" UFunction on a UserWidget (se_enter flags it)
#include "evdedupe.h"
enum { EV_HIT, EV_ATTACK, EV_SPAWN, EV_DESTROY, EV_DEATH, EV_MENU, EV_KINDS };
typedef struct { int plugin; void *cb; void *user; } SeEvCb;
static SeEvCb g_ev[EV_KINDS][16]; static int g_ev_n[EV_KINDS];
static const char *const g_ev_name[EV_KINDS] = { "on_hit", "on_attack_start", "on_pawn_spawned", "on_pawn_destroyed", "on_death", "on_menu" };
// every callback of one event kind, each in its own guard (CALL uses c_->cb / c_->user)
#define SE_FIRE(kind, CALL) for (int i_ = 0; i_ < g_ev_n[kind]; i_++) { SeEvCb *c_ = &g_ev[kind][i_]; if (!plug_guard(c_->plugin)) continue; __try { CALL; } __except (EXCEPTION_EXECUTE_HANDLER) { plug_fault(c_->plugin, g_ev_name[kind], GetExceptionCode()); } }
static int se_ev_add(const AbsApi *self, int kind, void *cb, void *user) {
    int p = plug_of(self); if (p < 0 || !cb || g_ev_n[kind] >= 16) return 1;
    EnterCriticalSection(&g_lock); SeEvCb *c = &g_ev[kind][g_ev_n[kind]]; c->plugin = p; c->cb = cb; c->user = user; g_ev_n[kind]++; LeaveCriticalSection(&g_lock);
    logf_("plugin %s: %s registered", plug_name_of(p), g_ev_name[kind]); return 0;
}
static int se_attack_name(uint64_t fn, char *out, int cap) {               // attack row FName -> text and numeric id (-1 if not a number)
    out[0] = 0; if (!fn) return -1; fname_text(*(FNameRaw *)&fn, out, cap);
    if (!out[0] || strlen(out) > 9) return -1; for (const char *c = out; *c; c++) if (*c < '0' || *c > '9') return -1; return atoi(out);
}
static void *se_owner(void *component) { return se_valid_ptr(component) ? *(void **)((uint8_t *)component + 0x20) : NULL; }   // a component's Outer = its actor (default subobject)
static volatile LONG g_ev_logged;                                           // the first events of each run are logged in full (for diagnosis), then only passed on
static int se_ev_log_ok(void) { return InterlockedIncrement(&g_ev_logged) <= 150; }

// on_hit
typedef void (*GenImpact_t)(void *, void *, uint64_t, uint64_t, uint64_t, void *, void *, float *, uint8_t *, uint8_t *, uint8_t *);
static GenImpact_t g_o_genimpact;
static void se_fire_hit(AbsHitInfo *h) {
    if (se_ev_log_ok()) { char v[100], a[100]; se_name_of(h->victim, v, sizeof v); se_name_of(h->attacker, a, sizeof a); logf_("events: hit victim=%s attacker=%s attack=%s (%d) damage=%.2f outcome=%d perfect=%d", v, a, h->attack, h->attack_id, h->damage, h->outcome, h->perfect_link); }
    SE_FIRE(EV_HIT, ((void (*)(const AbsHitInfo *, void *))c_->cb)(h, c_->user));
}
static void hk_genimpact(void *hc, void *attacker, uint64_t attack, uint64_t p3, uint64_t perfect, void *hitres, void *hitreq, float *dmg, uint8_t *guarded, uint8_t *broken, uint8_t *absorbed) {
    g_o_genimpact(hc, attacker, attack, p3, perfect, hitres, hitreq, dmg, guarded, broken, absorbed);
    __try {
        AbsHitInfo h; memset(&h, 0, sizeof h); h.victim = se_owner(hc); h.attacker = attacker; h.attack_id = se_attack_name(attack, h.attack, sizeof h.attack);
        h.damage = dmg ? *dmg : 0; h.perfect_link = (uint8_t)perfect != 0;
        h.outcome = broken && *broken ? ABS_HIT_GUARD_BROKEN : absorbed && *absorbed ? ABS_HIT_ABSORBED : guarded && *guarded ? ABS_HIT_GUARDED : h.damage > 0 ? ABS_HIT : ABS_HIT_NO_EFFECT;
        se_fire_hit(&h);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("events: EXCEPTION 0x%08lX reading a GenerateImpact result", GetExceptionCode()); }
}
typedef void (*Parry_t)(void *, float, void *, uint64_t, uint64_t);
typedef void (*Avoid_t)(void *, float, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void *);
static Parry_t g_o_parry; static Avoid_t g_o_avoid;
static void se_defense_event(void *dc, void *attacker, uint64_t attack, int outcome) {
    __try { AbsHitInfo h; memset(&h, 0, sizeof h); h.victim = se_owner(dc); h.attacker = attacker;
            if (outcome == ABS_HIT_PARRIED) ev_parry_roles(se_owner(dc), attacker, &h.victim, &h.attacker);
            h.attack_id = se_attack_name(attack, h.attack, sizeof h.attack); h.outcome = outcome; se_fire_hit(&h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf_("events: EXCEPTION 0x%08lX in a parry / dodge event", GetExceptionCode()); }
}
static void hk_parry(void *dc, float delay, void *attacker, uint64_t attack, uint64_t order) { g_o_parry(dc, delay, attacker, attack, order); se_defense_event(dc, attacker, attack, ABS_HIT_PARRIED); }
static void hk_avoid(void *dc, float delay, void *attacker, uint64_t type, uint64_t otype, uint64_t order, uint64_t attack, uint64_t exec, void *db) { g_o_avoid(dc, delay, attacker, type, otype, order, attack, exec, db); se_defense_event(dc, attacker, attack, ABS_HIT_DODGED); }

// on_attack_start
static void (*g_o_atkstart)(void *); static EvDedupe g_atk_dd; static volatile LONG g_atk_raw;
static void hk_atkstart(void *order) {
    g_o_atkstart(order);
    __try {
        uint8_t *o = order; void *owner = *(void **)(o + 0x30); uint8_t *net = *(uint8_t **)(o + 0x48); uint64_t fn = se_valid_ptr(net) ? *(uint64_t *)(net + 0x10) : 0, launch = *(uint64_t *)(o + 0x20);
        LONG raw = InterlockedIncrement(&g_atk_raw);
        if (raw <= 40) { char n[64]; se_attack_name(fn, n, sizeof n); logf_("events: OrderAttack::OnStart raw #%ld owner=%p attack=%s launch=%lld saved=%lld inPrediction=%d", (long)raw, owner, n, (long long)launch, (long long)*(int64_t *)(o + 0x28), o[0x100]); }
        if (!fn || !se_valid_ptr(owner) || evd_seen(&g_atk_dd, evd_key((uint64_t)(uintptr_t)owner, fn, launch))) return;
        char n[32]; int id = se_attack_name(fn, n, sizeof n);
        if (se_ev_log_ok()) { char c[100]; se_name_of(owner, c, sizeof c); logf_("events: attack start %s attack=%s (%d) (raw OnStart calls so far %ld)", c, n, id, (long)raw); }
        SE_FIRE(EV_ATTACK, ((void (*)(void *, int, const char *, void *))c_->cb)(owner, id, n, c_->user));
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("events: EXCEPTION 0x%08lX in OrderAttack::OnStart", GetExceptionCode()); }
}

// on_pawn_spawned / on_pawn_destroyed / on_death
static void (*g_o_beginplay)(void *); static void (*g_o_endplay)(void *, uint64_t); static void (*g_o_killed)(void *, void *, uint64_t);
static void hk_beginplay(void *ch) {
    g_o_beginplay(ch);
    if (se_ev_log_ok()) { char p[300]; se_path_of(ch, p, sizeof p); logf_("events: pawn spawned %p %s", ch, p); }
    SE_FIRE(EV_SPAWN, ((void (*)(void *, void *))c_->cb)(ch, c_->user));
}
static void hk_endplay(void *ch, uint64_t reason) {
    if (se_ev_log_ok()) { char p[300]; se_path_of(ch, p, sizeof p); logf_("events: pawn destroyed %p %s reason=%d", ch, p, (int)(uint8_t)reason); }
    SE_FIRE(EV_DESTROY, ((void (*)(void *, int, void *))c_->cb)(ch, (int)(uint8_t)reason, c_->user));
    g_o_endplay(ch, reason);
}
static void hk_killed(void *victim, void *killer, uint64_t attack) {
    char n[32] = ""; int id = -1; __try { id = se_attack_name(attack, n, sizeof n); } __except (EXCEPTION_EXECUTE_HANDLER) { n[0] = 0; }
    { char v[100], k[100]; se_name_of(victim, v, sizeof v); se_name_of(killer, k, sizeof k); logf_("events: death victim=%s killer=%s attack=%s (%d)", v, k[0] ? k : "-", n, id); }
    g_o_killed(victim, killer, attack);
    SE_FIRE(EV_DEATH, ((void (*)(void *, void *, int, const char *, void *))c_->cb)(victim, killer, id, n, c_->user));
}

// on_menu: se_enter flags any UFunction named "EnterState..." (cached per FName); fired after the call when the object is a UserWidget
static struct { uint64_t fn; int yes; } g_es_cache[4096];
static int se_is_enter_state(uint64_t fn) {
    unsigned i = (unsigned)((fn * 0x9E3779B97F4A7C15ull) >> 52);
    if (fn && g_es_cache[i].fn == fn) return g_es_cache[i].yes;
    char n[100]; fname_text(*(FNameRaw *)&fn, n, sizeof n); int yes = !strncmp(n, "EnterState", 10) && n[10];
    g_es_cache[i].fn = 0; g_es_cache[i].yes = yes; g_es_cache[i].fn = fn; return yes;
}
static void se_menu_fire(void *obj, void *func) {
    char n[100], cls[100]; const char *st;
    __try {
        if (!se_valid_ptr(obj) || !se_class_kind_is(*(void **)((uint8_t *)obj + 0x10), "UserWidget")) return;
        se_name_of(func, n, sizeof n); st = n + 10; while (*st == ' ' || *st == '_') st++;
        se_name_of(*(void **)((uint8_t *)obj + 0x10), cls, sizeof cls);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (se_ev_log_ok()) logf_("events: menu %s %p state %s", cls, obj, st);
    SE_FIRE(EV_MENU, ((void (*)(void *, const char *, const char *, void *))c_->cb)(obj, cls, st, c_->user));
}

// registration: the hook behind an event is installed the first time a plugin asks for it
static int api_p_on_hit(const AbsApi *self, void (*cb)(const AbsHitInfo *, void *), void *user) {
    if (!g_o_genimpact && !(g_o_genimpact = (GenImpact_t)api_p_hook_symbol(self, "UHitComponent::GenerateImpact", (void *)hk_genimpact))) return 2;
    if (!g_o_parry) g_o_parry = (Parry_t)api_p_hook_symbol(self, "UDefenseComponent::NotifyParrySuccessful", (void *)hk_parry);       // optional: on_hit works without them
    if (!g_o_avoid) g_o_avoid = (Avoid_t)api_p_hook_symbol(self, "UDefenseComponent::NotifyAvoidSuccessful", (void *)hk_avoid);
    return se_ev_add(self, EV_HIT, (void *)cb, user);
}
static int api_p_on_attack_start(const AbsApi *self, void (*cb)(void *, int, const char *, void *), void *user) {
    if (!g_o_atkstart && !(g_o_atkstart = (void (*)(void *))api_p_hook_symbol(self, "OrderAttack::OnStart", (void *)hk_atkstart))) return 2;
    return se_ev_add(self, EV_ATTACK, (void *)cb, user);
}
static int api_p_on_pawn_spawned(const AbsApi *self, void (*cb)(void *, void *), void *user) {
    if (!g_o_beginplay && !(g_o_beginplay = (void (*)(void *))api_p_hook_symbol(self, "AFightingCharacter::BeginPlay", (void *)hk_beginplay))) return 2;
    return se_ev_add(self, EV_SPAWN, (void *)cb, user);
}
static int api_p_on_pawn_destroyed(const AbsApi *self, void (*cb)(void *, int, void *), void *user) {
    if (!g_o_endplay && !(g_o_endplay = (void (*)(void *, uint64_t))api_p_hook_symbol(self, "AFightingCharacter::EndPlay", (void *)hk_endplay))) return 2;
    return se_ev_add(self, EV_DESTROY, (void *)cb, user);
}
static int api_p_on_death(const AbsApi *self, void (*cb)(void *, void *, int, const char *, void *), void *user) {
    if (!g_o_killed && !(g_o_killed = (void (*)(void *, void *, uint64_t))api_p_hook_symbol(self, "AFightingCharacter::ServerKilledBy", (void *)hk_killed))) return 2;
    return se_ev_add(self, EV_DEATH, (void *)cb, user);
}
static int api_p_on_menu(const AbsApi *self, void (*cb)(void *, const char *, const char *, void *), void *user) {
    if (!g_orig_pe) return 2; int rc = se_ev_add(self, EV_MENU, (void *)cb, user); if (!rc) { g_menu_n = g_ev_n[EV_MENU]; se_pe_flag(); } return rc;
}

// char_info: component pointers and values by property name (SDK dump offsets for reference: FightingCharacter +0x1BE8 m_DefenseComponent, +0x1C00 m_AnimationGraph,
// +0x1C50 m_HealthComponent, +0x1C60 m_StatsComponent; HealthComponent +0xF4 m_fHealth, +0x2B0 m_fMaxHealth, +0x2B4 m_fBonusMaxHealth; DefenseComponent +0x110 m_fCurrentGuard,
// +0x170 m_fMaxGuard; PlayerAnim +0x1437 m_eCurrentQuadrant). Looked up by name at run time, so a missing property reads as 0 / -1 instead of garbage
static void *se_comp(const AbsApi *self, void *ch, const char *prop) { void **pp = api_p_prop_ptr(self, ch, prop); return pp && se_valid_ptr(*pp) ? *pp : NULL; }
static float se_prop_f(const AbsApi *self, void *o, const char *prop) { float *f = o ? api_p_prop_ptr(self, o, prop) : NULL; return f ? *f : 0.0f; }
static int tk_invincible(const AbsApi *self, void *ch);          // testkit.inc
static int api_p_char_info(const AbsApi *self, void *ch, AbsCharInfo *out) {
    if (!se_valid_ptr(ch) || !out || out->size < sizeof(uint32_t)) return 0;
    AbsCharInfo r; memset(&r, 0, sizeof r); r.size = out->size; r.stance = -1; r.style = -1; r.invincible = -1; r.attacking = -1; int ok = 0;
    __try {
        se_name_of(ch, r.name, sizeof r.name); se_path_of(ch, r.path, sizeof r.path);
        void *hc = se_comp(self, ch, "m_HealthComponent"), *dc = se_comp(self, ch, "m_DefenseComponent"), *an = se_comp(self, ch, "m_AnimationGraph"), *sc = se_comp(self, ch, "m_StatsComponent"), *ctl = se_comp(self, ch, "Controller");
        r.health = se_prop_f(self, hc, "m_fHealth"); r.max_health = se_prop_f(self, hc, "m_fMaxHealth") + se_prop_f(self, hc, "m_fBonusMaxHealth");
        r.guard = se_prop_f(self, dc, "m_fCurrentGuard"); r.max_guard = se_prop_f(self, dc, "m_fMaxGuard");
        uint8_t *q = an ? api_p_prop_ptr(self, an, "m_eCurrentQuadrant") : NULL; if (q && *q < 4) r.stance = *q;
        r.is_player = ctl && se_class_kind_is(*(void **)((uint8_t *)ctl + 0x10), "PlayerController");
        r.is_dead = hc && r.health <= 0.0f;
        { int b = 0; r.invincible = tk_invincible(self, ch); if (hc && api_p_get_bool(self, hc, "m_bIsDown", &b)) r.is_down = b; if (dc && api_p_get_bool(self, dc, "m_bIsGuarding", &b)) r.guarding = b; }
        { void *ac = se_comp(self, ch, "m_AttackComponent"); if (ac && api_p_is_game_thread(self)) { void *f = api_p_find_function(self, ac, "BPF_IsPlayingAttack"); unsigned char pl = 0; if (f && api_p_call_params(self, ac, f, NULL, &pl)) r.attacking = pl != 0; } }
        if (sc && api_p_is_game_thread(self)) { void *f = api_p_find_function(self, sc, "BPF_GetFightingStyleID"); uint8_t mode = 0; int st = -1; void *args[1] = { &mode }; if (f && api_p_call_params(self, sc, f, args, &st)) r.style = st; }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: char_info(%p): EXCEPTION 0x%08lX", plug_name_of(plug_of(self)), ch, GetExceptionCode()); ok = 0; }
    memcpy(out, &r, out->size < sizeof r ? out->size : sizeof r); return ok;
}

#include "testkit.inc"
static int api_p_add_menu_item(const AbsApi *self, const char *label, void (*cb)(void *), void *user);       // modsui.inc
static int api_p_reload_content(const AbsApi *self);                                                         // modsui.inc
static int api_p_inventory_equip_slot(const AbsApi *self, int index, int slot); static int api_p_on_power_used(const AbsApi *self, void (*cb)(void *, void *, void *, int, void *), void *user); static int api_p_power_use(const AbsApi *self, void *character, int dir);   // powers.inc (included by content.inc)
#include "texture.inc"
#include "combodeck.inc"
#include "sidecar.inc"                                                  // API 2.18 per-character metadata store (LanNative\state\<GUID>.txt), used by weaponslot / kindstate
#include "animstyle.inc"                                                // API 2.17 weapon animation sets (another style's animations without a style change)
#include "modes.inc"
static void np_fill_api(AbsApi *a);                                                                       // npcs.inc (included by content.inc)
static void bt_fill_api(AbsApi *a);                                                                       // bots.inc (included by content.inc)
static void hp_fill_api(AbsApi *a);                                                                       // hitpre.inc (included by content.inc)
static void ov_fill_api(AbsApi *a);                                                                       // overlayapi.inc (included by content.inc)
static void de_fill_api(AbsApi *a);                                                                       // defense_ev.inc (included by content.inc)
#include "mapscript.inc"
// ----- wiring
static void se_fill_api(AbsApi *a) {
    a->create_texture = api_p_create_texture; a->texture_from_file = api_p_texture_from_file;
    a->test_heal = api_p_test_heal; a->test_set_invincible = api_p_test_set_invincible; a->test_attack = api_p_test_attack; a->test_guard = api_p_test_guard; a->test_teleport = api_p_test_teleport; a->test_get_location = api_p_test_get_location; a->test_spawn_npc = api_p_test_spawn_npc;
    a->add_menu_item = api_p_add_menu_item; a->reload_content = api_p_reload_content; a->test_dodge = api_p_test_dodge; a->test_ai = api_p_test_ai; a->test_hittable = api_p_test_hittable; a->test_get_forward = api_p_test_get_forward;
    a->hook_symbol = api_p_hook_symbol; a->find_symbol = api_p_find_symbol; a->find_class = api_p_find_class; a->find_function_in_class = api_p_find_function_in_class;
    a->on_call = api_p_on_call; a->trace = api_p_trace; a->replace_native = api_p_replace_native; a->prop_ptr = api_p_prop_ptr; a->get_bool = api_p_get_bool; a->set_bool = api_p_set_bool;
    a->call_function_params = api_p_call_params; a->construct_object = api_p_construct_object; a->spawn_actor = api_p_spawn_actor; a->get_world = api_p_get_world;
    a->frame_peek_int = api_p_frame_peek_int; a->object_path = api_p_object_path; a->dump_object = api_p_dump_object; a->trace_function = api_p_trace_function; a->dump_sdk = api_p_dump_sdk; a->set_timer = api_p_set_timer; a->on_world_loaded = api_p_on_world;
    a->duplicate_object = api_p_duplicate_object; a->find_cdo = api_p_find_cdo; a->create_widget = api_p_create_widget; a->add_to_viewport = api_p_add_to_viewport; a->remove_from_parent = api_p_remove_from_parent; a->get_local_player_controller = api_p_local_pc;
    a->make_text = api_p_make_text; a->text_to_string = api_p_text_to_string; a->on_key = api_p_on_key; a->on_damage = api_p_on_damage; a->kv_get = api_p_kv_get; a->kv_set = api_p_kv_set;
    a->create_asset = api_p_create_asset; a->add_to_root = api_p_add_to_root; a->table_row = api_p_table_row; a->table_add_row = api_p_table_add_row; a->row_set_text = api_p_row_set_text;
    a->inventory_add = api_p_inventory_add; a->inventory_equip = api_p_inventory_equip; a->inventory_index = api_p_inventory_index; a->set_text = api_p_set_text_prop; a->on_save_loading = api_p_on_save_loading; a->on_save_written = api_p_on_save_written;
    a->on_hit = api_p_on_hit; a->on_attack_start = api_p_on_attack_start; a->on_pawn_spawned = api_p_on_pawn_spawned; a->on_pawn_destroyed = api_p_on_pawn_destroyed; a->on_death = api_p_on_death; a->on_menu = api_p_on_menu; a->char_info = api_p_char_info; a->register_content_kind = api_p_register_content_kind;
    a->inventory_equip_slot = api_p_inventory_equip_slot; a->on_power_used = api_p_on_power_used; a->power_use = api_p_power_use;
    a->combo_slot_ensure = api_p_combo_slot_ensure; a->combo_load = api_p_combo_load; a->combo_current = api_p_combo_current; a->set_combat_style = api_p_set_combat_style; a->on_weapon_changed = api_p_on_weapon_changed; a->on_style_changed = api_p_on_style_changed; a->prop_ptr_path = api_p_prop_ptr_path; a->fname_make = api_p_fname_make; a->fname_text = api_p_fname_text;
    md_fill_api(a); ms_fill_api(a); np_fill_api(a); bt_fill_api(a); as_fill_api(a); sc_fill_api(a); hp_fill_api(a); de_fill_api(a); ov_fill_api(a);
}
static void se_install(void) {                                    // called once from init(): the ProcessEvent hook is always installed (fast path when nothing is registered)
    g_orig_pe = (PE_t)hook(IDX_PROCESSEVENT, (void *)hk_pe);
    g_orig_pi = (PI_t)hook(IDX_PROCESSINTERNAL, (void *)hk_pi);
    logf_("script extender: ProcessEvent hook %s, ProcessInternal hook %s", g_orig_pe ? "installed" : "NOT installed (on_call and the tracer are off)", g_orig_pi ? "installed" : "NOT installed (Blueprint-to-Blueprint calls are not seen)");
}

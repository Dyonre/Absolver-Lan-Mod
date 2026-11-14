// Row edits (included by lannative.c after lanmods.inc). Mods ship rows.json (format and parser: rowsjson.h); the mod loader turns every enabled mod's file into RowOps in
// load order (g_rowops). This file applies them: a hook on UDataTable::Serialize re-applies the ops of a table right after it (re)loads. Engine facts:
//   edit a field : UProperty::ImportText(prop, text, row + Offset_Internal, 0, NULL, GWarn) in place; soft object paths are written directly (ImportText clears unloaded assets)
//   add a row    : Malloc(aligned struct size), InitializeStruct, CopyScriptStruct from the source row, RowMap.Emplace(this, &id, &{&name,&ptr}, &already)
//   values are validated first (rowval_check): ImportText turns garbage numbers into 0 and still says "ok"
// Results: LanNative\rows.txt (per mod: applied / failed, first failures). F11 commands for development (LanNative\lmtest.txt, see lanmods.inc):
//   tbl | rowget | rowset | rowprops | rowprobe | rowapply <objpath> | objget | objset | rowdump <objpath> <first> <last> (-> rowdump.json) | rowfull <objpath> <first> <last> (-> rowfull.txt)
#define RVA_FNAME_CTOR_W    0x72E3C0u     // FName::FName(FName *out, const wchar_t *, EFindName)   (FNAME_Add = 1)
#define RVA_STATICFINDOBJ   0x8D0A50u     // StaticFindObject(UClass *, UObject *outer, const wchar_t *name, bool exact)
#define RVA_CLS_DATATABLE   0x183CAA0u    // UDataTable::GetPrivateStaticClass
#define RVA_CLS_ATTACKCOMP  0x567260u     // UAttackComponent::GetPrivateStaticClass
#define RVA_ATTACKDATA_BYNAME 0x2F6960u   // FAttackDataRow *UAttackComponent::GetAttackDataFromName(this, FName, bool)
#define RVA_PROP_IMPORTTEXT 0x86E0B0u     // UProperty::ImportText(this, const wchar_t *, void *data, int portflags, UObject *owner, FOutputDevice *)
#define RVA_PROP_EXPORTTEXT 0x88A3D0u     // bool UProperty::ExportText_Direct(this, FString *out, const void *data, const void *delta, UObject *parent, int portflags, UObject *scope)
#define RVA_STRUCT_INIT     0x7ABDB0u     // UScriptStruct::InitializeStruct(this, void *dest, int arrayDim)
#define RVA_STRUCT_COPY     0x7A6A30u     // UScriptStruct::CopyScriptStruct(this, void *dest, const void *src, int arrayDim)
#define RVA_ROWMAP_EMPLACE  0x7A0DF0u     // TSet<TTuple<FName, uint8 *>>::Emplace(this, int *id_out, TPairInitializer *{&name, &ptr}, bool *already)
#define RVA_DT_FINDROW      0x140CFC0u    // uint8 *UDataTable::FindRowUnchecked(this, FName, bool)
#define RVA_DT_GETROWNAMES  0x140F300u    // TArray<FName> *UDataTable::GetRowNames(this, TArray<FName> *out)
#define RVA_GWARN           0x368D9D0u    // FFeedbackContext *GWarn
#define RVA_FNAME_TOSTRING  0x7447D0u     // FString *FName::ToString(const FName *this, FString *out)
#define RVA_FTEXT_TOSTRING  0x6C9580u     // const FString &FText::ToString(const FText *this)
#define IDX_DT_SERIALIZE    15            // UDataTable::Serialize(this, FArchive &)
#define OFF_DT_ROWSTRUCT    0x28
#define OFF_DT_ROWMAP       0x30
#define OFF_ROWMAP_NUM      0x08          // RowMap.Data.ArrayNum ...
#define OFF_ROWMAP_FREE     0x34          // ... minus NumFreeIndices = live rows
typedef struct { uint32_t index, number; } FNameRaw;
typedef void (*DtSerialize_t)(void *table, void *archive);
static DtSerialize_t g_orig_dt_serialize;
static int g_row_faults;

// ----- engine helpers
static int rows_guard(void) {
    static const struct { unsigned rva; uint8_t b[10]; int n; const char *name; } g[] = {
        { RVA_FNAME_CTOR_W, { 0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0xD9 }, 9, "FName::FName(wchar_t)" },
        { RVA_STATICFINDOBJ, { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x18 }, 10, "StaticFindObject" },
        { RVA_CLS_DATATABLE, { 0x4C,0x8B,0xDC,0x48,0x83,0xEC,0x78 }, 7, "UDataTable class" },
        { RVA_CLS_ATTACKCOMP, { 0x4C,0x8B,0xDC,0x48,0x83,0xEC,0x78 }, 7, "UAttackComponent class" },
        { RVA_ATTACKDATA_BYNAME, { 0x48,0x83,0xEC,0x28,0x48,0x8B,0x89,0x80,0x03,0x00 }, 10, "GetAttackDataFromName" },
        { RVA_PROP_IMPORTTEXT, { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10 }, 10, "UProperty::ImportText" },
        { RVA_PROP_EXPORTTEXT, { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10 }, 10, "UProperty::ExportText_Direct" },
        { RVA_STRUCT_INIT, { 0x48,0x89,0x54,0x24,0x10,0x53,0x55,0x56,0x57 }, 9, "UScriptStruct::InitializeStruct" },
        { RVA_STRUCT_COPY, { 0x44,0x89,0x4C,0x24,0x20,0x4C,0x89,0x44,0x24,0x18 }, 10, "UScriptStruct::CopyScriptStruct" },
        { RVA_ROWMAP_EMPLACE, { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10 }, 10, "RowMap Emplace" },
        { RVA_DT_FINDROW, { 0x48,0x83,0xEC,0x08,0x48,0x83,0x79,0x28,0x00 }, 9, "UDataTable::FindRowUnchecked" },
        { RVA_DT_GETROWNAMES, { 0x40,0x53,0x48,0x83,0xEC,0x20,0x33,0xC0,0x48,0x83 }, 10, "UDataTable::GetRowNames" },
        { RVA_FNAME_TOSTRING, { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x40 }, 10, "FName::ToString" },
        { RVA_FTEXT_TOSTRING, { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9 }, 9, "FText::ToString" },
    };
    static int state;                                        // 0 unchecked, 1 ok, -1 mismatch
    if (state) return state > 0;
    state = 1;
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++)
        if (memcmp(g_base + g[i].rva, g[i].b, (size_t)g[i].n) != 0) { logf_("rows: %s bytes differ from this build - row editing is off", g[i].name); state = -1; }
    return state > 0;
}
static FNameRaw fname_of(const wchar_t *s) { FNameRaw n = { 0, 0 }; ((void *(*)(FNameRaw *, const wchar_t *, int))(g_base + RVA_FNAME_CTOR_W))(&n, s, 1); return n; }
static void fname_text(FNameRaw n, char *out, int cap) {
    out[0] = 0; struct { wchar_t *data; int num, max; } fs = { 0, 0, 0 };
    ((void *(*)(FNameRaw *, void *))(g_base + RVA_FNAME_TOSTRING))(&n, &fs);
    if (fs.data && fs.num > 0 && fs.num < 1000) narrow(fs.data, fs.num - 1, out, cap);
    if (fs.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(fs.data);
}
static void ftext_text(const uint8_t *ftext, char *out, int cap) {                   // FText = { ITextData shared ptr (8), ref controller (8), flags (4) }
    out[0] = 0; if (!*(void **)ftext) { snprintf(out, cap, ""); return; }
    struct { wchar_t *data; int num, max; } *fs = ((void *(*)(const void *))(g_base + RVA_FTEXT_TOSTRING))(ftext);
    if (fs && fs->data && fs->num > 0 && fs->num < 4000) narrow(fs->data, fs->num - 1, out, cap);
}
static void *table_find(const wchar_t *objpath) {
    void *cls = ((void *(*)(void))(g_base + RVA_CLS_DATATABLE))();
    return ((void *(*)(void *, void *, const wchar_t *, int))(g_base + RVA_STATICFINDOBJ))(cls, NULL, objpath, 0);
}
static int table_rows(void *t) { uint8_t *m = (uint8_t *)t + OFF_DT_ROWMAP; return *(int *)(m + OFF_ROWMAP_NUM) - *(int *)(m + OFF_ROWMAP_FREE); }
static uint8_t *table_row(void *t, FNameRaw name) { return ((uint8_t *(*)(void *, FNameRaw, int))(g_base + RVA_DT_FINDROW))(t, name, 0); }
static void *struct_prop(void *rowstruct, FNameRaw name) {         // walks the struct and its parents (UStruct +0x30 Super, +0x38 Children, UField +0x28 Next, FName at +0x18)
    for (uint8_t *s = rowstruct; s; s = *(uint8_t **)(s + 0x30))
        for (uint8_t *f = *(uint8_t **)(s + 0x38); f; f = *(uint8_t **)(f + 0x28))
            if (*(uint64_t *)(f + 0x18) == *(uint64_t *)&name) return f;
    return NULL;
}
static const char *prop_kind(uint8_t *prop) {                      // class name without the "Property" suffix, cached per class
    static struct { uint64_t cls; char kind[32]; } cache[32]; static int used;
    uint64_t c = *(uint64_t *)(*(uint8_t **)(prop + 0x10) + 0x18);
    for (int i = 0; i < used; i++) if (cache[i].cls == c) return cache[i].kind;
    char t[64]; fname_text(*(FNameRaw *)(*(uint8_t **)(prop + 0x10) + 0x18), t, sizeof t);
    size_t n = strlen(t); if (n > 8 && !strcmp(t + n - 8, "Property")) t[n - 8] = 0;
    if (used < 32) { cache[used].cls = c; snprintf(cache[used].kind, sizeof cache[used].kind, "%s", t); return cache[used++].kind; }
    static char spare[32]; snprintf(spare, sizeof spare, "%s", t); return spare;
}
// A TSoftObjectPtr (40 bytes) is { FWeakObjectPtr WeakPtr (+0), int32 TagAtLastTest (+8, a global counter), FSoftObjectPath { FName AssetPathName (+16), FString SubPath (+24) } }.
// Accepts  /Game/A/B.B  "/Game/A/B.B"  Class'/Game/A/B.B'  and None.
static void soft_set(uint8_t *value, const wchar_t *text) {
    wchar_t path[300]; size_t n = 0; const wchar_t *p = text, *slash;
    while (*p == L' ') p++;
    const wchar_t *q = wcschr(p, L'\''); slash = wcschr(p, L'/'); if (q && (!slash || q < slash)) p = q + 1;
    if (*p == L'"') p++;
    for (; p[n] && p[n] != L'"' && p[n] != L'\'' && n < 299; n++) path[n] = p[n];
    path[n] = 0; while (n && path[n - 1] == L' ') path[--n] = 0;
    FNameRaw none = { 0, 0 }; FNameRaw nm = (n == 0 || !_wcsicmp(path, L"None")) ? none : fname_of(path);
    *(int32_t *)(value + 0) = -1; *(int32_t *)(value + 4) = 0;       // weak pointer: not resolved yet (TagAtLastTest at +8 is left alone)
    memcpy(value + 16, &nm, 8);                                       // AssetPathName
    memset(value + 24, 0, 16);                                        // SubPathString empty
}
static void hexdump(const uint8_t *p, int n, char *out, int cap) { int w = 0; for (int i = 0; i < n && w + 3 < cap; i++) w += snprintf(out + w, cap - w, "%02X ", p[i]); out[w] = 0; }

// A hard object reference (Object / Class / Interface property) is only imported if the object is already in memory or its package file is in a pak: ImportText would
// load it, and loading a package that does not exist hangs the boot (runtime-created objects such as /Game/Mods/X/Item.Item cannot be named in rows.json; patch the
// pointer at runtime with a plugin instead). Returns 1 if the value is fine (or cannot be judged), 0 with a reason otherwise.
typedef void *(*PakFindRef_t)(void *self, const wchar_t *path, void **out_pak);
static int rows_objref_ok(const char *kind, const char *text, char *why, size_t cap) {
    char full[400], pkg[400], base[460];
    if (!rowval_is_hardref(kind) || !rowval_objpath(text, full, sizeof full, pkg, sizeof pkg)) return 1;
    wchar_t wfull[400]; widen(full, wfull, 400);
    void *inmem = NULL; __try { inmem = ((void *(*)(void *, void *, const wchar_t *, int))(g_base + RVA_STATICFINDOBJ))(NULL, NULL, wfull, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { inmem = NULL; }
    if (inmem) return 1;
    if (!rowval_pakbase(pkg, base, sizeof base)) return 1;                       // /Script/... and other roots: the engine resolves them itself
    if (!g_pak_self) { logf_("rows: objref %s: pak lookup unavailable, value accepted unchecked", full); return 1; }
    for (int u = 0; u < 2; u++) {
        char fn[480]; snprintf(fn, sizeof fn, "%s%s", base, u ? ".umap" : ".uasset"); wchar_t wf[480]; widen(fn, wf, 480); void *pak = NULL, *entry = NULL;
        __try { entry = ((PakFindRef_t)(g_base + RVA_PAK_FIND))(g_pak_self, wf, &pak); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("rows: objref pak lookup EXCEPTION 0x%08lX, value accepted unchecked", GetExceptionCode()); return 1; }
        if (entry) return 1;
    }
    snprintf(why, cap, "object reference '%s' is not loaded and its package is in no pak (loading it would hang the game); skipped", full); return 0;
}
// Is the object a hard reference names already in memory (or does the value name no object at all)? Then ImportText only looks it up and loads nothing.
static int rows_objref_loaded(const char *text) {
    char full[400], pkg[400]; if (!rowval_objpath(text, full, sizeof full, pkg, sizeof pkg)) return 1;
    wchar_t wfull[400]; widen(full, wfull, 400); void *o = NULL;
    __try { o = ((void *(*)(void *, void *, const wchar_t *, int))(g_base + RVA_STATICFINDOBJ))(NULL, NULL, wfull, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { o = NULL; }
    return o != NULL;
}
// Importing a hard reference to an object that is NOT loaded yet while the table itself is being loaded (inside UDataTable::Serialize, at boot) hangs the game before its
// first tick: ImportText loads the package synchronously from inside a running package load. Such edits are therefore DEFERRED while g_rows_in_load is set
// and applied at the start of the next game-thread tick (rows_apply_deferred), outside any load, where the synchronous load is the normal engine path.
static __declspec(thread) int g_rows_in_load;                       // per thread: an F11 / reload apply on the game thread is never "inside a load"
#define ROW_DEFERRED 2

// ----- row operations (each returns 1 on success, ROW_DEFERRED if queued for the game thread; reason explains a failure)
static int se_root_flag(void *obj, int on);   // secore.inc
static int row_set_q(void *table, const RowOp *op, char *reason, size_t rcap) {
    wchar_t wrow[ROWS_NAME_MAX], wprop[ROWS_NAME_MAX], wtext[400]; int cut = 0; widen(op->row, wrow, ROWS_NAME_MAX); widen(op->prop, wprop, ROWS_NAME_MAX); widen_u8(op->text, wtext, 400, &cut);
    if (cut) { snprintf(reason, rcap, "the value is longer than 399 characters (it would have been cut)"); return 0; }
    void *rs = *(void **)((uint8_t *)table + OFF_DT_ROWSTRUCT); if (!rs) { snprintf(reason, rcap, "the table has no row struct"); return 0; }
    uint8_t *row = table_row(table, fname_of(wrow)); if (!row) { snprintf(reason, rcap, "row '%s' does not exist", op->row); return 0; }
    uint8_t *prop = struct_prop(rs, fname_of(wprop)); if (!prop) { snprintf(reason, rcap, "no property '%s'", op->prop); return 0; }
    int off = *(int *)(prop + 0x44); const char *kind = prop_kind(prop);
    char why[100]; if (!rowval_check(kind, op->text, why, sizeof why)) { snprintf(reason, rcap, "%s (%s property)", why, kind); return 0; }
    if (!rows_objref_ok(kind, op->text, why, sizeof why)) { snprintf(reason, rcap, "%s", why); logf_("rows: %s.%s %s: %s", op->row, op->prop, op->text, why); return 0; }
    if (!strcmp(kind, "SoftObject")) { soft_set(row + off, wtext); return 1; }
    int hard = rowval_is_hardref(kind);
#ifndef ROWS_DIAG_NODEFER                                                 // diagnostic builds only (/DROWS_DIAG_NODEFER): import inside the load to reproduce the hang
    if (hard && g_rows_in_load && !rows_objref_loaded(op->text)) { snprintf(reason, rcap, "deferred to the game thread (%s is not loaded yet)", op->text); return ROW_DEFERRED; }
#endif
    if (hard) logf_("rows: import %s.%s = %s (%s reference, thread %lu) ...", op->row, op->prop, op->text, kind, GetCurrentThreadId());     // a hang here is attributable: no "done" line follows
    void *warn = *(void **)(g_base + RVA_GWARN);
    const wchar_t *rest = ((const wchar_t *(*)(void *, const wchar_t *, void *, int, void *, void *))(g_base + RVA_PROP_IMPORTTEXT))(prop, wtext, row + off, 0, NULL, warn);
    if (hard) logf_("rows: import %s.%s done: %s, value now %p", op->row, op->prop, rest ? "ok" : "REJECTED", *(void **)(row + off));
    if (!rest) { snprintf(reason, rcap, "the game did not accept '%s' for this %s property", op->text, kind); return 0; }
    // An object this edit pulled into memory has no other owner (a DataTable row does not keep it alive in the cooked game), so GC would collect it at the
    // next travel (an intro clip would then show the reference pose). Keep it rooted.
    if (hard) { void *v = *(void **)(row + off); if (v) { int r = 0; __try { r = se_root_flag(v, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; } if (!r) logf_("rows: %s.%s: could not root %p", op->row, op->prop, v); } }
    return 1;
}
static int row_add_q(void *table, const RowOp *op, char *reason, size_t rcap) {
    wchar_t wsrc[ROWS_NAME_MAX], wnew[ROWS_NAME_MAX]; widen(op->src, wsrc, ROWS_NAME_MAX); widen(op->row, wnew, ROWS_NAME_MAX);
    void *rs = *(void **)((uint8_t *)table + OFF_DT_ROWSTRUCT); if (!rs) { snprintf(reason, rcap, "the table has no row struct"); return 0; }
    FNameRaw nn = fname_of(wnew);
    if (table_row(table, nn)) return 1;                              // exists already (another mod added it): left alone
    uint8_t *from = table_row(table, fname_of(wsrc)); if (!from) { snprintf(reason, rcap, "the row to copy ('%s') does not exist", op->src); return 0; }
    int size = *(int *)((uint8_t *)rs + 0x40), align = *(int *)((uint8_t *)rs + 0x44); if (align < 1) align = 1;
    size = (size + align - 1) & ~(align - 1);
    uint8_t *mem = ((void *(*)(size_t, unsigned))(g_base + RVA_FMEMORY_MALLOC))((size_t)size, 0); if (!mem) { snprintf(reason, rcap, "out of memory"); return 0; }
    ((void (*)(void *, void *, int))(g_base + RVA_STRUCT_INIT))(rs, mem, 1);
    ((void (*)(void *, void *, const void *, int))(g_base + RVA_STRUCT_COPY))(rs, mem, from, 1);
    struct { FNameRaw *key; uint8_t **value; } pair = { &nn, &mem }; unsigned char already = 0; int element_id = -1;
    ((void *(*)(void *, int *, void *, unsigned char *))(g_base + RVA_ROWMAP_EMPLACE))((uint8_t *)table + OFF_DT_ROWMAP, &element_id, &pair, &already);
    if (table_row(table, nn) != mem) { snprintf(reason, rcap, "the new row was not found after insertion"); return 0; }
    return 1;
}

// remove a row (for example one another mod added). The game has no UDataTable::RemoveRow, so the RowMap TSet is unlinked by hand,
// UE 4.18 layout: TSparseArray Data {ptr, num, max} +0x00, AllocationFlags TBitArray (4 inline dwords +0x10, heap ptr +0x20, NumBits +0x28) , FirstFreeIndex +0x30,
// NumFreeIndices +0x34; Hash TInlineAllocator<1> (inline int +0x38, heap ptr +0x40), HashSize +0x48. Element (24 bytes) = FName key, uint8 *row, HashNextId, HashIndex.
// Nothing changes unless the element is found through its own hash chain AND is what FindRowUnchecked returns; afterwards the row must be gone and every other row
// still found. The row memory is not freed (a dangling pointer is worse than 1 leaked row). Hand-written TSet::Remove: re-check the offsets after an engine change.
static int row_remove_q(void *table, const RowOp *op, char *reason, size_t rcap) {
    wchar_t w[ROWS_NAME_MAX]; widen(op->row, w, ROWS_NAME_MAX); FNameRaw nm = fname_of(w); uint64_t key = *(uint64_t *)&nm;
    uint8_t *row = table_row(table, nm); if (!row) return 1;                                   // not there (never added, or removed already): fine
    uint8_t *rm = (uint8_t *)table + OFF_DT_ROWMAP; uint8_t *data = *(uint8_t **)rm; int num = *(int *)(rm + 0x08);
    uint32_t *bits = *(uint32_t **)(rm + 0x20) ? *(uint32_t **)(rm + 0x20) : (uint32_t *)(rm + 0x10); int nbits = *(int *)(rm + 0x28);
    int *hash = *(int **)(rm + 0x40) ? *(int **)(rm + 0x40) : (int *)(rm + 0x38); int hsize = *(int *)(rm + 0x48);
    if (!data || num <= 0 || nbits < num || hsize <= 0 || (hsize & (hsize - 1))) { snprintf(reason, rcap, "row map layout not as expected (num %d bits %d hash size %d): nothing removed", num, nbits, hsize); return 0; }
    #define RM_EL(i) (data + (size_t)(i) * 24)
    #define RM_LIVE(i) ((bits[(i) >> 5] >> ((i) & 31)) & 1)
    int id = -1; for (int i = 0; i < num; i++) if (RM_LIVE(i) && *(uint64_t *)RM_EL(i) == key) { id = i; break; }
    if (id < 0 || *(uint8_t **)(RM_EL(id) + 8) != row) { snprintf(reason, rcap, "row map layout not as expected (element %d does not hold the row): nothing removed", id); return 0; }
    int *slot = &hash[*(int *)(RM_EL(id) + 20) & (hsize - 1)], steps = 0;
    while (*slot != -1 && *slot != id && steps++ < num) { if (*slot < 0 || *slot >= num) { snprintf(reason, rcap, "hash chain points outside the set (%d): nothing removed", *slot); return 0; } slot = (int *)(RM_EL(*slot) + 16); }
    if (*slot != id) { snprintf(reason, rcap, "row not on its hash chain: nothing removed"); return 0; }
    *slot = *(int *)(RM_EL(id) + 16);                                                         // unlink from the hash chain
    int *first = (int *)(rm + 0x30), *nfree = (int *)(rm + 0x34);                             // TSparseArray::RemoveAt: free list + allocation bit
    if (*nfree) *(int *)RM_EL(*first) = id;
    *(int *)RM_EL(id) = -1; *(int *)(RM_EL(id) + 4) = *nfree > 0 ? *first : -1; *first = id; (*nfree)++; bits[id >> 5] &= ~(1u << (id & 31));
    int lost = 0; for (int i = 0; i < num; i++) if (RM_LIVE(i)) { FNameRaw k = *(FNameRaw *)RM_EL(i); if (table_row(table, k) != *(uint8_t **)(RM_EL(i) + 8)) lost++; }
    #undef RM_EL
    #undef RM_LIVE
    if (table_row(table, nm)) { snprintf(reason, rcap, "row still found after the removal"); return 0; }
    if (lost) { snprintf(reason, rcap, "%d other row(s) no longer found after the removal (layout mismatch)", lost); logf_("rows: ERROR removing %s broke %d rows", op->row, lost); return 0; }
    logf_("rows: removed row %s (element %d; %d rows left)", op->row, id, num - *nfree);
    return 1;
}

// ----- applying the ops of all mods to a table that just loaded
static uint64_t *g_op_table; static int g_op_table_n;               // FName of each op's table (computed on first use: the engine's name table must be up)
static uint64_t g_row_tables[64]; static int g_row_tables_n;        // distinct table names that have ops
static struct { int applied, failed; } g_rowstat[MODS_MAX + 1];
static char g_rowfail[60][480]; static int g_rowfail_n, g_rowfail_total;
static struct { int op; uint64_t table; } g_row_defer[256]; static int g_row_defer_n;        // ops (index into g_rowops) waiting for the game thread, see g_rows_in_load
static void rows_prepare(void) {
    if (g_op_table_n == g_rowops.count) return;
    free(g_op_table); g_op_table = malloc((size_t)(g_rowops.count ? g_rowops.count : 1) * sizeof *g_op_table); g_row_tables_n = 0; g_op_table_n = 0;
    if (!g_op_table) return;
    for (int i = 0; i < g_rowops.count; i++) {
        wchar_t w[ROWS_TABLE_MAX]; widen(g_rowops.a[i].table, w, ROWS_TABLE_MAX); FNameRaw f = fname_of(w); g_op_table[i] = *(uint64_t *)&f;
        int seen = 0; for (int k = 0; k < g_row_tables_n; k++) if (g_row_tables[k] == g_op_table[i]) seen = 1;
        if (!seen && g_row_tables_n < 64) g_row_tables[g_row_tables_n++] = g_op_table[i];
    }
    g_op_table_n = g_rowops.count;
}
static void rows_write_summary(void) {
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%srows.txt", g_dir);
    FILE *f = _wfopen(path, L"wb"); if (!f) return;
    fprintf(f, "Row edits (rows.json) applied by LanNative. Applied when a table loads; the later mod wins.\r\n\r\n");
    for (int m = 0; m < MODS_MAX + 1; m++) if (g_rowstat[m].applied || g_rowstat[m].failed) fprintf(f, "%-28s applied %d, failed %d\r\n", m < MODS_MAX ? g_posname[m] : "?", g_rowstat[m].applied, g_rowstat[m].failed);
    fprintf(f, "\r\nFailures (%d, first %d shown):\r\n", g_rowfail_total, g_rowfail_n);
    for (int i = 0; i < g_rowfail_n; i++) fprintf(f, "%s\r\n", g_rowfail[i]);
    fclose(f);
}
static int rows_apply_table(void *table, FNameRaw self, const char *tname) {
    int applied = 0, failed = 0, deferred = 0;
    logf_("rows: table %s: applying edits (thread %lu%s) ...", tname, GetCurrentThreadId(), g_rows_in_load ? ", inside the table load" : "");
    for (int i = 0; i < g_rowops.count; i++) {
        if (g_op_table[i] != *(uint64_t *)&self) continue;
        const RowOp *op = &g_rowops.a[i]; char why[300] = ""; int mod = op->mod < MODS_MAX ? op->mod : MODS_MAX;
        int ok = op->is_remove ? row_remove_q(table, op, why, sizeof why) : op->is_add ? row_add_q(table, op, why, sizeof why) : row_set_q(table, op, why, sizeof why);
        if (ok == ROW_DEFERRED) {
            int dup = 0; EnterCriticalSection(&g_lock);
            for (int k = 0; k < g_row_defer_n; k++) if (g_row_defer[k].op == i) dup = 1;
            if (!dup && g_row_defer_n < 256) { g_row_defer[g_row_defer_n].op = i; g_row_defer[g_row_defer_n].table = *(uint64_t *)&self; g_row_defer_n++; dup = 1; }
            LeaveCriticalSection(&g_lock);
            if (dup) { deferred++; logf_("rows: %s.%s %s", op->row, op->prop, why); continue; }
            ok = 0; snprintf(why, sizeof why, "more than 256 object references wait for the game thread - skipped");
        }
        if (ok == 1) { applied++; g_rowstat[mod].applied++; }
        else {
            failed++; g_rowstat[mod].failed++; g_rowfail_total++;
            if (g_rowfail_n < 60) { snprintf(g_rowfail[g_rowfail_n++], sizeof g_rowfail[0], "%s: %s %s%s%s%s%s: %s", g_posname[mod], op->is_add ? "add" : op->is_remove ? "remove" : "set", op->row, op->is_add || op->is_remove ? "" : ".", op->is_add || op->is_remove ? "" : op->prop, op->is_add ? " (copy of " : "", op->is_add ? op->src : "", why); if (op->is_add) { size_t n = strlen(g_rowfail[g_rowfail_n - 1]); (void)n; } }
        }
    }
    logf_("rows: table %s loaded (thread %lu): %d edit(s) applied, %d failed, %d deferred to the game thread, %d rows now", tname, GetCurrentThreadId(), applied, failed, deferred, table_rows(table));
    rows_write_summary();
    return applied;
}
static void hk_dt_serialize(void *table, void *archive) {
    g_orig_dt_serialize(table, archive);
    if (!g_rowops.count || g_row_faults >= 3 || !rows_guard()) return;
    g_rows_in_load = 1;
    __try {
        rows_prepare(); if (g_op_table_n == g_rowops.count) {
            FNameRaw self = *(FNameRaw *)((uint8_t *)table + 0x18); int hit = 0;
            for (int k = 0; k < g_row_tables_n; k++) if (g_row_tables[k] == *(uint64_t *)&self) hit = 1;
            if (hit) { char tn[80]; fname_text(self, tn, sizeof tn); rows_apply_table(table, self, tn); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_row_faults++; logf_("rows: EXCEPTION 0x%08lX while applying edits (fault %d of 3)", GetExceptionCode(), g_row_faults); }
    g_rows_in_load = 0;
}


// ----- development commands (F11)
typedef struct { void *cls; void *table; void *found; } CompSearch;
static void comp_visit(void *obj, void *ctx) {                      // a live UAttackComponent whose m_AnimDBXls is this table
    CompSearch *s = ctx; if (s->found) return;
    uint8_t *o = obj; void *c = *(void **)(o + 0x10);
    for (int depth = 0; c && depth < 16; depth++, c = *(void **)((uint8_t *)c + 0x30)) if (c == s->cls) { if (*(void **)(o + 0x380) == s->table) s->found = obj; return; }
}
static void comp_visit_any(void *obj, void *ctx) {                  // first live UAttackComponent that has an attack table
    CompSearch *s = ctx; if (s->found) return;
    uint8_t *o = obj; void *c = *(void **)(o + 0x10);
    for (int depth = 0; c && depth < 16; depth++, c = *(void **)((uint8_t *)c + 0x30)) if (c == s->cls) { if (*(void **)(o + 0x380)) s->found = obj; return; }
}
static void obj_command(const char *line) {                         // objget / objset <property> [text] on the first live UAttackComponent: any property of a plain UObject
    char cmd[16] = "", prop[ROWS_NAME_MAX] = "", rest[300] = ""; sscanf(line, "%15s %191s %299[^\n]", cmd, prop, rest);      // 191 = ROWS_NAME_MAX - 1
    CompSearch f = { ((void *(*)(void))(g_base + RVA_CLS_ATTACKCOMP))(), NULL, NULL };
    api_scan_objects(comp_visit_any, &f);
    if (!f.found) { logf_("rows: obj: no live UAttackComponent"); return; }
    uint8_t *o = f.found; void *cls = *(void **)(o + 0x10); wchar_t wprop[ROWS_NAME_MAX]; widen(prop, wprop, ROWS_NAME_MAX);
    uint8_t *p = struct_prop(cls, fname_of(wprop)); if (!p) { logf_("rows: obj: property '%s' not found on the object's class", prop); return; }
    int off = *(int *)(p + 0x44), size = *(int *)(p + 0x34) * *(int *)(p + 0x30); char hx[96], before[96];
    hexdump(o + off, size < 16 ? size : 16, before, sizeof before);
    if (!strcmp(cmd, "objset")) {
        wchar_t wtext[300]; widen(rest, wtext, 300);
        const wchar_t *r = ((const wchar_t *(*)(void *, const wchar_t *, void *, int, void *, void *))(g_base + RVA_PROP_IMPORTTEXT))(p, wtext, o + off, 0, o, *(void **)(g_base + RVA_GWARN));
        hexdump(o + off, size < 16 ? size : 16, hx, sizeof hx);
        logf_("rows: objset %s = '%s' on UAttackComponent %p -> %s: %s=> %s (as float %g)", prop, rest, f.found, r ? "ok" : "ImportText FAILED", before, hx, *(float *)(o + off));
    } else logf_("rows: objget %s on UAttackComponent %p = %s(offset 0x%X, %d bytes; as float %g, as int32 %d)", prop, f.found, before, off, size, *(float *)(o + off), *(int32_t *)(o + off));
}
// canonical text of one property value: what rows.json would hold
static void prop_value_text(uint8_t *prop, const uint8_t *value, char *out, int cap) {
    const char *kind = prop_kind(prop); out[0] = 0;
    if (!strcmp(kind, "Float")) { snprintf(out, cap, "%.9g", (double)*(const float *)value); return; }         // exact round trip (the engine prints 6 decimals)
    if (!strcmp(kind, "Double")) { snprintf(out, cap, "%.17g", *(const double *)value); return; }
    if (!strcmp(kind, "SoftObject")) { FNameRaw nm; memcpy(&nm, value + 16, 8); if (!nm.index) snprintf(out, cap, "None"); else fname_text(nm, out, cap); return; }
    if (!strcmp(kind, "Text")) {
        char t[600]; ftext_text(value, t, sizeof t); int w = snprintf(out, cap, "INVTEXT(\"");
        for (const char *p = t; *p && w + 6 < cap; p++) { if (*p == '"' || *p == '\\') out[w++] = '\\'; out[w++] = *p; }
        snprintf(out + w, cap - w, "\")"); return;
    }
    struct { wchar_t *data; int num, max; } fs = { 0, 0, 0 };
    ((int (*)(void *, void *, const void *, const void *, void *, int, void *))(g_base + RVA_PROP_EXPORTTEXT))(prop, &fs, value, value, NULL, 0, NULL);
    if (fs.data && fs.num > 0 && fs.num < 1000) narrow(fs.data, fs.num - 1, out, cap);
    if (fs.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(fs.data);
}
static int prop_same(uint8_t *prop, const uint8_t *a, const uint8_t *b) {
    const char *kind = prop_kind(prop);
    if (!strcmp(kind, "SoftObject") || !strcmp(kind, "Text") || !strcmp(kind, "Str") || !strcmp(kind, "Array") || !strcmp(kind, "Map") || !strcmp(kind, "Struct")) {
        char x[700], y[700]; prop_value_text(prop, a, x, sizeof x); prop_value_text(prop, b, y, sizeof y); return !strcmp(x, y);
    }
    return memcmp(a, b, (size_t)(*(int *)(prop + 0x34) * *(int *)(prop + 0x30))) == 0;
}
// ----- generic objects (data assets): find any loaded object by path and dump every property as text
static void *obj_find_any(const wchar_t *path) {
    return ((void *(*)(void *, void *, const wchar_t *, int))(g_base + RVA_STATICFINDOBJ))(NULL, NULL, path, 0);
}
static int field_is_property(uint8_t *f) {                         // UStruct::Children also holds UFunctions: keep the fields whose class name ends in "Property"
    static struct { uint64_t cls; int yes; } cache[64]; static int used;
    uint64_t c = *(uint64_t *)(*(uint8_t **)(f + 0x10) + 0x18);
    for (int i = 0; i < used; i++) if (cache[i].cls == c) return cache[i].yes;
    char t[64]; fname_text(*(FNameRaw *)(*(uint8_t **)(f + 0x10) + 0x18), t, sizeof t); size_t n = strlen(t);
    int yes = n > 8 && !strcmp(t + n - 8, "Property"); if (used < 64) { cache[used].cls = c; cache[used].yes = yes; used++; }
    return yes;
}
// canonical text of a property value of any type (arrays, structs, maps, references too) via the engine's own export; written straight to out
static void prop_export_full(uint8_t *prop, const uint8_t *value, FILE *out) {
    const char *kind = prop_kind(prop);
    if (!strcmp(kind, "Float") || !strcmp(kind, "Double") || !strcmp(kind, "SoftObject") || !strcmp(kind, "Text")) { char v[800]; prop_value_text(prop, value, v, sizeof v); fputs(v, out); return; }
    struct { wchar_t *data; int num, max; } fs = { 0, 0, 0 };
    ((int (*)(void *, void *, const void *, const void *, void *, int, void *))(g_base + RVA_PROP_EXPORTTEXT))(prop, &fs, value, value, NULL, 0, NULL);
    if (fs.data && fs.num > 1) {
        int n = fs.num - 1; char *u8 = malloc((size_t)n * 4 + 4);
        if (u8) { int w = WideCharToMultiByte(CP_UTF8, 0, fs.data, n, u8, n * 4 + 3, NULL, NULL); fwrite(u8, 1, (size_t)(w > 0 ? w : 0), out); free(u8); }
    }
    if (fs.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(fs.data);
}
static void obj_dump(const char *objpath, void *obj) {
    char shortname[80]; const char *dot = strrchr(objpath, '.'); snprintf(shortname, sizeof shortname, "%s", dot ? dot + 1 : objpath);
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sobjdump_%S.txt", g_dir, shortname);
    FILE *out = _wfopen(path, L"wb"); if (!out) { logf_("rows: objdump: cannot write the file"); return; }
    void *cls = *(void **)((uint8_t *)obj + 0x10); char cn[80]; fname_text(*(FNameRaw *)((uint8_t *)cls + 0x18), cn, sizeof cn);
    fprintf(out, "# %s  (class %s)\n", objpath, cn); int n = 0;
    for (uint8_t *s = cls; s; s = *(uint8_t **)(s + 0x30))
        for (uint8_t *f = *(uint8_t **)(s + 0x38); f; f = *(uint8_t **)(f + 0x28)) {
            if (!field_is_property(f)) continue;
            char pn[100]; fname_text(*(FNameRaw *)(f + 0x18), pn, sizeof pn); int dim = *(int *)(f + 0x30), esz = *(int *)(f + 0x34), off = *(int *)(f + 0x44);
            for (int i = 0; i < dim; i++) {
                if (dim > 1) fprintf(out, "%s[%d] : %s = ", pn, i, prop_kind(f)); else fprintf(out, "%s : %s = ", pn, prop_kind(f));
                prop_export_full(f, (uint8_t *)obj + off + i * esz, out); fputc('\n', out); n++;
            }
        }
    fclose(out); logf_("rows: objdump %s: class %s, %d value(s) written to objdump_%s.txt", objpath, cn, n, shortname);
}
// engine export of one property value as a malloc'd UTF-16 string (any type: arrays, structs, references); caller frees. NULL on failure.
static wchar_t *prop_export_w(uint8_t *prop, const uint8_t *value, void *owner) {
    struct { wchar_t *data; int num, max; } fs = { 0, 0, 0 };
    ((int (*)(void *, void *, const void *, const void *, void *, int, void *))(g_base + RVA_PROP_EXPORTTEXT))(prop, &fs, value, value, owner, 0, NULL);
    wchar_t *copy = NULL;
    if (fs.data && fs.num > 0) { copy = malloc((size_t)fs.num * sizeof(wchar_t)); if (copy) memcpy(copy, fs.data, (size_t)fs.num * sizeof(wchar_t)); }
    if (fs.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(fs.data);
    return copy;
}
static int prop_import_w(uint8_t *prop, uint8_t *value, void *owner, const wchar_t *text) {
    return ((const wchar_t *(*)(void *, const wchar_t *, void *, int, void *, void *))(g_base + RVA_PROP_IMPORTTEXT))(prop, text, value, 0, owner, *(void **)(g_base + RVA_GWARN)) != NULL;
}
// objrt <objpath> <property>: export the property, import the same text back, export again: is the round trip lossless? (the basis of text edits to arrays and structs)
static void obj_roundtrip(const char *objpath, const char *propname, void *obj) {
    void *cls = *(void **)((uint8_t *)obj + 0x10); wchar_t wp[100]; widen(propname, wp, 100);
    uint8_t *p = struct_prop(cls, fname_of(wp)); if (!p) { logf_("rows: objrt: no property '%s'", propname); return; }
    uint8_t *val = (uint8_t *)obj + *(int *)(p + 0x44);
    wchar_t *a = prop_export_w(p, val, obj); if (!a) { logf_("rows: objrt: export failed"); return; }
    DWORD t0 = GetTickCount(); int ok = prop_import_w(p, val, obj, a); DWORD t1 = GetTickCount();
    wchar_t *b = prop_export_w(p, val, obj);
    logf_("rows: objrt %s.%s: %d chars exported, import %s in %lu ms, second export %s (%d chars)", objpath, propname, (int)wcslen(a), ok ? "ok" : "FAILED", (unsigned long)(t1 - t0),
          b && !wcscmp(a, b) ? "IDENTICAL" : "DIFFERENT", b ? (int)wcslen(b) : -1);
    free(a); free(b);
}
static int row_numeric(const char *s, long *v) { if (!*s) return 0; for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0; *v = atol(s); return 1; }
// rowdump <objpath> <first> <last>: the numeric rows in [first,last] as a rows.json (each one a clone of the most similar other row + the properties that differ)
// rowfull <objpath> <first> <last>: every property of those rows as text, for comparing two game states (a pak mod versus its rows.json)
static void rows_dump(void *table, long first, long last, int full) {
    void *rs = *(void **)((uint8_t *)table + OFF_DT_ROWSTRUCT); struct { FNameRaw *data; int num, max; } names = { 0, 0, 0 };
    ((void *(*)(void *, void *))(g_base + RVA_DT_GETROWNAMES))(table, &names);
    int n = names.num; if (!names.data || n <= 0 || n > 20000) { logf_("rows: dump: no rows"); return; }
    char (*nm)[ROWS_NAME_MAX] = calloc((size_t)n, ROWS_NAME_MAX); uint8_t **rowp = calloc((size_t)n, sizeof *rowp); int *inrange = calloc((size_t)n, sizeof *inrange);
    for (int i = 0; i < n; i++) { fname_text(names.data[i], nm[i], ROWS_NAME_MAX); rowp[i] = table_row(table, names.data[i]); long v; inrange[i] = row_numeric(nm[i], &v) && v >= first && v <= last; }
    ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(names.data);
    uint8_t *props[400]; int np = 0;
    for (uint8_t *s = rs; s; s = *(uint8_t **)(s + 0x30)) for (uint8_t *f = *(uint8_t **)(s + 0x38); f && np < 400; f = *(uint8_t **)(f + 0x28)) props[np++] = f;
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, full ? L"%srowfull.txt" : L"%srowdump.json", g_dir); FILE *out = _wfopen(path, L"w"); if (!out) { logf_("rows: dump: cannot write the file"); goto done; }
    int dumped = 0, total_set = 0;
    if (!full) fprintf(out, "{ \"format\": 1,\n  \"edits\": [\n");
    // order rows by numeric name so the output is stable
    int *order = malloc((size_t)n * sizeof *order); int no = 0; for (int i = 0; i < n; i++) if (inrange[i] && rowp[i]) order[no++] = i;
    for (int a = 0; a < no; a++) for (int b = a + 1; b < no; b++) if (atol(nm[order[b]]) < atol(nm[order[a]])) { int t = order[a]; order[a] = order[b]; order[b] = t; }
    for (int oi = 0; oi < no; oi++) {
        int r = order[oi]; char val[700];
        if (full) { for (int p = 0; p < np; p++) { char pn[ROWS_NAME_MAX]; fname_text(*(FNameRaw *)(props[p] + 0x18), pn, sizeof pn); prop_value_text(props[p], rowp[r] + *(int *)(props[p] + 0x44), val, sizeof val); fprintf(out, "%s %s = %s\n", nm[r], pn, val); } dumped++; continue; }
        int best = -1, bestdiff = 1 << 30;
        for (int c = 0; c < n; c++) {
            if (c == r || inrange[c] || !rowp[c]) continue; int diff = 0;
            for (int p = 0; p < np && diff < bestdiff; p++) { int off = *(int *)(props[p] + 0x44); if (!prop_same(props[p], rowp[r] + off, rowp[c] + off)) diff++; }
            if (diff < bestdiff) { bestdiff = diff; best = c; }
        }
        if (best < 0) continue;
        fprintf(out, "%s    { \"table\": \"attacks\", \"add\": \"%s\", \"copy_of\": \"%s\", \"set\": {", dumped ? ",\n" : "", nm[r], nm[best]);
        int first_set = 1;
        for (int p = 0; p < np; p++) {
            int off = *(int *)(props[p] + 0x44); if (prop_same(props[p], rowp[r] + off, rowp[best] + off)) continue;
            char pn[ROWS_NAME_MAX]; fname_text(*(FNameRaw *)(props[p] + 0x18), pn, sizeof pn); prop_value_text(props[p], rowp[r] + off, val, sizeof val);
            fprintf(out, "%s\n        \"%s\": \"", first_set ? "" : ",", pn); first_set = 0; total_set++;
            for (const char *q = val; *q; q++) { if (*q == '"' || *q == '\\') fputc('\\', out); fputc(*q, out); }
            fputc('"', out);
        }
        fprintf(out, "\n    } }"); dumped++;
    }
    if (!full) fprintf(out, "\n  ] }\n");
    free(order); fclose(out);
    logf_("rows: dump: %d row(s) written to %s (%d property edits)", dumped, full ? "rowfull.txt" : "rowdump.json", total_set);
done:
    free(nm); free(rowp); free(inrange);
}
static void rows_command(const char *line) {
    char cmd[16] = "", path[160] = "", row[ROWS_NAME_MAX] = "", prop[ROWS_NAME_MAX] = "", rest[300] = "";
    sscanf(line, "%15s %159s %191s %191s %299[^\n]", cmd, path, row, prop, rest);                                     // 191 = ROWS_NAME_MAX - 1
    if (!rows_guard()) return;
    if (!strcmp(cmd, "objget") || !strcmp(cmd, "objset")) { obj_command(line); return; }
    if (!strcmp(cmd, "objrt")) { wchar_t wp[160]; widen(path, wp, 160); void *o = obj_find_any(wp); if (!o) logf_("rows: objrt: '%s' is not loaded", path); else obj_roundtrip(path, row, o); return; }
    if (!strcmp(cmd, "objdump")) { wchar_t wp[160]; widen(path, wp, 160); void *o = obj_find_any(wp); if (!o) logf_("rows: objdump: '%s' is not loaded", path); else obj_dump(path, o); return; }
    wchar_t wpath[160]; widen(path, wpath, 160);
    void *t = table_find(wpath);
    if (!t) { logf_("rows: table '%s' not found (loaded yet?)", path); return; }
    wchar_t wrow[ROWS_NAME_MAX], wprop[ROWS_NAME_MAX]; widen(row, wrow, ROWS_NAME_MAX); widen(prop, wprop, ROWS_NAME_MAX);
    void *rs = *(void **)((uint8_t *)t + OFF_DT_ROWSTRUCT);
    if (!strcmp(cmd, "tbl")) {
        logf_("rows: table %s = %p, row struct %p, %d rows (checked on thread %lu)", path, t, rs, table_rows(t), GetCurrentThreadId());
    } else if (!strcmp(cmd, "rowget")) {
        uint8_t *r = table_row(t, fname_of(wrow)); uint8_t *p = rs ? struct_prop(rs, fname_of(wprop)) : NULL;
        if (!r || !p) { logf_("rows: rowget: row or property not found"); return; }
        char val[700]; prop_value_text(p, r + *(int *)(p + 0x44), val, sizeof val);
        logf_("rows: %s.%s = %s  (%s property, offset 0x%X)", row, prop, val, prop_kind(p), *(int *)(p + 0x44));
    } else if (!strcmp(cmd, "rowset")) {
        RowOp op; memset(&op, 0, sizeof op); snprintf(op.row, sizeof op.row, "%s", row); snprintf(op.prop, sizeof op.prop, "%s", prop); op.text = rest; char why[200] = "";
        logf_("rows: set %s.%s = '%s' -> %s %s", row, prop, rest, row_set_q(t, &op, why, sizeof why) ? "ok" : "FAILED:", why);
    } else if (!strcmp(cmd, "rowapply")) {
        rows_prepare(); FNameRaw self = *(FNameRaw *)((uint8_t *)t + 0x18); char tn[80]; fname_text(self, tn, sizeof tn); rows_apply_table(t, self, tn);
    } else if (!strcmp(cmd, "rowprops")) {
        int n = 0;
        for (uint8_t *st = rs; st; st = *(uint8_t **)(st + 0x30))
            for (uint8_t *f = *(uint8_t **)(st + 0x38); f; f = *(uint8_t **)(f + 0x28)) {
                char nm[80]; fname_text(*(FNameRaw *)(f + 0x18), nm, sizeof nm);
                logf_("rows: prop %s : %sProperty  offset 0x%X  size %d x %d", nm, prop_kind(f), *(int *)(f + 0x44), *(int *)(f + 0x34), *(int *)(f + 0x30)); n++;
            }
        logf_("rows: %d properties in the row struct", n);
    } else if (!strcmp(cmd, "rowprobe")) {
        CompSearch s = { ((void *(*)(void))(g_base + RVA_CLS_ATTACKCOMP))(), t, NULL };
        api_scan_objects(comp_visit, &s);
        if (!s.found) { logf_("rows: probe: no live UAttackComponent uses this table (is a character loaded?)"); return; }
        uint8_t *viaengine = ((uint8_t *(*)(void *, FNameRaw, int))(g_base + RVA_ATTACKDATA_BYNAME))(s.found, fname_of(wrow), 0);
        uint8_t *direct = table_row(t, fname_of(wrow)); uint8_t *p = rs ? struct_prop(rs, fname_of(wprop)) : NULL; char val[700] = "";
        if (viaengine && p) prop_value_text(p, viaengine + *(int *)(p + 0x44), val, sizeof val);
        logf_("rows: probe: a live UAttackComponent uses this table; its own GetAttackDataFromName('%s') -> %p, table lookup -> %p (%s); %s = %s as seen by the game",
              row, viaengine, direct, viaengine == direct ? "same row" : "DIFFERENT", prop, val);
    } else if (!strcmp(cmd, "rowdump") || !strcmp(cmd, "rowfull")) {
        rows_dump(t, atol(row), atol(prop), !strcmp(cmd, "rowfull"));
    }
}

// ----- data asset edits: { "object": path, "ops": [ set / append / insert at a path into a property's text ] }
// The property is exported to text with the engine's own exporter, the ops are spliced into the text (uetext.h), and the result is imported back with ImportText
// (the export / import round trip is lossless). Applied right after the object finishes loading (see rows_drain_objects for off-game-thread loads).
#define IDX_POSTLOAD 16                   // UObject::ConditionalPostLoad(this)
typedef void (*PostLoad_t)(void *obj);
static PostLoad_t g_orig_postload;
static uint64_t g_obj_names[64]; static int g_obj_names_n, g_obj_prepared = -1; static int g_obj_faults;
static void *g_obj_done[256]; static int g_obj_done_n;
static void obj_prepare(void) {
    if (g_obj_prepared == g_objops.count) return;
    g_obj_names_n = 0;
    for (int i = 0; i < g_objops.count; i++) {
        const char *dot = strrchr(g_objops.a[i].object, '.'); wchar_t w[100]; widen(dot ? dot + 1 : g_objops.a[i].object, w, 100); FNameRaw f = fname_of(w); int seen = 0;
        for (int k = 0; k < g_obj_names_n; k++) if (g_obj_names[k] == *(uint64_t *)&f) seen = 1;
        if (!seen && g_obj_names_n < 64) g_obj_names[g_obj_names_n++] = *(uint64_t *)&f;
    }
    g_obj_prepared = g_objops.count;
}
static char *wide_to_utf8(const wchar_t *w) { int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL); if (n <= 0) return NULL; char *s = malloc((size_t)n); if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); return s; }
static wchar_t *utf8_to_wide(const char *s) { int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0); if (n <= 0) return NULL; wchar_t *w = malloc((size_t)n * sizeof(wchar_t)); if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); return w; }
static int path_on_prop(const char *path, const char *name) { size_t n = strlen(name); return !strncmp(path, name, n) && (path[n] == 0 || path[n] == '.' || path[n] == '['); }   // first path segment == property
static void obj_fail(int mod, const char *objpath, const ObjOp *op, const char *why) {
    int m = mod < MODS_MAX ? mod : MODS_MAX; g_rowstat[m].failed++; g_rowfail_total++;
    if (g_rowfail_n < 60) snprintf(g_rowfail[g_rowfail_n++], sizeof g_rowfail[0], "%s: %s %s %s: %s", g_posname[m], op->kind == UT_SET ? "set" : op->kind == UT_APPEND ? "append" : "insert", strrchr(objpath, '/') ? strrchr(objpath, '/') + 1 : objpath, op->path, why);
}
static void obj_apply_object(void *obj, const char *objpath) {
    typedef struct { char name[80]; uint8_t *prop; int off; char *text; int dirty; int first_op, nops, failed; } WorkProp;
    WorkProp wp[24]; int nwp = 0; void *cls = *(void **)((uint8_t *)obj + 0x10); DWORD t0 = GetTickCount(); int applied = 0, failed = 0;
    unsigned char *opok = calloc((size_t)(g_objops.count ? g_objops.count : 1), 1);
    for (int i = 0; i < g_objops.count; i++) {
        const ObjOp *op = &g_objops.a[i]; if (strcmp(op->object, objpath)) continue;
        char pname[80]; const char *p = op->path; size_t n = 0; while (p[n] && p[n] != '.' && p[n] != '[' && n < 79) { pname[n] = p[n]; n++; } pname[n] = 0;
        const char *rest = p + n; char restbuf[220]; int idx = -1;
        if (*rest == '[') { char *end; idx = (int)strtol(rest + 1, &end, 10); rest = end + 1; snprintf(restbuf, sizeof restbuf, "[%d]%s", idx, *rest == '.' ? rest : ""); rest = restbuf; if (rest[0] == '[') { const char *q = strchr(rest, ']'); (void)q; } }
        else if (*rest == '.') rest++;
        int w = -1; for (int k = 0; k < nwp; k++) if (!strcmp(wp[k].name, pname)) w = k;
        if (w < 0) {
            if (nwp >= 24) { obj_fail(op->mod, objpath, op, "too many different properties in one object"); failed++; continue; }
            wchar_t wn[80]; widen(pname, wn, 80); uint8_t *prop = struct_prop(cls, fname_of(wn));
            if (!prop || !field_is_property(prop)) { char why[120]; snprintf(why, sizeof why, "the object has no property '%s'", pname); obj_fail(op->mod, objpath, op, why); failed++; continue; }
            int off = *(int *)(prop + 0x44); wchar_t *ex = prop_export_w(prop, (uint8_t *)obj + off, obj); char *u8 = ut_start_text(ex ? wide_to_utf8(ex) : NULL, !*rest && op->kind == UT_SET); free(ex);   // an empty Text exports as nothing (Lost Prospect texts)
            if (!u8) { obj_fail(op->mod, objpath, op, "the current value could not be exported"); failed++; continue; }
            w = nwp++; snprintf(wp[w].name, sizeof wp[w].name, "%s", pname); wp[w].prop = prop; wp[w].off = off; wp[w].text = u8; wp[w].dirty = 0; wp[w].first_op = i; wp[w].nops = 0; wp[w].failed = 0;
        }
        char err[200] = ""; char *nt = NULL;
        if (!*rest && op->kind == UT_SET) {                                             // the whole property: validated like a table value
            char why[100]; const char *kind = prop_kind(wp[w].prop);
            if (!rowval_check(kind, op->value, why, sizeof why)) { snprintf(err, sizeof err, "%s (%s property)", why, kind); }
            else if (!rows_objref_ok(kind, op->value, why, sizeof why)) { snprintf(err, sizeof err, "%s", why); logf_("rows: %s %s: %s", objpath, op->path, why); }
            else nt = _strdup(op->value);
        } else nt = ut_apply(wp[w].text, rest, (UtOp)op->kind, op->value, err, sizeof err);
        if (!nt) { obj_fail(op->mod, objpath, op, err[0] ? err : "could not apply"); failed++; wp[w].failed++; continue; }
        free(wp[w].text); wp[w].text = nt; wp[w].dirty = 1; wp[w].nops++; if (opok) opok[i] = 1;
    }
    for (int k = 0; k < nwp; k++) {
        if (wp[k].dirty) {
            wchar_t *wt = utf8_to_wide(wp[k].text); int ok = wt && prop_import_w(wp[k].prop, (uint8_t *)obj + wp[k].off, obj, wt); free(wt);
            if (ok) { applied += wp[k].nops; for (int i = 0; i < g_objops.count; i++) if (opok && opok[i] && !strcmp(g_objops.a[i].object, objpath) && path_on_prop(g_objops.a[i].path, wp[k].name)) g_rowstat[g_objops.a[i].mod < MODS_MAX ? g_objops.a[i].mod : MODS_MAX].applied++; }
            else { failed += wp[k].nops; for (int i = 0; i < g_objops.count; i++) if (!strcmp(g_objops.a[i].object, objpath)) { if (path_on_prop(g_objops.a[i].path, wp[k].name)) obj_fail(g_objops.a[i].mod, objpath, &g_objops.a[i], "the game did not accept the edited value (the whole property was left unchanged)"); } }
        }
        free(wp[k].text);
    }
    free(opok);
    logf_("rows: data asset %s loaded (thread %lu): %d op(s) applied, %d failed in %lu ms", objpath, GetCurrentThreadId(), applied, failed, (unsigned long)(GetTickCount() - t0));
    rows_write_summary();
}
// Data assets are post-loaded on whichever thread the async loader uses, and the edit may load animation assets (hard references), which Unreal only supports on the game
// thread (off it, loads race and can crash). So off the game thread the edit is queued and applied at the start of the next frame.
typedef struct { void *obj; char path[200]; } ObjQueued;
static ObjQueued g_obj_queue[256]; static int g_obj_queue_n;
static void rows_apply_deferred(void);
static void rows_drain_objects(void) {
    if (g_row_defer_n) rows_apply_deferred();                       // table edits with hard references to objects that were not loaded when the table loaded
    if (!g_obj_queue_n || g_obj_faults >= 3) return;
    ObjQueued todo[256]; int n;
    EnterCriticalSection(&g_lock); n = g_obj_queue_n; memcpy(todo, g_obj_queue, (size_t)n * sizeof todo[0]); g_obj_queue_n = 0; LeaveCriticalSection(&g_lock);
    for (int i = 0; i < n; i++) {
        __try { obj_apply_object(todo[i].obj, todo[i].path); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_obj_faults++; logf_("rows: EXCEPTION 0x%08lX while applying edits to %s (fault %d of 3)", GetExceptionCode(), todo[i].path, g_obj_faults); }
    }
}
static void hk_postload(void *obj) {
    g_orig_postload(obj);
    if (obj) plugins_postload(obj);
    if (!obj || !g_objops.count || g_obj_faults >= 3) return;
    __try {
        if (!rows_guard()) return;
        obj_prepare();
        FNameRaw self = *(FNameRaw *)((uint8_t *)obj + 0x18); int hit = 0;
        for (int k = 0; k < g_obj_names_n; k++) if (g_obj_names[k] == *(uint64_t *)&self) hit = 1;
        if (!hit) return;
        for (int k = 0; k < g_obj_done_n; k++) if (g_obj_done[k] == obj) return;
        char nm[100], pk[200], path[320]; void *outer = *(void **)((uint8_t *)obj + 0x20);
        fname_text(self, nm, sizeof nm); pk[0] = 0; if (outer) fname_text(*(FNameRaw *)((uint8_t *)outer + 0x18), pk, sizeof pk);
        snprintf(path, sizeof path, "%s.%s", pk, nm);
        int known = 0; for (int i = 0; i < g_objops.count; i++) if (!strcmp(g_objops.a[i].object, path)) { known = 1; break; }
        if (!known) return;
        if (g_obj_done_n < 256) g_obj_done[g_obj_done_n++] = obj;
        else { static int told; if (!told) { told = 1; logf_("rows: LIMIT: more than 256 data assets were edited - later ones are no longer remembered, so an asset that reloads may get its ops applied twice (append / insert would duplicate)"); } }
        if (g_game_tid && GetCurrentThreadId() == g_game_tid) obj_apply_object(obj, path);
        else {
            EnterCriticalSection(&g_lock);
            if (g_obj_queue_n < 256) { g_obj_queue[g_obj_queue_n].obj = obj; snprintf(g_obj_queue[g_obj_queue_n].path, sizeof g_obj_queue[0].path, "%s", path); g_obj_queue_n++; }
            LeaveCriticalSection(&g_lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_obj_faults++; logf_("rows: EXCEPTION 0x%08lX while applying data asset edits (fault %d of 3)", GetExceptionCode(), g_obj_faults); }
}

static void rowedit_install(void) {
    g_orig_dt_serialize = (DtSerialize_t)hook(IDX_DT_SERIALIZE, (void *)hk_dt_serialize);
    if (!g_orig_dt_serialize) logf_("rows: UDataTable::Serialize hook NOT installed - rows.json edits will not be applied");
    g_orig_postload = (PostLoad_t)hook(IDX_POSTLOAD, (void *)hk_postload);
    if (!g_orig_postload) logf_("rows: UObject::ConditionalPostLoad hook NOT installed - data asset edits will not be applied");
}

// ----- hot reload of rows.json (LanNative\reload.txt / Mods page / AbsApi::reload_content): re-reads every loaded mod's rows.json and re-applies the edits that are new or changed to the tables that are
// already in memory (the same engine as at load: row_set_q / row_add_q, so every value is validated again). Not reloadable (logged): edits that were REMOVED stay in effect until a restart (the old values are
// not kept), data asset edits (append / insert are not idempotent, and a data asset is edited once when it loads), and a rows.json that fails to parse (the old edits stay).
typedef struct { uint64_t names[64]; void *found[64]; FNameRaw fn[64]; int n; } RowLive;
static void rowlive_visit(void *obj, void *ctx) {                                  // live (non-default) UDataTable objects whose name is one of the wanted table names
    RowLive *r = ctx; uint8_t *o = obj; if ((*(uint32_t *)(o + OFF_OBJ_FLAGS) & 0x30u) != 0) return;
    uint64_t nm = *(uint64_t *)(o + 0x18); int want = -1;
    for (int i = 0; i < r->n; i++) if (r->names[i] == nm) { want = i; break; }
    if (want < 0 || r->found[want]) return;
    void *dt = ((void *(*)(void))(g_base + RVA_CLS_DATATABLE))();
    for (void *c = *(void **)(o + 0x10); c; c = *(void **)((uint8_t *)c + 0x30)) if (c == dt) { r->found[want] = obj; return; }
}
// Game thread, start of a tick (rows_drain_objects): applies the deferred hard-reference edits. ImportText loads the referenced package here, synchronously, outside any
// other load. An edit is dropped if a later op (later mod) set the same field directly in the meantime, so "the later mod wins" still holds.
static void rows_apply_deferred(void) {
    if (g_row_faults >= 3) { g_row_defer_n = 0; return; }
    static struct { int op; uint64_t table; } todo[256]; int n;
    EnterCriticalSection(&g_lock); n = g_row_defer_n; memcpy(todo, g_row_defer, (size_t)n * sizeof todo[0]); g_row_defer_n = 0; LeaveCriticalSection(&g_lock);
    RowLive live; memset(&live, 0, sizeof live);
    for (int i = 0; i < n; i++) { int seen = 0; for (int k = 0; k < live.n; k++) if (live.names[k] == todo[i].table) seen = 1; if (!seen && live.n < 64) { live.names[live.n] = todo[i].table; memcpy(&live.fn[live.n], &todo[i].table, 8); live.n++; } }
    __try { api_scan_objects(rowlive_visit, &live); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("rows: deferred: EXCEPTION 0x%08lX while looking for the tables", GetExceptionCode()); return; }
    DWORD t0 = GetTickCount(); int ok_n = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        int oi = todo[i].op; if (oi < 0 || oi >= g_rowops.count) continue;
        const RowOp *op = &g_rowops.a[oi]; int mod = op->mod < MODS_MAX ? op->mod : MODS_MAX; char why[300] = ""; void *t = NULL;
        for (int k = 0; k < live.n; k++) if (live.names[k] == todo[i].table) t = live.found[k];
        int later = 0; for (int j = oi + 1; j < g_rowops.count && !later; j++) if (!g_rowops.a[j].is_add && !_stricmp(g_rowops.a[j].table, op->table) && !_stricmp(g_rowops.a[j].row, op->row) && !_stricmp(g_rowops.a[j].prop, op->prop)) later = 1;
        if (later) { logf_("rows: deferred %s.%s dropped: a later mod sets the same field", op->row, op->prop); continue; }
        int r = 0;
        if (!t) snprintf(why, sizeof why, "the table %s is no longer in memory", op->table);
        else __try { r = row_set_q(t, op, why, sizeof why); } __except (EXCEPTION_EXECUTE_HANDLER) { g_row_faults++; snprintf(why, sizeof why, "EXCEPTION 0x%08lX (fault %d of 3)", GetExceptionCode(), g_row_faults); r = 0; }
        if (r == 1) { ok_n++; g_rowstat[mod].applied++; }
        else {
            bad++; g_rowstat[mod].failed++; g_rowfail_total++;
            if (g_rowfail_n < 60) snprintf(g_rowfail[g_rowfail_n++], sizeof g_rowfail[0], "%s: set %s.%s (deferred): %s", g_posname[mod], op->row, op->prop, why);
            logf_("rows: deferred %s.%s = %s FAILED: %s", op->row, op->prop, op->text, why);
        }
    }
    logf_("rows: deferred object references applied on the game thread (thread %lu): %d ok, %d failed in %lu ms", GetCurrentThreadId(), ok_n, bad, (unsigned long)(GetTickCount() - t0));
    rows_write_summary();
}
static int rowop_same(const RowOp *a, const RowOp *b) { return a->is_add == b->is_add && a->is_remove == b->is_remove && a->learn == b->learn &&!strcmp(a->table, b->table) && !strcmp(a->row, b->row) && !strcmp(a->src, b->src) && !strcmp(a->prop, b->prop) && !strcmp(a->text ? a->text : "", b->text ? b->text : ""); }
// returns the number of changed or new edits applied, -1 if rows.json could not be reloaded (status gets a one-line summary)
static int rows_reload(char *status, int cap) {
    if (!rows_guard()) { snprintf(status, cap, "rows.json: row editing is off in this build"); return -1; }
    if (g_row_defer_n) rows_apply_deferred();                                            // the deferred list holds indices into the g_rowops that is replaced below
    RowOps nr; ObjOps no; memset(&nr, 0, sizeof nr); memset(&no, 0, sizeof no); int mods = 0;
    for (int pos = 0; pos < MODS_MAX; pos++) {
        int m = -1; for (int i = 0; i < g_mi_n; i++) if (g_mi_pos[i] == pos) m = i;
        if (m < 0) continue;
        wchar_t rj[MAX_PATH]; swprintf(rj, MAX_PATH, L"%ls\\rows.json", g_mi_dir[m]); char *t = mods_read_text(rj, 8u << 20); if (!t) continue;
        JDoc jd; char err[200] = "";
        if (jdoc_parse(&jd, t) != 0) { snprintf(err, sizeof err, "%s", jd.err); jdoc_free(&jd); free(t); logf_("reload: %s rows.json has an error (%s) - ALL rows.json edits stay as they were", g_mi[m].name, err); snprintf(status, cap, "rows.json of %s has an error: %.80s - not reloaded", g_mi[m].name, err); rowops_free(&nr); objops_free(&no); return -1; }
        RowsWarn rw; memset(&rw, 0, sizeof rw); int n = rows_from_json_w(&jd, pos, &nr, &no, err, sizeof err, &rw); jdoc_free(&jd); free(t);
        for (int w = 0; w < rw.n && w < 8; w++) logf_("reload: %s rows.json: %s", g_mi[m].name, rw.line[w]);
        if (n <= 0 && err[0]) { logf_("reload: %s rows.json rejected: %s - ALL rows.json edits stay as they were", g_mi[m].name, err); snprintf(status, cap, "rows.json of %s rejected: %.80s - not reloaded", g_mi[m].name, err); rowops_free(&nr); objops_free(&no); return -1; }
        if (n > 0) mods++;
    }
    int changed = 0, removed = 0, tables = 0; RowLive live; memset(&live, 0, sizeof live);
    for (int i = 0; i < nr.count; i++) {                                              // new or changed edits -> their tables
        int same = 0; for (int k = 0; k < g_rowops.count && !same; k++) if (g_rowops.a[k].mod == nr.a[i].mod && rowop_same(&g_rowops.a[k], &nr.a[i])) same = 1;
        if (same) continue; changed++;
        wchar_t w[ROWS_TABLE_MAX]; widen(nr.a[i].table, w, ROWS_TABLE_MAX); FNameRaw f = fname_of(w); int seen = 0; for (int k = 0; k < live.n; k++) if (live.names[k] == *(uint64_t *)&f) seen = 1;
        if (!seen && live.n < 64) { live.names[live.n] = *(uint64_t *)&f; live.fn[live.n] = f; live.n++; }
    }
    for (int k = 0; k < g_rowops.count; k++) { int still = 0; for (int i = 0; i < nr.count && !still; i++) if (nr.a[i].mod == g_rowops.a[k].mod && rowop_same(&g_rowops.a[k], &nr.a[i])) still = 1; if (!still) removed++; }
    int objchg = no.count != g_objops.count; for (int i = 0; i < no.count && !objchg; i++) if (strcmp(no.a[i].object, g_objops.a[i].object) || strcmp(no.a[i].path, g_objops.a[i].path) || strcmp(no.a[i].value, g_objops.a[i].value) || no.a[i].kind != g_objops.a[i].kind) objchg = 1;
    objops_free(&no);
    rowops_free(&g_rowops); g_rowops = nr; g_op_table_n = -1;                         // rows_prepare() recomputes the table name lists
    int applied = 0, notloaded = 0;
    if (live.n) {
        api_scan_objects(rowlive_visit, &live);
        for (int i = 0; i < live.n; i++) {
            char tn[80]; fname_text(live.fn[i], tn, sizeof tn);
            if (!live.found[i]) { notloaded++; logf_("reload: table %s is not in memory - its edits apply when the game loads it", tn); continue; }
            __try { rows_prepare(); applied += rows_apply_table(live.found[i], live.fn[i], tn); tables++; } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("reload: EXCEPTION 0x%08lX while re-applying table %s", GetExceptionCode(), tn); }
        }
    }
    logf_("reload: rows.json of %d mod(s): %d new or changed edit(s) -> %d table(s) re-applied (%d edits ok, %d table(s) not in memory); %d removed edit(s) stay in effect until a restart%s", mods, changed, tables, applied, notloaded, removed, objchg ? "; data asset edits changed: they only apply at load - restart" : "");
    snprintf(status, cap, "rows.json: %d changed, %d table(s) updated%s%s", changed, tables, removed ? ", removed edits need a restart" : "", objchg ? ", data-asset edits need a restart" : "");
    return changed;
}

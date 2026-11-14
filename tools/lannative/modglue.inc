// mod loader (included by lannative.c): mounts <game folder>\Mods\<ModName>\*.pak after the engine has mounted its own paks
// (hook on FPakPlatformFile::Initialize). Load order = Mods\loadorder.txt (last line wins). Each mod pak gets its own read order
// (modconf.h); a missing .sig is created next to the pak (copy of the game's own), so mod makers ship just the .pak. Never touches stock paks, never writes outside Mods\ and LanNative\.
// -NoMods on the command line skips all of it.
#include "AbsolverSE.h"
#include "manifest.h"
#include "contentreg.h"
#include "modsui.h"
#include "pakshadow.h"
#include "records.h"
// Records (pure code: records.h): config "records = on" (default OFF). Pass 1 collects the flats / templates of every
// enabled mod's rows.json in load order; pass 2 rewrites each rows.json document into plain edits before rows_from_json. Off: documents are read unchanged.
// Known limit: the rows.json hot reload (rowedit.inc) re-reads files WITHOUT the expansion; restart after editing a records file.
static RecCtx g_rec; static int g_rec_on = -1;
static int records_enabled(void) { if (g_rec_on < 0) { char v[16] = ""; wchar_t src[MAX_PATH]; g_rec_on = setting_get("records", L"", 0, v, sizeof v, src) && !_strnicmp(v, "on", 2); logf_("records: %s", g_rec_on ? "ON (flats / templates / base in rows.json)" : "OFF (default; 'records = on' in config.txt enables flats / templates / base in rows.json)"); } return g_rec_on; }
static void toast_show(char lines[][LM_LINE_MAX], int n, unsigned seconds);                          // lanmods.inc
static int content_scan_mod(int pos, const char *folder, const char *modid, const wchar_t *dir, uint64_t *hash);   // content.inc: lists Mods\<mod>\content\<kind>\*.json
static void content_boot_reset(void);                                                                 // content.inc
static void plugin_register_builtin(void);                                                            // plugins.inc: the loader's own "Content" plugin (only when some mod ships content)
#define RVA_PAK_MOUNT 0x193EF70u        // FPakPlatformFile::Mount(this, const TCHAR *pak, uint32 order, const TCHAR *mountpoint)
#define PAK_SIGNED_OFFSET 0x20          // FPakPlatformFile::bSigned (offset from the PDB struct)
#define RVA_PAK_FIND  0x193A5B0u        // FPakEntry *FPakPlatformFile::FindFileInPakFiles(this, const TCHAR *path, FPakFile **out_pak): first pak in read-order that has the file
#define IDX_PAKINIT 11
typedef void *(*PakFind_t)(void *self, const wchar_t *path, void **out_pak);
static void *g_pak_self;                 // the FPakPlatformFile, kept for rows_objref_ok (is a package file in any pak?)
typedef uint8_t (*PakInit_t)(void *self, void *lower, const wchar_t *cmdline);
typedef uint8_t (*PakMount_t)(void *self, const wchar_t *path, uint32_t order, const wchar_t *mountpoint);
static PakInit_t g_orig_pakinit;
// what the F1 "Mods" page shows (modsui.h): one ModInfo per mod folder (same index as the ModList), filled below while the mods load; g_mi_dir keeps each mod's folder for content hot reload
static ModInfo g_mi[MODS_MAX]; static int g_mi_n; static wchar_t g_mi_dir[MODS_MAX][MAX_PATH]; static int g_mi_pos[MODS_MAX];   // g_mi_pos = load position (counts mounted mods only), -1 = not loaded
static char g_mi_order[1400]; static int g_mi_pakc, g_mi_pakc_real, g_mi_rowc, g_mi_objc, g_mi_split, g_mi_rows_mods;
// content warnings (F2 Mods page "WARNING:" lines, mods.txt, log): pak shadowing, superseded paks, equip conflicts. Filled while the mods load and when items are granted.
static char g_mi_warn[MUI_WARN][240]; static int g_mi_warn_n;
static void mods_warn(const char *fmt, ...) {
    char t[240]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    logf_("mods: WARNING %s", t);
    for (int i = 0; i < g_mi_warn_n; i++) if (!strcmp(g_mi_warn[i], t)) return;
    if (g_mi_warn_n < MUI_WARN) snprintf(g_mi_warn[g_mi_warn_n++], sizeof g_mi_warn[0], "%s", t);
}
static int mods_dir_has_pak(const wchar_t *dir) {
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*.pak", dir); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0; do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found = 1; while (!found && FindNextFileW(h, &fd));
    FindClose(h); return found;
}
static uint64_t g_mods_fp;               // fingerprint of the enabled mods (order + pak index hashes); 0 = no mods
static int g_mods_active;                // number of enabled mods that mounted at least one pak

static int cmdline_has_flag(const wchar_t *cl, const wchar_t *flag) {     // "-flag" or "/flag" as a whole token, case-insensitive
    size_t n = wcslen(flag);
    for (const wchar_t *p = cl; p && *p; p++) {
        int left = p == cl || p[-1] == L' ' || p[-1] == L'\t' || p[-1] == L'"';
        if (left && (p[0] == L'-' || p[0] == L'/') && _wcsnicmp(p + 1, flag, n) == 0 &&
            (p[1 + n] == 0 || p[1 + n] == L' ' || p[1 + n] == L'"' || p[1 + n] == L'\t')) return 1;
    }
    return 0;
}
static void u8_to_w(const char *s, wchar_t *w, int cap) { if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap)) w[0] = 0; }
static void w_to_u8(const wchar_t *w, char *s, int cap) { if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, cap, NULL, NULL)) s[0] = 0; }
static int cmp_cstr(const void *a, const void *b) { return _stricmp(*(char *const *)a, *(char *const *)b); }

static char *mods_read_text(const wchar_t *path, size_t max) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL); char *buf = NULL;
    if (size != INVALID_FILE_SIZE && size <= max && (buf = malloc(size + 1))) {
        DWORD got = 0; if (!ReadFile(h, buf, size, &got, NULL)) got = 0; buf[got] = 0;
        if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) memmove(buf, buf + 3, got - 2);
    }
    CloseHandle(h); return buf;
}

static void mods_mount_all(void *self) {
    const wchar_t *cl = GetCommandLineW();
    if (cmdline_has_flag(cl, L"nomods")) { logf_("mods: -NoMods on the command line - no mods are loaded"); return; }
    wchar_t root[MAX_PATH]; wcscpy(root, g_dir);                           // <game>\Absolver\Binaries\Win64\LanNative\ -> <game>
    for (int i = 0; i < 4; i++) {
        size_t n = wcslen(root); while (n && root[n - 1] == L'\\') root[--n] = 0;
        wchar_t *sl = wcsrchr(root, L'\\'); if (!sl) { logf_("mods: cannot derive the game folder from the LanNative folder"); return; } *sl = 0;
    }
    wchar_t mods[MAX_PATH]; swprintf(mods, MAX_PATH, L"%ls\\Mods", root);
    if (GetFileAttributesW(mods) == INVALID_FILE_ATTRIBUTES) {          // first run without the launcher: make the folder so people can find it
        if (CreateDirectoryW(mods, NULL)) logf_("mods: created the Mods folder (put each mod in <game folder>\\Mods\\<ModName>\\ as a .pak)");
        else { logf_("mods: no Mods folder and it could not be created (error %lu)", GetLastError()); return; }
    }
    static const uint8_t mount_expect[16] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55 };
    if (memcmp(g_base + RVA_PAK_MOUNT, mount_expect, 16) != 0) { logf_("mods: FPakPlatformFile::Mount bytes differ from this build - mods NOT loaded"); return; }
    if (*((uint8_t *)self + PAK_SIGNED_OFFSET) > 1) { logf_("mods: unexpected FPakPlatformFile layout - mods NOT loaded"); return; }
    wchar_t sigsrc[MAX_PATH]; swprintf(sigsrc, MAX_PATH, L"%ls\\Absolver\\Content\\Paks\\pakchunk0-WindowsNoEditor.sig", root);
    int have_sigsrc = GetFileAttributesW(sigsrc) != INVALID_FILE_ATTRIBUTES;
    if (!have_sigsrc) logf_("mods: the game's own pakchunk0 .sig was not found - mod paks without a .sig will crash the game, so they are skipped");
    PakMount_t mount = (PakMount_t)(g_base + RVA_PAK_MOUNT);

    // 1. folders -> ModList (loadorder.txt, synced with what is on disk)
    char *names[MODS_MAX]; int nnames = 0;
    char longnames[8][64]; int longlen[8], nlong = 0;                       // folders whose name exceeds the limit: skipped, reported once the warning list is reset below
    {
        wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*", mods); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && nnames < MODS_MAX) {
                    char u[1024]; w_to_u8(fd.cFileName, u, sizeof u);
                    if (u[0] && strlen(u) < MOD_NAME_MAX) names[nnames++] = _strdup(u);
                    else if (u[0]) {
                        if (nlong < 8) {
                            size_t cut = 40; while (cut && ((unsigned char)u[cut] & 0xC0) == 0x80) cut--;      // do not split a UTF-8 sequence
                            snprintf(longnames[nlong], sizeof longnames[0], "%.*s...", (int)cut, u); longlen[nlong] = (int)strlen(u);
                        }
                        nlong++;
                    }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    qsort(names, nnames, sizeof *names, cmp_cstr);
    wchar_t orderpath[MAX_PATH]; swprintf(orderpath, MAX_PATH, L"%ls\\loadorder.txt", mods);
    static ModList list; char *text = mods_read_text(orderpath, 1 << 20); modlist_parse(&list, text ? text : ""); free(text);
    int added = modlist_sync(&list, (const char **)names, nnames);
    static char rendered[MODS_MAX * (MOD_NAME_MAX + 4) + 512]; int rn = modlist_render(&list, rendered, sizeof rendered);
    char *old = mods_read_text(orderpath, 1 << 20);
    if (rn > 0 && (!old || strcmp(old, rendered) != 0)) {
        HANDLE h = CreateFileW(orderpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, rendered, (DWORD)rn, &w, NULL); CloseHandle(h); logf_("mods: loadorder.txt updated (%d new folder(s) added at the end)", added); }
        else logf_("mods: could not write loadorder.txt (error %lu)", GetLastError());
    }
    for (int i = 0; i < nnames; i++) if (modlist_find(&list, names[i]) < 0) logf_("mods: LIMIT: folder '%s' is NOT in the load order and will not load (at most %d mods, folder names shorter than %d characters)", names[i], MODS_MAX, MOD_NAME_MAX);
    free(old); for (int i = 0; i < nnames; i++) free(names[i]);
    logf_("mods: %d mod folder(s), %d listed in loadorder.txt", nnames, list.count);

    // 1b. manifests (mod.json) -> which mods load, and in which order (requires / after on top of loadorder.txt)
    static Manifest mfs[MODS_MAX]; static MfResult mres; int men[MODS_MAX]; char skipped_lines[8][LM_LINE_MAX]; int nskipped = 0;
    for (int m = 0; m < list.count; m++) {
        wchar_t wname[MOD_NAME_MAX * 2], mj[MAX_PATH]; u8_to_w(list.mods[m].name, wname, MOD_NAME_MAX * 2); swprintf(mj, MAX_PATH, L"%ls\\%ls\\mod.json", mods, wname);
        char *jt = mods_read_text(mj, 65536); char merr[200] = "";
        if (manifest_parse(jt, list.mods[m].name, &mfs[m], merr, sizeof merr) != 0) logf_("mods: %s: %s (using the folder name as id and no dependencies)", list.mods[m].name, merr);
        free(jt); men[m] = list.mods[m].enabled;
    }
    mf_resolve(mfs, men, list.count, ABS_API_VERSION, &mres);
    int has_pak[MODS_MAX] = {0}; g_mi_warn_n = 0;
    for (int i = 0; i < nlong && i < 8; i++) mods_warn("mod folder '%s' (%d bytes) exceeds the %d-character name limit - skipped", longnames[i], longlen[i], MOD_NAME_MAX - 1);
    if (nlong > 8) mods_warn("%d more mod folder(s) exceed the %d-character name limit - skipped", nlong - 8, MOD_NAME_MAX - 1);
    for (int m = 0; m < list.count && m < MODS_MAX; m++) if (mres.status[m] == MF_OK && mfs[m].nsup) {          // only a mod that supersedes something needs to know whether it ships a pak
        wchar_t wn[MOD_NAME_MAX * 2], dd[MAX_PATH]; u8_to_w(list.mods[m].name, wn, MOD_NAME_MAX * 2); swprintf(dd, MAX_PATH, L"%ls\\%ls", mods, wn); has_pak[m] = mods_dir_has_pak(dd);
    }
    content_boot_reset();
    memset(g_mi, 0, sizeof g_mi); g_mi_n = list.count < MODS_MAX ? list.count : MODS_MAX;
    for (int i = 0; i < g_mi_n; i++) {
        ModInfo *mi = &g_mi[i]; snprintf(mi->name, sizeof mi->name, "%s", list.mods[i].name); snprintf(mi->id, sizeof mi->id, "%s", mfs[i].id); snprintf(mi->version, sizeof mi->version, "%s", mfs[i].version);
        mi->affects_gameplay = mfs[i].affects_gameplay; mi->status = mres.status[i] == MF_OFF ? MI_OFF : mres.status[i] == MF_SKIPPED ? MI_SKIPPED : MI_ON; snprintf(mi->reason, sizeof mi->reason, "%s", mres.reason[i]); g_mi_pos[i] = -1;
    }
    {
        char ord[1400] = ""; size_t ow = 0;
        for (int k = 0; k < mres.norder && ow < sizeof ord; k++) ow += snprintf(ord + ow, sizeof ord - ow, "%s%s", k ? ", " : "", list.mods[mres.order[k]].name);
        logf_("mods: load order (%d mod(s)): %s", mres.norder, mres.norder ? ord : "(none)"); snprintf(g_mi_order, sizeof g_mi_order, "%s", ord);
        for (int i = 0; i < list.count; i++) if (mres.status[i] == MF_SKIPPED) {
            logf_("mods: SKIPPED %s: %s", list.mods[i].name, mres.reason[i]);
            if (nskipped < 7) { nskipped++; snprintf(skipped_lines[nskipped], LM_LINE_MAX, "%s: %s", list.mods[i].name, mres.reason[i]); }
        }
        if (nskipped) { snprintf(skipped_lines[0], LM_LINE_MAX, "MODS SKIPPED (%d) - details in LanNative\\mods.txt", nskipped); toast_show(skipped_lines, nskipped + 1, 90); }
    }

    // 2. mount, in load order (position counts mounted mods only)
    static PakEntry *lists[MODS_MAX * 4]; static ModFiles sets[MODS_MAX * 4]; static char set_name[MODS_MAX * 4][96]; static unsigned set_order[MODS_MAX * 4]; int nsets = 0;
    uint64_t fp = mods_fingerprint_begin(); int position = 0, mounted_mods = 0, mounted_paks = 0;
    int rows_mods = 0; rowops_free(&g_rowops); objops_free(&g_objops);
    rec_free(&g_rec);
    if (records_enabled()) {                                                     // records pass 1: flats + templates of every mod that loads, in load order (a later one wins)
        static RowsWarn rcw; memset(&rcw, 0, sizeof rcw); int nrec = 0, nmods = 0;
        for (int oi = 0; oi < mres.norder; oi++) {
            const ModEntry *me = &list.mods[mres.order[oi]]; wchar_t wn[MOD_NAME_MAX * 2], rj[MAX_PATH]; u8_to_w(me->name, wn, MOD_NAME_MAX * 2); swprintf(rj, MAX_PATH, L"%ls\\%ls\\rows.json", mods, wn);
            char *t = mods_read_text(rj, 8u << 20); if (!t) continue; JDoc jd;
            if (jdoc_parse(&jd, t) == 0) { int k = rec_collect(&g_rec, &jd, me->name, &rcw); if (k) { nrec += k; nmods++; } jdoc_free(&jd); }
            free(t);
        }
        for (int w = 0; w < rcw.n && w < 8; w++) logf_("mods: %s", rcw.line[w]);
        logf_("records: pass 1: %d flat(s) and %d template(s) from %d mod(s) (%d definition(s), %d problem(s))", g_rec.nf, g_rec.nt, nmods, nrec, rcw.n);
        for (int i = 0; i < g_rec.nf; i++) if (g_rec.f[i].overrides) logf_("records: flat %s = %s from %s overrides %s", g_rec.f[i].name, g_rec.f[i].val, g_rec.f[i].mod, g_rec.f[i].prev);
        for (int i = 0; i < g_rec.nt; i++) if (g_rec.t[i].overrides) logf_("records: template %s from %s overrides %s", g_rec.t[i].id, g_rec.t[i].mod, g_rec.t[i].prev);
    }
    uint64_t gp_fp = mods_fingerprint_begin(); const char *gpn[MODS_MAX]; int ngp = 0;      // gameplay mods only: what the LAN mod check compares
    static char report[16384]; int rl = 0;
    // guarded append: snprintf returns the untruncated length, so a bare "rl += snprintf(...)" would run past the buffer once the report fills up (many mods / conflicts)
#define RPT(...) do { if (rl < (int)sizeof report - 1) { int n_ = snprintf(report + rl, sizeof report - rl, __VA_ARGS__); if (n_ > 0) rl += n_; if (rl > (int)sizeof report - 1) rl = (int)sizeof report - 1; } } while (0)
    RPT("Absolver mods loaded by LanNative (last in the list wins)\r\n");
    {   char ord[1400] = ""; size_t ow = 0; for (int k = 0; k < mres.norder && ow < sizeof ord; k++) ow += snprintf(ord + ow, sizeof ord - ow, "%s%s", k ? ", " : "", list.mods[mres.order[k]].name);
        RPT("Load order (requires / after applied on top of loadorder.txt): %s\r\n", mres.norder ? ord : "(none)"); }
    for (int i = 0; i < nlong && i < 8; i++) RPT("[SKIPPED] %s: exceeds the %d-character name limit\r\n", longnames[i], MOD_NAME_MAX - 1);
    if (nlong > 8) RPT("[SKIPPED] %d more folder(s): name exceeds the %d-character limit\r\n", nlong - 8, MOD_NAME_MAX - 1);
    for (int i = 0; i < list.count; i++) {
        if (mres.status[i] == MF_OFF) { logf_("mods: [off] %s", list.mods[i].name); RPT("[off] %s\r\n", list.mods[i].name); }
        else if (mres.status[i] == MF_SKIPPED) RPT("[SKIPPED] %s: %s\r\n", list.mods[i].name, mres.reason[i]);
    }
    for (int oi = 0; oi < mres.norder; oi++) {
        int m = mres.order[oi];
        const ModEntry *me = &list.mods[m];
        wchar_t wname[MOD_NAME_MAX * 2], dir[MAX_PATH]; u8_to_w(me->name, wname, MOD_NAME_MAX * 2); swprintf(dir, MAX_PATH, L"%ls\\%ls", mods, wname);
        char *paks[64]; int np = 0;
        {
            wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*.pak", dir); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && np < 64) { char u[MAX_PATH * 2]; w_to_u8(fd.cFileName, u, sizeof u); paks[np++] = _strdup(u); }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        qsort(paks, np, sizeof *paks, cmp_cstr);
        char *rows_text = NULL; int rows_big = 0; { wchar_t rj[MAX_PATH]; swprintf(rj, MAX_PATH, L"%ls\\rows.json", dir); rows_text = mods_read_text(rj, 8u << 20);
          WIN32_FILE_ATTRIBUTE_DATA fa; if (!rows_text && GetFileAttributesExW(rj, GetFileExInfoStandard, &fa)) rows_big = fa.nFileSizeHigh || fa.nFileSizeLow > (8u << 20); }
        if (rows_big) { mods_warn("%s/rows.json is larger than 8 MB and was not read", me->name); RPT("[FAILED]  %s/rows.json  larger than 8 MB, not read\r\n", me->name); }
        wchar_t plug_dll[MAX_PATH]; swprintf(plug_dll, MAX_PATH, L"%ls\\plugin.dll", dir); int has_plugin = GetFileAttributesW(plug_dll) != INVALID_FILE_ATTRIBUTES;
        uint64_t chash = 0; int ncf = content_scan_mod(position, me->name, mfs[m].id, dir, &chash);
        if (m < g_mi_n) { g_mi[m].paks_total = np; g_mi[m].content_files = ncf; wcsncpy(g_mi_dir[m], dir, MAX_PATH - 1); g_mi_dir[m][MAX_PATH - 1] = 0; }
        if (!np && !rows_text && !has_plugin && !ncf) {
            if (m < g_mi_n) g_mi[m].status = MI_EMPTY;
            if (rows_big) logf_("mods: %s has nothing else to load (skipped)", me->name);
            else logf_("mods: %s has no .pak files, no rows.json, no content\\ files and no plugin.dll (skipped)", me->name); RPT("[empty] %s\r\n", me->name); continue; }
        int cosmetic = 0;
        cosmetic = !mfs[m].affects_gameplay;
        int ok_paks = 0;
        for (int k = 0; k < np; k++) {
            { int by = mf_pak_superseded(mfs, &mres, has_pak, list.count, m, paks[k]);
              if (by >= 0) {
                  mods_warn("%s/%s is superseded by %s (its mod.json 'supersedes') - NOT mounted", me->name, paks[k], list.mods[by].name);
                  RPT("[SUPERSEDED] %s/%s  not mounted: %s supersedes it\r\n", me->name, paks[k], list.mods[by].name); continue; } }
            wchar_t wpak[MAX_PATH * 2], fullbs[MAX_PATH], full[MAX_PATH]; u8_to_w(paks[k], wpak, MAX_PATH * 2);
            swprintf(fullbs, MAX_PATH, L"%ls\\%ls", dir, wpak); wcscpy(full, fullbs);
            for (wchar_t *c = full; *c; c++) if (*c == L'\\') *c = L'/';       // the engine's own pak paths use forward slashes
            unsigned char ih[20] = {0}; PakEntry *entries = NULL; int ne = -1;
            FILE *f = _wfopen(fullbs, L"rb");
            int fok = f ? modpak_footer_ok(f) : 0;
            if (fok != 1) {                                  // the engine's mount code is not safe to call on a file that is not a pak
                const char *why = !f ? "cannot be read" : fok < 0 ? "index outside the file" : "bad footer"; if (f) fclose(f);
                mods_warn("%s/%s is not a valid pak file (%s) - NOT mounted", me->name, paks[k], why);
                RPT("[FAILED] %s/%s  not a valid pak file (%s)\r\n", me->name, paks[k], why); continue;
            }
            { ne = modpak_read(f, &entries, ih); if (ne < 0) { _fseeki64(f, 0, SEEK_END); int64_t sz = _ftelli64(f); memcpy(ih, &sz, 8); } fclose(f); }
            uint32_t order = mod_read_order(position, k);
            // The engine's pak cache opens <pak>.sig for EVERY pak and crashes on a missing one (FPakPrecacher::RegisterPakFile).
            // The game accepts the .sig of any of its own paks for any mod pak, so a missing one is created here, inside the mod's own folder.
            wchar_t sigpath[MAX_PATH]; wcscpy(sigpath, fullbs); { size_t L = wcslen(sigpath); if (L > 4) wcscpy(sigpath + L - 4, L".sig"); }
            int sig_ok = GetFileAttributesW(sigpath) != INVALID_FILE_ATTRIBUTES;
            if (!sig_ok && have_sigsrc) { sig_ok = CopyFileW(sigsrc, sigpath, TRUE); logf_("mods: %s/%s had no .sig - %s", me->name, paks[k], sig_ok ? "created one (copy of the game's own)" : "could not create one"); }
            uint8_t r = 0;
            if (!sig_ok) logf_("mods: %s/%s SKIPPED (no .sig and none could be created)", me->name, paks[k]);
            else __try { r = mount(self, full, order, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mods: EXCEPTION 0x%08lX while mounting %s", GetExceptionCode(), paks[k]); }
            logf_("mods: %s/%s -> %s (read order %u, %s)", me->name, paks[k], r ? "mounted" : "NOT mounted (invalid pak?)", order, ne >= 0 ? "index readable" : "index not readable");
            RPT("%s %s/%s  order %u  %d files\r\n", r ? "[on]    " : "[FAILED]", me->name, paks[k], order, ne);
            if (r) { ok_paks++; mounted_paks++; fp = mods_fingerprint_add(fp, me->name, ih); if (!cosmetic) gp_fp = mods_fingerprint_add(gp_fp, me->name, ih); }
            if (r && ne > 0 && nsets < MODS_MAX * 4) { lists[nsets] = entries; sets[nsets].entries = entries; sets[nsets].count = ne; sets[nsets].mod = position; snprintf(set_name[nsets], sizeof set_name[0], "%s/%s", me->name, paks[k]); set_order[nsets] = order; nsets++; }
            else free(entries);
        }
        for (int k = 0; k < np; k++) free(paks[k]);
        int ok_rows = 0;
        if (rows_text) {
            JDoc jd; char rerr[200] = ""; int nops = 0, rec_defs_only = 0; static RowsWarn rw; memset(&rw, 0, sizeof rw);
            if (jdoc_parse(&jd, rows_text) != 0) { snprintf(rerr, sizeof rerr, "%s", jd.err); }
            else {
                rec_defs_only = records_enabled() && jd.count > 0 && jd.n[0].t == J_OBJ && (jfind(&jd, 0, "flats") >= 0 || jfind(&jd, 0, "templates") >= 0);   // a file of definitions alone is fine
                if (rec_uses(&jd)) {                                                         // records pass 2: flats / templates / base -> plain edits (in place)
                    if (records_enabled()) { int ch = rec_expand(&g_rec, &jd, me->name, &rw); logf_("records: %s rows.json: %d edit(s) / field(s) expanded", me->name, ch); }
                    else logf_("mods: %s rows.json uses flats / templates / base but 'records' is OFF: those fields are read literally and will be skipped", me->name);
                }
                nops = rows_from_json_w(&jd, position, &g_rowops, &g_objops, rerr, sizeof rerr, &rw); jdoc_free(&jd);
            }
            for (int w = 0; w < rw.n && w < 8; w++) { logf_("mods: %s rows.json: %s", me->name, rw.line[w]); RPT("[WARN]    %s/rows.json  %s\r\n", me->name, rw.line[w]); if (rl >= (int)sizeof report) rl = (int)sizeof report - 1; }
            if (rw.n) mods_warn("%s/rows.json: %d field(s) skipped (bad name or value), the rest loads - see the log or mods.txt", me->name, rw.n);
            if (nops > 0) {
                ok_rows = 1; rows_mods++; unsigned char ih[20]; uint64_t h1 = fnv64(0xCBF29CE484222325ull, rows_text, strlen(rows_text)), h2 = fnv64(h1, "rows", 4);
                memcpy(ih, &h1, 8); memcpy(ih + 8, &h2, 8); uint32_t h3 = (uint32_t)(h1 ^ h2); memcpy(ih + 16, &h3, 4);
                fp = mods_fingerprint_add(fp, me->name, ih); if (!cosmetic) gp_fp = mods_fingerprint_add(gp_fp, me->name, ih);
                logf_("mods: %s rows.json: %d edit(s) read (table rows and data asset ops)", me->name, nops); if (m < g_mi_n) g_mi[m].rows_ops = nops;
                RPT("[rows]    %s/rows.json  %d edits\r\n", me->name, nops);
            } else if (rec_defs_only && !rerr[0]) {
                logf_("mods: %s rows.json: flats / templates only (no edits of its own)", me->name);
                RPT("[records] %s/rows.json  flats / templates only\r\n", me->name);
            } else {
                logf_("mods: %s rows.json IGNORED: %s", me->name, rerr[0] ? rerr : "it holds no edits");
                RPT("[FAILED]  %s/rows.json  %s\r\n", me->name, rerr[0] ? rerr : "no edits");
            }
            free(rows_text); rows_text = NULL;
        }
        int ok_content = 0;
        if (ncf > 0) {
            ok_content = 1; unsigned char ih[20] = {0}; uint64_t h2 = fnv64(chash, "content", 7); memcpy(ih, &chash, 8); memcpy(ih + 8, &h2, 8); uint32_t h3 = (uint32_t)(chash ^ h2); memcpy(ih + 16, &h3, 4);
            fp = mods_fingerprint_add(fp, me->name, ih); if (!cosmetic) gp_fp = mods_fingerprint_add(gp_fp, me->name, ih);
            logf_("mods: %s content: %d file(s) (id %s)", me->name, ncf, mfs[m].id);
            RPT("[content] %s/content  %d file(s)\r\n", me->name, ncf);
        }
        int ok_plugin = 0;
        if (has_plugin && plugin_register(mods, me->name, plug_dll, dir)) {          // code: only with the player's OK in plugins.txt; its bytes are part of the fingerprint
            ok_plugin = 1; if (m < g_mi_n) g_mi[m].plugin = MIP_ALLOWED; unsigned char ih[20] = {0}; FILE *pf = _wfopen(plug_dll, L"rb"); uint64_t h1 = 0xCBF29CE484222325ull; uint32_t sz = 0;
            if (pf) { unsigned char buf[65536]; size_t k; while ((k = fread(buf, 1, sizeof buf, pf)) > 0) { h1 = fnv64(h1, buf, k); sz += (uint32_t)k; } fclose(pf); }
            uint64_t h2 = fnv64(h1, "plugin", 6); memcpy(ih, &h1, 8); memcpy(ih + 8, &h2, 8); memcpy(ih + 16, &sz, 4);
            fp = mods_fingerprint_add(fp, me->name, ih); if (!cosmetic) gp_fp = mods_fingerprint_add(gp_fp, me->name, ih);
            RPT("[plugin]  %s/plugin.dll  allowed\r\n", me->name);
        } else if (has_plugin) { if (m < g_mi_n) g_mi[m].plugin = MIP_BLOCKED; RPT("[BLOCKED] %s/plugin.dll  not allowed in plugins.txt\r\n", me->name); }
        if (m < g_mi_n) g_mi[m].paks_ok = ok_paks;
        if (ok_paks || ok_rows || ok_plugin || ok_content) {
            if (m < g_mi_n) g_mi_pos[m] = position;
            strcpy(g_posname[position], me->name); mounted_mods++; position++;
            if (cosmetic) logf_("mods: %s is marked cosmetic (mod.json: affects_gameplay false) - not part of the LAN mod check", me->name);
            else if (ngp < MODS_MAX) gpn[ngp++] = me->name;
        }
    }
    // 3. conflicts between mods
    static Conflict cf[256]; int real = 0; int nc = mods_conflicts_ex(sets, nsets, cf, 256, &real); int shown = nc < 256 ? nc : 256;      // real = ALL different-content files, even past the 256 stored
    logf_("mods: %d mod(s) mounted (%d pak(s)); %d file(s) shipped by more than one mod (%d with different content)", mounted_mods, mounted_paks, nc, real); g_mi_pakc = nc; g_mi_pakc_real = real;
    RPT("\r\nConflicts (same file in more than one mod; the later mod wins):\r\n");
    int same_shown = 0;
    for (int i = 0; i < shown; i++) {
        const char *wn = cf[i].winner < mounted_mods ? g_posname[cf[i].winner] : "?", *ln = cf[i].loser < mounted_mods ? g_posname[cf[i].loser] : "?";
        if (cf[i].same_content && ++same_shown > 12) continue;                      // identical shared files: the first 12 are logged, the rest only counted
        logf_("mods: conflict %s: %s overrides %s%s", cf[i].path, wn, ln, cf[i].same_content ? " (identical content)" : "");
        if (rl < (int)sizeof report - 700) RPT("%s: %s overrides %s%s\r\n", cf[i].path, wn, ln, cf[i].same_content ? " (identical content)" : "");
    }
    if (nc == 0) RPT("none\r\n");
    if (nc - real > 12) { logf_("mods: ... and %d more file(s) with identical content in several mods (not listed)", nc - real - 12); RPT("... and %d more identical file(s) not listed\r\n", nc - real - 12); }
    // pak shadowing: the SAME asset path with DIFFERENT content in two mounted paks - only the pak with the higher read order is read
    {
        static ShadowPair shp[PSH_MAX_PAIRS]; int nshp = pak_shadow_pairs(sets, nsets, shp, PSH_MAX_PAIRS); int shshown = nshp < PSH_MAX_PAIRS ? nshp : PSH_MAX_PAIRS;
        RPT("\r\nPak shadowing (same asset path, DIFFERENT content, in two mounted paks; only the pak with the higher read order is read):\r\n");
        for (int i = 0; i < shshown; i++) {
            char t[400]; pak_shadow_text(&shp[i], set_name[shp[i].loser], set_order[shp[i].loser], set_name[shp[i].winner], set_order[shp[i].winner], t, sizeof t);
            mods_warn("%s", t);
            for (int d = 0; d < shp[i].ndiff && d < 3; d++) logf_("mods:   shadowed file: %s", shp[i].diff_paths[d]);
            RPT("WARNING %s\r\n", t);
        }
        if (nshp > shshown) mods_warn("%d more pak pair(s) shadow each other (see mods.txt)", nshp - shshown);
        if (!nshp) RPT("none\r\n"); else RPT("Rule: ship ONE attacks-table pak per install (mod.json 'supersedes' drops the other mod's copy; 'after' puts a mod later in the load order so its paks win).\r\n");
    }
    if (records_enabled()) { static char rr[4096]; rec_report(&g_rec, rr, sizeof rr); logf_("records: %d edit(s) inherited %d field(s), %d flat reference(s), %d edit(s) / %d field(s) skipped", g_rec.inherited, g_rec.fields_inherited, g_rec.subs, g_rec.skipped_edits, g_rec.skipped_fields);
        RPT("\r\n%s", rr); if (rl >= (int)sizeof report) rl = (int)sizeof report - 1; }
    // row edits (rows.json): the same field set by more than one mod
    {
        static RowConflict rcf[128]; int nrc = rows_conflicts(&g_rowops, rcf, 128); int rshown = nrc < 128 ? nrc : 128; int rdiff = 0;
        for (int i = 0; i < rshown; i++) if (!rcf[i].same) rdiff++;
        logf_("mods: %d row edit(s) from %d mod(s); %d field(s) are set by more than one mod (%d with different values)", g_rowops.count, rows_mods, nrc, rdiff); g_mi_rowc = nrc; g_mi_rows_mods = rows_mods;
        RPT("\r\nRow edits (rows.json): %d edits from %d mod(s)\r\nRow edit conflicts (same field set by several mods; the later mod wins):\r\n", g_rowops.count, rows_mods);
        for (int i = 0; i < rshown; i++) {
            logf_("mods: row conflict %s.%s.%s: %s then %s%s", rcf[i].table, rcf[i].row, rcf[i].prop, g_posname[rcf[i].first_mod], g_posname[rcf[i].last_mod], rcf[i].same ? " (same value)" : "");
            if (rl < (int)sizeof report - 700) RPT("%s.%s.%s: %s then %s%s\r\n", rcf[i].table, rcf[i].row, rcf[i].prop, g_posname[rcf[i].first_mod], g_posname[rcf[i].last_mod], rcf[i].same ? " (same value)" : "");
        }
        if (!nrc) RPT("none\r\n");
        static ObjConflict ocf[128]; int noc = objops_conflicts(&g_objops, ocf, 128); int oshown = noc < 128 ? noc : 128;
        logf_("mods: %d data asset op(s); %d value(s) are set by more than one mod", g_objops.count, noc); g_mi_objc = noc;
        RPT("\r\nData asset ops (object edits): %d ops (appends and inserts from several mods combine; only a 'set' of the same path conflicts)\r\n", g_objops.count);
        for (int i = 0; i < oshown; i++) {
            logf_("mods: data asset conflict %s %s: %s then %s%s", ocf[i].object, ocf[i].path, g_posname[ocf[i].first_mod], g_posname[ocf[i].last_mod], ocf[i].same ? " (same value)" : "");
            if (rl < (int)sizeof report - 700) RPT("%s %s: %s then %s%s\r\n", ocf[i].object, ocf[i].path, g_posname[ocf[i].first_mod], g_posname[ocf[i].last_mod], ocf[i].same ? " (same value)" : "");
        }
        if (!noc) RPT("no conflicts\r\n");
    }
    // split assets: the dangerous kind (two mods each serve part of one asset -> mixed versions -> crash at load)
    static SplitAsset sp[128]; int nsp = mods_split_assets(sets, nsets, sp, 128); int spshown = nsp < 128 ? nsp : 128;
    g_mi_split = nsp; logf_("mods: %d asset(s) would be served half from one mod and half from another%s", nsp, nsp ? " - THE GAME IS LIKELY TO CRASH when it loads them; disable or reorder one of the mods" : "");
    RPT("\r\nSplit assets (parts served by different mods; likely to crash the game):\r\n");
    for (int i = 0; i < spshown; i++) {
        char who[400] = ""; for (int k = 0; k < sp[i].nmods; k++) { size_t w = strlen(who); snprintf(who + w, sizeof who - w, "%s%s", k ? " + " : "", sp[i].mods[k] < mounted_mods ? g_posname[sp[i].mods[k]] : "?"); }
        logf_("mods: SPLIT ASSET %s: %s", sp[i].base, who);
        if (rl < (int)sizeof report - 700) RPT("%s: %s\r\n", sp[i].base, who);
    }
    if (nsp == 0) RPT("none\r\n");

    // 4. ask the engine itself which pak serves each conflicting file: proves the mount worked and the order is what the report says
    {
        static const uint8_t find_expect[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55 };
        if (memcmp(g_base + RVA_PAK_FIND, find_expect, 16) == 0) {
            PakFind_t find = (PakFind_t)(g_base + RVA_PAK_FIND); int checked = 0, good = 0;
            for (int i = 0; i < shown && checked < 16; i++) {
                if (cf[i].same_content || cf[i].winner >= mounted_mods) continue;
                wchar_t wp[600], wpath[600]; u8_to_w(cf[i].path, wp, 600); swprintf(wpath, 600, L"../../../%ls", wp);
                void *pak = NULL; void *entry = NULL;
                __try { entry = find(self, wpath, &pak); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mods: verify EXCEPTION 0x%08lX", GetExceptionCode()); break; }
                char served[MAX_PATH * 2] = "(none)";
                if (entry && pak) { const wchar_t *fn = *(const wchar_t **)pak; if (fn && *(int *)((uint8_t *)pak + 8) > 0) w_to_u8(fn, served, sizeof served); }
                char want[MOD_NAME_MAX + 8]; snprintf(want, sizeof want, "/Mods/%s/", g_posname[cf[i].winner]);
                int ok = entry && strstr(served, want) != NULL; checked++; good += ok;
                const char *base = strrchr(served, '/');
                logf_("mods: verify %s -> served by %s (%s expected winner %s)", cf[i].path, base ? base + 1 : served, ok ? "OK, is the" : "MISMATCH, not the", g_posname[cf[i].winner]);
            }
            logf_("mods: verify: %d of %d checked conflicts are served by the expected winner", good, checked);
            RPT("\r\nSelf-check (engine lookup): %d of %d conflicting files served by the expected winner\r\n", good, checked);
        } else logf_("mods: verify skipped (FindFileInPakFiles bytes differ from this build)");
    }
    for (int i = 0; i < nsets; i++) free(lists[i]);
    plugin_register_builtin();
    g_mods_fp = (mounted_paks || rows_mods || g_plugins_n) ? fp : 0; g_mods_active = mounted_mods;
    lm_names_join(gpn, ngp, g_lm_me.names, sizeof g_lm_me.names); g_lm_me.fp = ngp ? gp_fp : 0; g_lm_me.present = 1;
    logf_("mods: LAN mod check fingerprint (gameplay mods only, %d of %d): %016llX %s", ngp, mounted_mods, (unsigned long long)g_lm_me.fp, g_lm_me.names);
    RPT("\r\nMod set fingerprint: %016llX\r\n", (unsigned long long)g_mods_fp);
    RPT("LAN mod check (gameplay mods only, %d): %016llX  %s\r\n", ngp, (unsigned long long)g_lm_me.fp, g_lm_me.names);
    {
        wchar_t rp[MAX_PATH]; swprintf(rp, MAX_PATH, L"%smods.txt", g_dir);
        HANDLE h = CreateFileW(rp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, report, (DWORD)rl, &w, NULL); CloseHandle(h); }
    }
    logf_("mods: fingerprint %016llX", (unsigned long long)g_mods_fp);
    plugins_load_all();                                                   // plugins last: everything they might look at is mounted
}
static uint8_t hk_pakinit(void *self, void *lower, const wchar_t *cmdline) {
    uint8_t r = g_orig_pakinit(self, lower, cmdline);                      // stock paks first, exactly as the game does it
    g_pak_self = self;
    if (g_safe_mode) { logf_("SAFE MODE: the mod loader is skipped (no Mods\\, no rows.json, no plugins)"); return r; }
    __try { mods_mount_all(self); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mods: EXCEPTION 0x%08lX in the mod loader - the game continues without mods", GetExceptionCode()); }
    return r;
}

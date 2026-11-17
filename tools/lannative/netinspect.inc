// ---------------------------------------------------------------------------------------------------------------------
// netinspect.inc - Net Inspector built into the loader: which bodies, animations and outfit pieces each character in the world uses, and which mod provides them.
// Included by lannative.c after hostmods.inc and before modsui.inc. Needs hostmods.inc (g_hj, g_hm_cs, hj_server_conn, hj_host_addr,
// hs_send_all, hs_recv_line, hj_mod_loaded_own, hm_menu_*), the api_p_* engine helpers (plugins.inc / secore.inc / testkit.inc / npcs.inc, called
// with self = NULL like room_palace), rowsjson.h (rowval_pakbase) and modglue.inc (g_pak_self, RVA_PAK_FIND).
// Pure logic: netinspect.h.
//
//   every PC : on demand (F2 Mods page rows, "refresh"), 8 s after a world loads, 6 s after joining a host and 4 s after the player count
//              changes, one UGameplayStatics::GetAllActorsOfClass(Character) in the current world (no per-tick scan, no object-table walk):
//              body mesh, running anim class, outfit pieces (FightingCharacter BPF_GetSkeletalMeshEquipment 0-9), net role, player name,
//              position; each asset path gets its provider here (FindFileInPakFiles -> the pak that serves it -> mod / base / pak; in memory
//              only -> made at run time). Rows on the F2 Mods page (after the host-mods rows) + LanNative\netinspect.txt.
//   host     : the host-mods side channel (TCP moddownload_port, same peer binding) answers "NI" with a fresh snapshot of its list
//              (netinspect.h wire format). The server thread asks the game thread for it and waits up to 5 s.
//   joiner   : fetches the host's list on a worker thread and diffs it (netinspect.h ni_report): MISSING / NOT APPLIED / DIFFERS /
//              OTHER COPY / NEVER ARRIVED per host character, the host mod that provides each gap, and the host-mods offer state of that mod
//              (OFFERED rows lead to its Install; not-shareable mods are named).
// ---------------------------------------------------------------------------------------------------------------------
#include "netinspect.h"

static CRITICAL_SECTION g_ni_cs; static int g_ni_init; static HANDLE g_ni_ev;
static NiChar g_ni_loc[NI_CHARS]; static int g_ni_nl;                  // this PC (game thread only)
static NiAsset g_ni_la[NI_ASSETS]; static int g_ni_nla;                 // their assets + providers here (game thread only)
static NiChar g_ni_host[NI_CHARS]; static int g_ni_nh = -1;             // the host's list (client); -1 = none. g_ni_cs
static NiAsset g_ni_ha[NI_ASSETS]; static int g_ni_nha;                 // g_ni_cs
static char g_ni_note[200] = "not asked yet";                          // g_ni_cs: why there is no host list
static char *g_ni_snap; static int g_ni_snap_len;                       // g_ni_cs: last snapshot text served to joiners
static volatile LONG g_ni_req, g_ni_fetch_done, g_ni_fetching, g_ni_asked;
static NiRow g_ni_rows[NI_ROWS]; static int g_ni_nrows, g_ni_show_all, g_ni_view = NIV_SOLO, g_ni_have_me; static float g_ni_me[3];
static char g_ni_hostaddr[64];
static uint64_t g_ni_due; static char g_ni_due_why[48];
static void *g_ni_local_pawn;

static void ni_init_once(void) {
    if (g_ni_init) return; g_ni_init = 1; InitializeCriticalSection(&g_ni_cs); g_ni_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hm_init_once();
}
static void ni_schedule(uint64_t at, const char *why) {
    if (g_ni_due && g_ni_due <= at) return;                              // an earlier refresh is pending already
    g_ni_due = at ? at : 1; snprintf(g_ni_due_why, sizeof g_ni_due_why, "%s", why);
}

// ----- engine reads (game thread)
static void *ni_objref(void *obj, const char *prop) { void **p = obj ? (void **)api_p_prop_ptr(NULL, obj, prop) : NULL; void *r = NULL; __try { r = p ? *p : NULL; } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; } return r; }
static void ni_text_path(void *obj, const char *prop, char *out, size_t cap) {
    char t[600]; out[0] = 0; if (!obj) return;
    if (api_p_get_text(NULL, obj, prop, t, sizeof t) >= 0) ni_objpath(t, out, cap);
}
// provider of an object path on this PC; 1 = present (prov filled), 0 = absent
static int ni_provider(const char *path, char *prov, size_t cap) {
    prov[0] = 0; if (!path || !path[0]) return 0;
    if (!strncmp(path, "/Script/", 8)) { snprintf(prov, cap, "exe"); return 1; }
    char pkg[NI_PATH], base[NI_PATH + 64]; ni_copy(pkg, sizeof pkg, path); { char *d = strchr(pkg, '.'); if (d) *d = 0; char *c = strchr(pkg, ':'); if (c) *c = 0; }
    if (g_pak_self && rowval_pakbase(pkg, base, sizeof base)) {
        for (int u = 0; u < 2; u++) {
            char fn[NI_PATH + 80]; snprintf(fn, sizeof fn, "%s%s", base, u ? ".umap" : ".uasset"); wchar_t wf[NI_PATH + 80]; widen(fn, wf, NI_PATH + 80);
            void *pak = NULL, *entry = NULL;
            __try { entry = ((PakFindRef_t)(g_base + RVA_PAK_FIND))(g_pak_self, wf, &pak); } __except (EXCEPTION_EXECUTE_HANDLER) { entry = NULL; pak = NULL; }
            if (!entry) continue;
            char served[MAX_PATH * 2] = "";
            __try { if (pak) { const wchar_t *f = *(const wchar_t **)pak; if (f && *(int *)((uint8_t *)pak + 8) > 0) w_to_u8(f, served, sizeof served); } } __except (EXCEPTION_EXECUTE_HANDLER) { served[0] = 0; }
            ni_prov_from_pak(served, prov, cap); return 1;
        }
    }
    if (api_p_find_object(NULL, path)) { char m[64]; snprintf(prov, cap, "runtime:%s", ni_mod_of_path(path, m, sizeof m) ? m : "-"); return 1; }
    return 0;
}
static int ni_here_cb(const char *path, char *prov, size_t cap, void *u) { (void)u; return ni_provider(path, prov, cap); }

static void ni_read_char(void *a, NiChar *c) {
    memset(c, 0, sizeof *c); char t[200];
    api_p_object_name(NULL, a, c->name, sizeof c->name);
    api_p_object_path(NULL, api_p_object_class(NULL, a), c->cls, sizeof c->cls);
    c->role = api_p_get_text(NULL, a, "Role", t, sizeof t) >= 0 ? ni_role_parse(t) : NI_ROLE_UNKNOWN;
    void *ps = ni_objref(a, "PlayerState"); if (ps && api_p_get_text(NULL, ps, "PlayerName", t, sizeof t) >= 0) { char *q = t; if (*q == '"') q++; size_t n = strlen(q); if (n && q[n - 1] == '"') q[n - 1] = 0; ni_clean(q, c->player, sizeof c->player); if (!c->player[0]) snprintf(c->player, sizeof c->player, "(player)"); }
    api_p_test_get_location(NULL, a, c->pos);
    void *mc = ni_objref(a, "Mesh");
    ni_text_path(mc, "SkeletalMesh", c->mesh, sizeof c->mesh);
    void *ai = ni_objref(mc, "AnimScriptInstance"); if (ai) api_p_object_path(NULL, api_p_object_class(NULL, ai), c->anim, sizeof c->anim);
    c->fighting = api_p_is_a(NULL, a, "FightingCharacter");
    if (c->fighting) {
        void *f = api_p_find_function(NULL, a, "BPF_GetSkeletalMeshEquipment");
        for (int s = 0; f && s < NI_SLOTS; s++) { uint8_t slot = (uint8_t)s; void *comp = NULL, *args[1] = { &slot }; if (api_p_call_params(NULL, a, f, args, &comp) && comp) ni_text_path(comp, "SkeletalMesh", c->piece[s], sizeof c->piece[s]); }
    }
}
// net view of this PC: joined client / listen host (MainMap is hosted even in solo play) / no net driver
static int ni_view_now(void) {
    if (hj_server_conn()) return NIV_CLIENT;
    void *nd = NULL; __try { void *w = world(); nd = w ? *(void **)((uint8_t *)w + OFF_W_NETDRIVER) : NULL; } __except (EXCEPTION_EXECUTE_HANDLER) { nd = NULL; }
    return nd ? NIV_HOST : NIV_SOLO;
}
static int ni_player_count(void) {
    int n = -1; void *gs = ni_objref(world(), "GameState"); if (!gs) return -1;
    __try { uint8_t *arr = api_p_prop_ptr(NULL, gs, "PlayerArray"); if (arr) n = *(int *)(arr + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
    return n;
}
// GetAllActorsOfClass(world, Character) -> g_ni_loc
static int ni_scan_local(void) {
    void *w = world(); g_ni_nl = 0; g_ni_nla = 0; g_ni_have_me = 0; g_ni_local_pawn = NULL;
    void *lib = api_p_find_cdo(NULL, "GameplayStatics"), *fn = lib ? api_p_find_function(NULL, lib, "GetAllActorsOfClass") : NULL, *cls = api_p_find_class(NULL, "Character");
    if (!w || !lib || !fn || !cls) { logf_("net inspector: scan skipped (world %p, GameplayStatics %p, GetAllActorsOfClass %p, Character class %p)", w, lib, fn, cls); return -1; }
    struct { void **d; int n, m; } arr = { NULL, 0, 0 }; void *args[3] = { &w, &cls, &arr };
    if (!api_p_call_params(NULL, lib, fn, args, NULL)) { logf_("net inspector: GetAllActorsOfClass failed"); return -1; }
    // ponytail: the engine-allocated OutActors buffer (8 bytes per actor) is not freed - a few hundred bytes per refresh; FMemory::Free when refreshes get frequent
    void *pc = api_p_local_pc(NULL); g_ni_local_pawn = ni_objref(pc, "Pawn");
    int total = arr.n;
    for (int i = 0; i < arr.n && g_ni_nl < NI_CHARS; i++) {
        void *a = NULL; __try { a = arr.d[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { a = NULL; } if (!a) continue;
        NiChar *c = &g_ni_loc[g_ni_nl];
        __try { ni_read_char(a, c); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("net inspector: EXCEPTION 0x%08lX reading character %d", GetExceptionCode(), i); continue; }
        if (a == g_ni_local_pawn) { g_ni_have_me = 1; memcpy(g_ni_me, c->pos, sizeof g_ni_me); }
        g_ni_nl++;
    }
    if (total > NI_CHARS) logf_("net inspector: %d characters in the world, the first %d are inspected", total, NI_CHARS);
    for (int i = 0; i < g_ni_nl; i++) {                                             // asset table with providers on this PC
        const NiChar *c = &g_ni_loc[i]; const char *p[NI_SLOTS + 3]; int np = 0; p[np++] = c->cls; p[np++] = c->mesh; p[np++] = c->anim; for (int s = 0; s < NI_SLOTS; s++) p[np++] = c->piece[s];
        for (int k = 0; k < np; k++) { int ai = ni_asset_add(g_ni_la, &g_ni_nla, p[k]); if (ai >= 0 && !g_ni_la[ai].prov[0]) ni_provider(g_ni_la[ai].path, g_ni_la[ai].prov, NI_PROV); }
    }
    return g_ni_nl;
}
// the text a joiner gets (g_ni_snap)
static void ni_make_snapshot(void) {
    size_t cap = 512 + (size_t)g_ni_nl * NI_WIRE + (size_t)g_ni_nla * (NI_PATH + NI_PROV + 8); char *s = malloc(cap); if (!s) return;
    size_t w = 0; char line[NI_WIRE];
    ni_format_header(g_ni_nl, g_ni_nla, g_ni_view == NIV_HOST ? "listen host" : g_ni_view == NIV_CLIENT ? "client" : "solo", line, sizeof line); w += (size_t)snprintf(s + w, cap - w, "%s", line);
    for (int i = 0; i < g_ni_nl; i++) if (ni_format_char(&g_ni_loc[i], line, sizeof line)) w += (size_t)snprintf(s + w, cap - w, "%s", line);
    for (int i = 0; i < g_ni_nla; i++) if (ni_format_asset(&g_ni_la[i], line, sizeof line)) w += (size_t)snprintf(s + w, cap - w, "%s", line);
    EnterCriticalSection(&g_ni_cs); free(g_ni_snap); g_ni_snap = s; g_ni_snap_len = (int)w; LeaveCriticalSection(&g_ni_cs);
}

// ----- the host-mods offer as the page sees it
static int ni_offer_cb(const char *mod, void *u) {
    (void)u; int st = NIO_UNKNOWN;
    EnterCriticalSection(&g_hm_cs);
    if (g_hj.state == HJ_LISTING) st = NIO_LISTING;
    for (int i = 0; i < g_hj.n && st == NIO_UNKNOWN; i++) if (!_stricmp(g_hj.e[i].mod, mod)) {
        if (g_hj.state == HJ_DONE || (g_hj.state == HJ_DOWNLOADING && g_hj.have_dl[i])) st = NIO_DOWNLOADED;
        else if (g_hj.state == HJ_DOWNLOADING && g_hj.want[i]) st = NIO_DOWNLOADING;
        else if (g_hj.want[i] && g_hj.state == HJ_OFFER) st = NIO_OFFERED;
        else if (g_hj.want[i] && g_hj.have_dl[i]) st = NIO_DOWNLOADED;
        else if (!g_hj.want[i]) st = NIO_OWN;                                     // you load the same file already (own or _FromHost)
    }
    for (int i = 0; i < g_hj.nns && st == NIO_UNKNOWN; i++) if (!_stricmp(g_hj.ns[i].mod, mod)) st = NIO_NOSHARE;
    LeaveCriticalSection(&g_hm_cs);
    if (st == NIO_UNKNOWN && hj_mod_loaded_own(mod)) st = NIO_OWN;
    return st;
}

// ----- page + report (game thread)
static void ni_render(const char *why) {
    static NiChar hostc[NI_CHARS]; static NiAsset ha[NI_ASSETS]; int nh, nha; char note[200];
    EnterCriticalSection(&g_ni_cs); nh = g_ni_nh; nha = g_ni_nha; if (nh > 0) memcpy(hostc, g_ni_host, sizeof(NiChar) * (size_t)nh); if (nha > 0) memcpy(ha, g_ni_ha, sizeof(NiAsset) * (size_t)nha); snprintf(note, sizeof note, "%s", g_ni_note); LeaveCriticalSection(&g_ni_cs);
    NiView v; memset(&v, 0, sizeof v); v.view = g_ni_view; v.host = g_ni_hostaddr; v.loc = g_ni_loc; v.nl = g_ni_nl;
    if (g_ni_view == NIV_CLIENT) { v.hostc = hostc; v.nh = nh; v.ha = ha; v.nha = nha; } else { v.nh = 0; v.ha = g_ni_la; v.nha = g_ni_nla; }
    v.host_note = note; v.have_me = g_ni_have_me; memcpy(v.me, g_ni_me, sizeof v.me); v.show_all = g_ni_show_all; v.asked = (int)g_ni_asked;
    v.here = ni_here_cb; v.offer = ni_offer_cb;
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%snetinspect.txt", g_dir); FILE *f = _wfopen(p, L"wb");
    if (f) { SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d, refresh: %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, why); }
    int n = 0; __try { n = ni_report(&v, g_ni_rows, NI_ROWS, f); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("net inspector: EXCEPTION 0x%08lX building the page", GetExceptionCode()); n = 0; }
    if (f) fclose(f); else logf_("net inspector: cannot write %ls", p);
    if (!n) { memset(g_ni_rows, 0, sizeof g_ni_rows[0]); snprintf(g_ni_rows[0].text, NI_LINE, "NET INSPECTOR: error building the page (see lannative.log)  (click: refresh)"); g_ni_rows[0].act = NIA_REFRESH; n = 1; }
    g_ni_nrows = n;
    for (int i = 0; i < n; i++) if (g_ni_rows[i].act != NIA_ALL || i == 0) logf_("net inspector: | %s", g_ni_rows[i].text);
}
static DWORD WINAPI ni_fetch_thread(LPVOID u);
static void ni_refresh(const char *why) {
    ni_init_once(); uint64_t t0 = GetTickCount64();
    g_ni_view = ni_view_now();
    int n = -1; __try { n = ni_scan_local(); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("net inspector: EXCEPTION 0x%08lX in the scan", GetExceptionCode()); n = -1; }
    if (g_ni_view != NIV_CLIENT) { ni_make_snapshot(); g_ni_hostaddr[0] = 0; }
    logf_("net inspector: refresh (%s): %s, %d character(s), %d asset(s), %llu ms", why, g_ni_view == NIV_CLIENT ? "joined client" : g_ni_view == NIV_HOST ? "listen host" : "solo", n, g_ni_nla, (unsigned long long)(GetTickCount64() - t0));
    if (g_ni_view == NIV_CLIENT && g_hm_mode != HM_OFF && !InterlockedCompareExchange(&g_ni_fetching, 1, 0)) {
        EnterCriticalSection(&g_ni_cs); snprintf(g_ni_note, sizeof g_ni_note, "asking the host ..."); LeaveCriticalSection(&g_ni_cs);
        HANDLE th = CreateThread(NULL, 0, ni_fetch_thread, NULL, 0, NULL); if (th) CloseHandle(th); else { InterlockedExchange(&g_ni_fetching, 0); logf_("net inspector: CreateThread failed (%lu)", GetLastError()); }
    } else if (g_ni_view == NIV_CLIENT && g_hm_mode == HM_OFF) { EnterCriticalSection(&g_ni_cs); snprintf(g_ni_note, sizeof g_ni_note, "moddownload = off on this PC (the host is not asked)"); g_ni_nh = -1; LeaveCriticalSection(&g_ni_cs); }
    ni_render(why);
}

// ----- joiner: the host's list (worker thread)
static DWORD WINAPI ni_fetch_thread(LPVOID u) {
    (void)u; static NiChar hc[NI_CHARS]; static NiAsset ha[NI_ASSETS]; int nh = -1, na = 0; char note[200] = "", addr[64] = "", role[32] = ""; uint32_t ip = 0;
    static char line[NI_WIRE + 8];
    if (!hj_host_addr(addr, sizeof addr, &ip)) snprintf(note, sizeof note, "ip.txt holds no usable host address");
    else {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) { DWORD to = 10000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
            struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)g_hm_port); a.sin_addr.s_addr = htonl(ip);
            if (connect(s, (struct sockaddr *)&a, sizeof a)) { int e = WSAGetLastError(); closesocket(s); s = INVALID_SOCKET; WSASetLastError(e); } }
        if (s == INVALID_SOCKET) snprintf(note, sizeof note, "cannot reach %s TCP %u (error %d): the host runs no loader gameplay mods, has moddownload = off, or a firewall", addr, g_hm_port, WSAGetLastError());
        else {
            if (!hs_send_all(s, "NI\n", 3) || !hs_recv_line(s, line, sizeof line)) snprintf(note, sizeof note, "no answer from %s", addr);
            else if (!strncmp(line, "ERR", 3)) snprintf(note, sizeof note, "%s refused: %.80s%s", addr, line, strstr(line, "bad request") ? " (its loader has no Net Inspector)" : "");
            else if ((nh = ni_parse_header(line, &na, role, sizeof role)) < 0) snprintf(note, sizeof note, "%s answered '%.60s'", addr, line);
            else {
                char why[160] = "";
                for (int i = 0; i < nh + na; i++) {
                    if (!hs_recv_line(s, line, sizeof line)) { snprintf(note, sizeof note, "the host's list ended early (line %d of %d)", i, nh + na); nh = -1; break; }
                    if (i < nh ? !ni_parse_char(line, &hc[i], why, sizeof why) : !ni_parse_asset(line, &ha[i - nh], why, sizeof why)) { snprintf(note, sizeof note, "bad line %d from the host: %s", i, why); nh = -1; break; }
                }
            }
            closesocket(s);
        }
    }
    EnterCriticalSection(&g_ni_cs);
    snprintf(g_ni_hostaddr, sizeof g_ni_hostaddr, "%s", addr[0] ? addr : "?");
    if (nh >= 0) { memcpy(g_ni_host, hc, sizeof(NiChar) * (size_t)nh); memcpy(g_ni_ha, ha, sizeof(NiAsset) * (size_t)na); g_ni_nh = nh; g_ni_nha = na; snprintf(g_ni_note, sizeof g_ni_note, "%s (%s): %d character(s), %d asset(s)", addr, role, nh, na); }
    else { g_ni_nh = -1; g_ni_nha = 0; snprintf(g_ni_note, sizeof g_ni_note, "%s", note); }
    LeaveCriticalSection(&g_ni_cs);
    logf_("net inspector: host list from %s: %s", addr[0] ? addr : "?", nh >= 0 ? "received" : note);
    if (nh >= 0) logf_("net inspector: host is %s, %d character(s), %d asset(s)", role, nh, na);
    InterlockedExchange(&g_ni_fetching, 0); InterlockedExchange(&g_ni_fetch_done, 1);
    return 0;
}

// ----- host: hostmods.inc hs_serve hands "NI" requests here (server thread)
static void ni_serve(SOCKET c, const char *masked) {
    ni_init_once();
    InterlockedIncrement(&g_ni_asked);                                               // before the refresh, so the host's own page counts this request
    InterlockedExchange(&g_ni_req, 1);
    DWORD wr = WaitForSingleObject(g_ni_ev, 5000);
    EnterCriticalSection(&g_ni_cs); char *s = g_ni_snap ? _strdup(g_ni_snap) : NULL; int n = g_ni_snap_len; LeaveCriticalSection(&g_ni_cs);
    int ok = s ? hs_send_all(c, s, n) : hs_send_all(c, "LNNI1 0 0 no_snapshot\n", 22);
    logf_("net inspector: NI from %s -> %d bytes (%s)%s", masked, s ? n : 22, wr == WAIT_OBJECT_0 ? "fresh snapshot" : "game thread busy: previous snapshot", ok ? "" : " - send failed");
    free(s);
}

// ----- the F2 Mods page (modsui.inc appends these rows after the host-mods rows)
static int ni_menu_count(void) {
    ni_init_once();
    static uint64_t last; uint64_t now = GetTickCount64();                        // modsui re-reads the rows every 0.5 s while the page is open
    if ((!g_ni_nrows || now - last > 2000) && world()) ni_refresh("Mods page opened");   // on demand: opening the page scans
    last = now;
    return g_ni_nrows;
}
static const char *ni_menu_label(int i) { return i >= 0 && i < g_ni_nrows ? g_ni_rows[i].text : ""; }
static void ni_install_link(const char *mod) {
    int n = hm_menu_count();
    for (int i = 0; i < n; i++) if (g_hm_act[i] == HMA_INSTALL) { logf_("net inspector: '%s' row -> the host-mods INSTALL row (%s)", mod, g_hm_lbl[i]); hm_menu_action(i); return; }
    logf_("net inspector: '%s' row clicked, but the host-mods page has no INSTALL row right now (state %d)", mod, g_hj.state);
    char lines[1][LM_LINE_MAX]; snprintf(lines[0], LM_LINE_MAX, "Net Inspector: no download offer open for %s right now", mod); toast_show(lines, 1, 6);
}
static void ni_menu_action(int i) {
    if (i < 0 || i >= g_ni_nrows) return;
    NiRow r = g_ni_rows[i];
    switch (r.act) {
    case NIA_REFRESH: ni_refresh("refresh clicked"); break;
    case NIA_ALL: g_ni_show_all = !g_ni_show_all; logf_("net inspector: every character %s", g_ni_show_all ? "shown" : "hidden"); ni_render("show / hide clicked"); break;
    case NIA_INSTALL: ni_install_link(r.arg); ni_render("install clicked"); break;
    default: break;
    }
}


// ----- game thread, every frame (lannative.c on_tick); the work below runs at most 4 times a second, the scan only when due
static void ni_tick(void) {
    if (!g_hm_init) return;                                                         // hostmods_install has not run (minimal build / too early)
    ni_init_once();
    if (InterlockedExchange(&g_ni_req, 0)) {                                         // a joiner asked: fresh snapshot, then wake the server thread
        __try { ni_refresh("a joiner asked"); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("net inspector: EXCEPTION 0x%08lX in the refresh", GetExceptionCode()); }
        SetEvent(g_ni_ev);
    }
    static uint64_t next, next_pc; static void *last_w, *last_sc; static int last_players = -1, last_seq = -1; uint64_t now = GetTickCount64();
    if (now < next) return; next = now + 250;
    __try {
        void *w = world(); if (w != last_w) {                                        // a NEW world replaces a refresh still pending for the previous one (NIA-1: the
            last_w = w; last_players = -1; g_ni_nrows = 0;                          // title world's +8 s refresh ran the moment MainMap finished loading, before the
            if (w) { g_ni_due = now + 10000; snprintf(g_ni_due_why, sizeof g_ni_due_why, "world loaded"); } else g_ni_due = 0;   // player's outfit was applied: a false EMPTY OUTFIT)
        }
        void *sc = hj_server_conn(); if (sc != last_sc) { last_sc = sc; if (sc) ni_schedule(now + 6000, "joined a host"); else { EnterCriticalSection(&g_ni_cs); g_ni_nh = -1; snprintf(g_ni_note, sizeof g_ni_note, "not joined"); LeaveCriticalSection(&g_ni_cs); } }
        if (w && now >= next_pc) { next_pc = now + 5000; int pcnt = ni_player_count(); if (pcnt >= 0 && last_players >= 0 && pcnt != last_players) { logf_("net inspector: players %d -> %d", last_players, pcnt); ni_schedule(now + 4000, "player count changed"); } if (pcnt >= 0) last_players = pcnt; }
        if (g_ni_due && now >= g_ni_due) { char why[48]; snprintf(why, sizeof why, "%s", g_ni_due_why); g_ni_due = 0; if (world()) ni_refresh(why); }
        int seq; EnterCriticalSection(&g_hm_cs); seq = g_hj.seq; LeaveCriticalSection(&g_hm_cs);
        if (InterlockedExchange(&g_ni_fetch_done, 0)) { last_seq = seq; ni_render("host list arrived"); }
        else if (seq != last_seq) { last_seq = seq; if (g_ni_nrows && g_ni_view == NIV_CLIENT) ni_render("host-mods offer changed"); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("net inspector: EXCEPTION 0x%08lX in the tick", GetExceptionCode()); }
}

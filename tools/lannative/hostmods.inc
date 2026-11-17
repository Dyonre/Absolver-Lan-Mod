// ---------------------------------------------------------------------------------------------------------------------
// hostmods.inc - host mod download (Counter-Strike style "this server has mods, install them?") and the host player cap.
// Included by lannative.c after content.inc and before modsui.inc. Needs lanmods.inc (toast_show, lm_refuse, widen), joinguard.h (g_jg),
// modglue.inc (mods_read_text, PakMount_t, g_mi*), modes.inc (md_console).
// Pure logic: hostmods.h / maxplayers.h.
//
//   host   : PreLogin (hm_on_prelogin) checks the player cap, records the joiner's address and opens a TCP listener (one thread, one request
//            per connection) that serves the .pak files of its loaded pak-only gameplay mods whose mod.json says "shareable": true (the
//            author's opt-in; other gameplay mods are only named): LIST / GET <index>. Only addresses that passed its PreLogin are answered,
//            the allow list (config "allow") applies, paths come only from its own table.
//   joiner : once connected to a host as a client ("moddownload = prompt"), asks the host it joined (ip.txt) for the list; if the host's
//            gameplay-mod fingerprint differs from its own, keeps what it does not load already and shows the offer (toast + F2 Mods page).
//            Nothing is fetched or written before Install is clicked.
//            Install: download each file to <pak>.part, check size + SHA-256 + "is a UE pak" + allowed packed types + Config/*.ini flag, then rename
//            to Mods\_FromHost\<host>\<Mod>\<pak> and mark that host active (Mods\_FromHost\active.txt). Those mods mount at the next start
//            (hm_mount_fromhost, after the player's own mods); the player restarts and joins again.
//   cap    : "maxplayers" (maxplayers.h) checked in PreLogin.
// Config (LanNative\config.txt): moddownload = prompt | off (default prompt; off = neither offers nor asks), moddownload_port = 7779, maxplayers = 6 | 2..16 | off.
// ---------------------------------------------------------------------------------------------------------------------
#include "hostmods.h"
#include "maxplayers.h"
#pragma comment(lib, "ws2_32.lib")
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))   // winsock2.h value; windows.h brings only winsock.h
#endif

static int g_mp_cap = MP_DEFAULT;
static int g_hm_mode = HM_PROMPT;
static unsigned g_hm_port = HM_PORT;

static void hm_cfg_write(const char *key, const char *value) {         // config.txt only (a legacy one-file-per-setting install keeps the value for this session)
    if (!g_cfg_present) { logf_("host mods: config.txt does not exist - %s = %s applies to this session only (add the line to LanNative\\config.txt to keep it)", key, value); return; }
    static char out[sizeof g_cfg_text]; int n = mset_cfg_set(g_cfg_text, key, value, out, sizeof out);
    if (n < 0) { logf_("host mods: config.txt is too large to add '%s' - not saved", key); return; }
    wchar_t path[MAX_PATH], tmp[MAX_PATH]; swprintf(path, MAX_PATH, L"%sconfig.txt", g_dir); swprintf(tmp, MAX_PATH, L"%sconfig.txt.tmp", g_dir);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD w = 0; int ok = 0;
    if (h != INVALID_HANDLE_VALUE) { ok = WriteFile(h, out, (DWORD)n, &w, NULL) && w == (DWORD)n; CloseHandle(h); }
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); else DeleteFileW(tmp);
    if (ok) { memcpy(g_cfg_text, out, (size_t)n + 1); logf_("host mods: config.txt: %s = %s (saved)", key, value); }
    else logf_("host mods: config.txt could not be written (error %lu) - %s = %s applies to this session only", GetLastError(), key, value);
}

static void hs_on_login(uint32_t ip);
// ----- PreLogin: player cap, then remember the joiner's address for the download side channel. 1 = refused.
static int hm_on_prelogin(const char *which, void *address, void *err) {
    int peers = 0;
    __try { peers = T_peers_now(); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("max players (%s): EXCEPTION 0x%08lX reading the connection count - login allowed", which, GetExceptionCode()); peers = -1; }
    if (peers >= 0 && mp_refuse(g_mp_cap, peers)) {
        logf_("max players (%s): login REFUSED - %d players would exceed maxplayers = %d", which, 1 + peers, g_mp_cap);
        char t[120]; snprintf(t, sizeof t, "Join refused: the host's world is full (%d players). Try again later.", g_mp_cap); lm_refuse(err, t);
        char lines[1][LM_LINE_MAX]; snprintf(lines[0], LM_LINE_MAX, "JOIN REFUSED: world full (maxplayers = %d)", g_mp_cap); toast_show(lines, 1, 8);
        return 1;
    }
    if (g_mp_cap && peers >= 0) logf_("max players (%s): %d of %d after this login - allowed", which, 1 + peers, g_mp_cap);
    const FStringView *a = address; char addr[80] = ""; uint32_t ip;
    if (a && a->data && a->num > 1 && a->num < 200) narrow(a->data, a->num - 1, addr, sizeof addr);
    if (jg_parse_addr(addr, &ip)) hs_on_login(ip);
    return 0;
}
// ----- shared state (g_hm_cs guards everything below that the worker threads touch)
static CRITICAL_SECTION g_hm_cs; static int g_hm_init, g_hm_wsa;
static HmPeers g_hs_peers;                                // host: addresses that passed PreLogin (guarded by g_hm_cs)
enum { HJ_IDLE = 0, HJ_LISTING, HJ_OFFER, HJ_NOTHING, HJ_DOWNLOADING, HJ_DONE, HJ_FAILED, HJ_SKIPPED };
static struct {
    int state, seq, shown_seq;                          // seq changes with every state change; the game thread toasts once per change
    char addr[64], key[HM_NAME_MAX]; unsigned port; uint32_t ip; uint64_t fp;
    HmEntry e[HM_MAX]; int n, want[HM_MAX], have_dl[HM_MAX]; uint64_t total, got; int files_done, files_all;
    HmNoShare ns[HM_MAX]; int nns;                      // host mods this PC lacks that the host may not share
    char msg[160];
} g_hj;
static char g_hm_active[HM_NAME_MAX];                    // Mods\_FromHost\active.txt at boot ("" = none)
static char g_hfm[MODS_MAX][HM_NAME_MAX]; static int g_hfm_n;   // mods mounted from _FromHost at boot

static void hm_set(int state, const char *fmt, ...) {   // caller holds g_hm_cs
    g_hj.state = state; g_hj.seq++;
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(g_hj.msg, sizeof g_hj.msg, fmt, ap); va_end(ap); } else g_hj.msg[0] = 0;
    logf_("host mods: state %d: %s", state, g_hj.msg);
}
static void hm_init_once(void) {
    if (g_hm_init) return; g_hm_init = 1; InitializeCriticalSection(&g_hm_cs);
    WSADATA wd; g_hm_wsa = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    if (!g_hm_wsa) logf_("host mods: WSAStartup failed - no downloads this run");
}
static void hm_fromhost_dir(const char *key, const char *mod, wchar_t *out) {   // Mods\_FromHost[\key[\mod]]
    wchar_t k[HM_NAME_MAX], m[HM_NAME_MAX]; u8_to_w(key ? key : "", k, HM_NAME_MAX); u8_to_w(mod ? mod : "", m, HM_NAME_MAX);
    if (key && mod) swprintf(out, MAX_PATH, L"%ls\\_FromHost\\%ls\\%ls", g_mods_dir, k, m);
    else if (key) swprintf(out, MAX_PATH, L"%ls\\_FromHost\\%ls", g_mods_dir, k);
    else swprintf(out, MAX_PATH, L"%ls\\_FromHost", g_mods_dir);
}
// a pak on disk passes: not an executable, UE pak footer + readable index, every packed file a data type. *ini = has Config/*.ini; ih = index hash.
static int hm_check_pak_file(const wchar_t *path, int *ini, unsigned char ih[20], char *why, size_t wcap) {
    FILE *f = _wfopen(path, L"rb"); if (!f) { snprintf(why, wcap, "cannot be read"); return 0; }
    int ok = hm_check_pak_stream(f, ini, ih, why, wcap); fclose(f); return ok;      // hostmods.h (tested offline)
}

// ===== host side
typedef struct { HmEntry e; wchar_t path[MAX_PATH]; } HsFile;
static HsFile g_hs[HM_MAX]; static int g_hs_n = -1, g_hs_pending_n; static SOCKET g_hs_sock = INVALID_SOCKET;
static HmNoShare g_hsns[HM_MAX]; static int g_hsns_n;      // gameplay mods this host runs but does not share (named in the LIST answer)
static void hs_noshare(const ModInfo *mi, const char *label, const char *why) {
    logf_("host mods: %s is not offered: %s", mi->name, why);
    if (g_hsns_n < HM_MAX && hm_name_ok(mi->name)) { snprintf(g_hsns[g_hsns_n].mod, HM_NAME_MAX, "%s", mi->name); hm_clean_label(label, g_hsns[g_hsns_n].label, HM_LABEL_MAX); g_hsns_n++; }
}
// game thread: which files may be offered (no hashing here). Pak-only gameplay mods that loaded and whose mod.json says "shareable": true, in load order.
static void hs_collect(void) {
    int order[MODS_MAX], no = 0;
    for (int m = 0; m < g_mi_n && no < MODS_MAX; m++) if (g_mi_pos[m] >= 0) order[no++] = m;
    for (int a = 1; a < no; a++) for (int b = a; b > 0 && g_mi_pos[order[b - 1]] > g_mi_pos[order[b]]; b--) { int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    g_hs_pending_n = 0; g_hsns_n = 0;
    for (int oi = 0; oi < no; oi++) {
        const ModInfo *mi = &g_mi[order[oi]];
        if (!mi->affects_gameplay || mi->joiners_optional) continue;              // cosmetic or "joiners_need": false: not part of the mod check, not offered
        char lab[HM_LABEL_MAX] = ""; if (mi->version[0] && strcmp(mi->version, "0")) snprintf(lab, sizeof lab, "%s%sv%s", strcmp(mi->id, mi->name) ? mi->id : "", strcmp(mi->id, mi->name) ? " " : "", mi->version);
        if (!hm_name_ok(mi->name)) { logf_("host mods: %s is not offered: folder name has characters other than A-Z a-z 0-9 . _ -", mi->name); continue; }
        if (mi->has_rows || mi->has_content || mi->has_plugin || mi->has_lua) { hs_noshare(mi, lab, "it is not pak-only (rows.json / content / plugin / Lua never travel)"); continue; }
        { wchar_t mj[MAX_PATH]; swprintf(mj, MAX_PATH, L"%ls\\mod.json", g_mi_dir[order[oi]]); char *j = mods_read_text(mj, 1 << 20); int sh = hm_manifest_shareable(j); free(j);
          if (!sh) { hs_noshare(mi, lab, "its mod.json does not say \"shareable\": true (the author's opt-in)"); continue; } }
        wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*.pak", g_mi_dir[order[oi]]); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        char names[16][HM_NAME_MAX]; int nn = 0;
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nn < 16) { char u[MAX_PATH]; w_to_u8(fd.cFileName, u, sizeof u); if (hm_pak_name_ok(u)) snprintf(names[nn++], HM_NAME_MAX, "%s", u); else logf_("host mods: %s/%s is not offered: file name not allowed", mi->name, u); } } while (FindNextFileW(h, &fd)); FindClose(h); }
        qsort(names, (size_t)nn, sizeof names[0], (int (*)(const void *, const void *))_stricmp);
        for (int k = 0; k < nn && g_hs_pending_n < HM_MAX; k++) {
            HsFile *s = &g_hs[g_hs_pending_n++]; memset(s, 0, sizeof *s);
            snprintf(s->e.mod, sizeof s->e.mod, "%s", mi->name); snprintf(s->e.pak, sizeof s->e.pak, "%s", names[k]);
            hm_clean_label(lab, s->e.label, sizeof s->e.label);
            wchar_t wn[HM_NAME_MAX]; u8_to_w(names[k], wn, HM_NAME_MAX); swprintf(s->path, MAX_PATH, L"%ls\\%ls", g_mi_dir[order[oi]], wn);
        }
    }
}
// server thread, first request: hash + check each candidate (can take seconds for big paks; never on the game thread)
static void hs_build(void) {
    int w = 0;
    for (int i = 0; i < g_hs_pending_n; i++) {
        HsFile *s = &g_hs[i]; char why[160] = ""; unsigned char ih[20];
        if (!hm_check_pak_file(s->path, &s->e.ini, ih, why, sizeof why)) { logf_("host mods: %s/%s is not offered: %s", s->e.mod, s->e.pak, why); continue; }
        if (!hm_sha_file(s->path, s->e.sha, &s->e.size)) { logf_("host mods: %s/%s is not offered: cannot hash it", s->e.mod, s->e.pak); continue; }
        if (s->e.size > HM_FILE_CAP) { logf_("host mods: %s/%s is not offered: larger than the per-file limit", s->e.mod, s->e.pak); continue; }
        logf_("host mods: offering %s/%s %llu bytes sha256 %.16s...%s", s->e.mod, s->e.pak, (unsigned long long)s->e.size, s->e.sha, s->e.ini ? " (has Config/*.ini)" : "");
        if (w != i) g_hs[w] = *s; w++;
    }
    g_hs_n = w; logf_("host mods: %d file(s) can be downloaded from this host", w);
}
static int hs_send_all(SOCKET c, const char *p, int n) { while (n > 0) { int k = send(c, p, n, 0); if (k <= 0) return 0; p += k; n -= k; } return 1; }
static int hs_recv_line(SOCKET c, char *out, int cap) {
    int n = 0; for (;;) { char ch; int k = recv(c, &ch, 1, 0); if (k <= 0) return 0; if (ch == '\n') break; if (ch == '\r') continue; if (n >= cap - 1) return 0; out[n++] = ch; }
    out[n] = 0; return 1;
}
static void hs_serve(SOCKET c, uint32_t ip) {
    char masked[40]; { char a[24]; snprintf(a, sizeof a, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); jg_mask_addr(a, masked, sizeof masked); }
    if (!jg_allowed_ip(&g_jg, ip)) { logf_("host mods: request from %s REFUSED (allow list)", masked); return; }
    EnterCriticalSection(&g_hm_cs); int known = hm_peer_ok(&g_hs_peers, ip, GetTickCount64()); LeaveCriticalSection(&g_hm_cs);
    if (!known) { logf_("host mods: request from %s REFUSED: that address has not joined this game", masked); hs_send_all(c, "ERR join the game first\n", 24); return; }
    DWORD to = 20000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    char line[200], cmd[8] = "", extra[8] = ""; int idx = -1;
    if (!hs_recv_line(c, line, sizeof line)) { logf_("host mods: request from %s: no request line", masked); return; }
    int nf = sscanf(line, "%7s %d %7s", cmd, &idx, extra);
    if (g_hm_mode == HM_OFF) { hs_send_all(c, "ERR this host does not share mods\n", 34); return; }
    if (g_hs_n < 0) hs_build();
    char buf[400];
    if (nf == 1) LN_FOREACH(hsverb, LnVerb, v) if (!strcmp(cmd, v->verb)) {          // the units' verbs (lnhooks.h LN_HS_VERB): NI
        if (!strcmp(v->verb, "LIST") || !strcmp(v->verb, "GET")) {                // built-in verbs: a unit may not take them over (the built-in one answers below)
            static volatile LONG said; if (!InterlockedExchange(&said, 1)) logf_("host mods: unit '%s' registered the built-in verb %s - REFUSED, the built-in %s answers (rename the unit's verb)", v->name, v->verb, v->verb);
            continue;
        }
        v->fn(c, masked); return;
    }
    if (!strcmp(cmd, "LIST") && nf == 1) {
        hm_format_header(g_lm_me.fp, g_hs_n, g_hsns_n, buf, sizeof buf); int ok = hs_send_all(c, buf, (int)strlen(buf));
        for (int i = 0; ok && i < g_hs_n; i++) { hm_format_line(&g_hs[i].e, buf, sizeof buf); ok = hs_send_all(c, buf, (int)strlen(buf)); }
        for (int i = 0; ok && i < g_hsns_n; i++) { hm_format_noshare(&g_hsns[i], buf, sizeof buf); ok = hs_send_all(c, buf, (int)strlen(buf)); }
        logf_("host mods: LIST from %s -> %d file(s), %d mod(s) not shareable%s", masked, g_hs_n, g_hsns_n, ok ? "" : " (send failed)"); return;
    }
    if (strcmp(cmd, "GET") || nf != 2 || idx < 0 || idx >= g_hs_n) { hs_send_all(c, "ERR bad request\n", 16); logf_("host mods: request from %s: '%s' refused", masked, cmd); return; }
    HsFile *s = &g_hs[idx];
    HANDLE f = CreateFileW(s->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { hs_send_all(c, "ERR cannot read the file\n", 25); logf_("host mods: GET %s/%s: cannot open (error %lu)", s->e.mod, s->e.pak, GetLastError()); return; }
    int n = snprintf(buf, sizeof buf, "OK %llu\n", (unsigned long long)s->e.size); uint64_t sent = 0; int ok = hs_send_all(c, buf, n);
    static char chunk[1 << 16]; DWORD got;
    while (ok && sent < s->e.size && ReadFile(f, chunk, sizeof chunk, &got, NULL) && got) { if (sent + got > s->e.size) got = (DWORD)(s->e.size - sent); ok = hs_send_all(c, chunk, (int)got); sent += got; }
    CloseHandle(f);
    logf_("host mods: GET %s/%s to %s: %llu of %llu bytes%s", s->e.mod, s->e.pak, masked, (unsigned long long)sent, (unsigned long long)s->e.size, ok && sent == s->e.size ? "" : " - INCOMPLETE");
}
static DWORD WINAPI hs_thread(LPVOID u) {
    (void)u; logf_("host mods: server thread %lu listening on TCP %u", GetCurrentThreadId(), g_hm_port);
    for (;;) {
        struct sockaddr_in a; int al = sizeof a; SOCKET c = accept(g_hs_sock, (struct sockaddr *)&a, &al);
        if (c == INVALID_SOCKET) { logf_("host mods: accept failed (%d) - server stopped", WSAGetLastError()); return 0; }
        __try { hs_serve(c, ntohl(a.sin_addr.s_addr)); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("host mods: EXCEPTION 0x%08lX while serving a request", GetExceptionCode()); }
        closesocket(c);                                            // Limit: one request at a time; a second thread pool when hosts serve many joiners at once
    }
}
// 1 = the listener runs (started now or before)
static int hs_start(void) {
    if (g_hs_sock != INVALID_SOCKET) return 1;
    static int failed; if (failed || !g_hm_wsa) return 0;
    hs_collect();
    if (!g_hs_pending_n && !g_hsns_n) { logf_("host mods: no loaded gameplay mods to share or name - no listener"); failed = 1; return 0; }
    logf_("host mods: %d file(s) of shareable mods, %d gameplay mod(s) not shareable", g_hs_pending_n, g_hsns_n);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)g_hm_port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    BOOL excl = TRUE; if (s != INVALID_SOCKET) setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) {
        logf_("host mods: cannot listen on TCP %u (error %d; another game on this PC hosting?) - joiners cannot download", g_hm_port, WSAGetLastError());
        if (s != INVALID_SOCKET) closesocket(s); failed = 1; return 0;
    }
    g_hs_sock = s;
    HANDLE th = CreateThread(NULL, 0, hs_thread, NULL, 0, NULL); if (th) CloseHandle(th); else { logf_("host mods: CreateThread failed (%lu)", GetLastError()); closesocket(s); g_hs_sock = INVALID_SOCKET; failed = 1; return 0; }
    return 1;
}
// game thread, PreLogin accepted a joiner: it may ask this host; the listener starts at the first one (never in a solo game)
static void hs_on_login(uint32_t ip) {
    if (g_hm_mode == HM_OFF || !g_lm_me.present || !g_lm_me.fp) return;   // switched off, or no gameplay mods to share or name
    hm_init_once();
    EnterCriticalSection(&g_hm_cs); hm_peer_add(&g_hs_peers, ip, GetTickCount64()); LeaveCriticalSection(&g_hm_cs);
    hs_start();
}

// ===== joiner side
static SOCKET hj_connect(void) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (s == INVALID_SOCKET) return s;
    DWORD to = 20000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)g_hj.port); a.sin_addr.s_addr = htonl(g_hj.ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return INVALID_SOCKET; }
    return s;
}
// the address this PC joins (ip.txt line 1); only that host is ever asked
static int hj_host_addr(char *addr, size_t cap, uint32_t *ip) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sip.txt", g_dir); FILE *f = _wfopen(p, L"r"); if (!f) return 0;
    char raw[256]; addr[0] = 0;
    while (fgets(raw, sizeof raw, f)) { char *r = raw; while (*r == ' ' || *r == '\t') r++; if (*r == '#' || *r == '\r' || *r == '\n' || !*r) continue; snprintf(addr, cap, "%s", r); break; }
    fclose(f);
    size_t n = strcspn(addr, ": \t\r\n/?"); addr[n] = 0; if (!n) return 0;
    unsigned long v = inet_addr(addr);
    if (v == INADDR_NONE) { struct hostent *he = gethostbyname(addr); if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return 0; memcpy(&v, he->h_addr_list[0], 4); }
    *ip = ntohl(v); return 1;
}
static int hj_mod_loaded_own(const char *mod) {                         // the player's own Mods\<mod> loaded at boot
    for (int m = 0; m < g_mi_n; m++) if (g_mi_pos[m] >= 0 && !_stricmp(g_mi[m].name, mod)) return m + 1;
    return 0;
}
static int hj_file_has_sha(const wchar_t *path, const HmEntry *e) {
    WIN32_FILE_ATTRIBUTE_DATA fa; if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    if ((((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) != e->size) return 0;
    char hex[65]; return hm_sha_file(path, hex, NULL) && !strcmp(hex, e->sha);
}
static DWORD WINAPI hj_list_thread(LPVOID u) {
    (void)u; HmEntry e[HM_MAX]; HmNoShare ns[HM_MAX]; int n = -1, m = 0, nns = 0; char why[160] = ""; uint64_t hfp = 0;
    SOCKET s = hj_connect();
    if (s == INVALID_SOCKET) snprintf(why, sizeof why, "cannot reach %s TCP %u (error %d; host without this loader version, moddownload off there, or a firewall)", g_hj.addr, g_hj.port, WSAGetLastError());
    else {
        char line[400];
        if (!hs_send_all(s, "LIST\n", 5) || !hs_recv_line(s, line, sizeof line)) snprintf(why, sizeof why, "no answer from the host");
        else if ((n = hm_parse_header(line, &hfp, &m)) < 0) snprintf(why, sizeof why, "the host answered '%.60s'", line);
        else if (g_lm_me.present && hfp == g_lm_me.fp) {                     // same gameplay mods: nothing to say
            closesocket(s); EnterCriticalSection(&g_hm_cs); hm_set(HJ_IDLE, "Host mods: the host's gameplay mods match mine (%016llX)", (unsigned long long)hfp); g_hj.shown_seq = g_hj.seq; LeaveCriticalSection(&g_hm_cs); return 0;
        }
        else for (int i = 0; i < n + m; i++) {
            if (!hs_recv_line(s, line, sizeof line)) { snprintf(why, sizeof why, "the list ended early"); n = -1; break; }
            HmNoShare one;
            if (i < n ? !hm_parse_line(line, &e[i], why, sizeof why) : !hm_parse_noshare(line, &one, why, sizeof why)) { logf_("host mods: bad list line %d: %s", i, why); n = -1; break; }
            if (i >= n) {                                                // a mod the host may not share: worth naming only when this PC lacks it
                int have = hj_mod_loaded_own(one.mod) != 0; for (int k = 0; k < g_hfm_n; k++) if (!_stricmp(g_hfm[k], one.mod)) have = 1;
                logf_("host mods: the host runs %s, not shareable%s", one.mod, have ? " (you load a mod of that name)" : " - you do not have it");
                if (!have) ns[nns++] = one;
            }
        }
        closesocket(s);
    }
    EnterCriticalSection(&g_hm_cs);
    if (n < 0) { hm_set(HJ_FAILED, "Host mods: could not get the list: %s", why); LeaveCriticalSection(&g_hm_cs); return 0; }
    LeaveCriticalSection(&g_hm_cs);
    int want[HM_MAX] = { 0 }, have[HM_MAX] = { 0 };                       // hashing happens outside the lock
    for (int i = 0; i < n; i++) {
        wchar_t p[MAX_PATH], wp[HM_NAME_MAX]; u8_to_w(e[i].pak, wp, HM_NAME_MAX);
        int own = hj_mod_loaded_own(e[i].mod);
        if (own) { swprintf(p, MAX_PATH, L"%ls\\%ls", g_mi_dir[own - 1], wp); if (hj_file_has_sha(p, &e[i])) { logf_("host mods: %s/%s - you load the same file already", e[i].mod, e[i].pak); continue; } }
        hm_fromhost_dir(g_hj.key, e[i].mod, p); { size_t L = wcslen(p); swprintf(p + L, MAX_PATH - L, L"\\%ls", wp); }
        int mounted = 0; for (int k = 0; k < g_hfm_n; k++) if (!_stricmp(g_hfm[k], e[i].mod)) mounted = 1;
        if (hj_file_has_sha(p, &e[i])) { have[i] = 1; if (mounted && !strcmp(g_hm_active, g_hj.key)) { logf_("host mods: %s/%s - loaded from _FromHost already", e[i].mod, e[i].pak); continue; } }
        if (own) logf_("host mods: %s/%s - your own %s differs from the host's; the host's copy goes to _FromHost and loads only when your own is switched off", e[i].mod, e[i].pak, e[i].mod);
        want[i] = 1;
    }
    uint64_t total = 0; int cnt = 0; for (int i = 0; i < n; i++) cnt += want[i];
    EnterCriticalSection(&g_hm_cs);
    memcpy(g_hj.e, e, sizeof(HmEntry) * (size_t)n); g_hj.n = n; memcpy(g_hj.want, want, sizeof want); memcpy(g_hj.have_dl, have, sizeof have);
    memcpy(g_hj.ns, ns, sizeof(HmNoShare) * (size_t)nns); g_hj.nns = nns;
    int wdl[HM_MAX]; for (int i = 0; i < n; i++) wdl[i] = want[i] && !have[i];
    if (!cnt && nns) hm_set(HJ_NOTHING, "Host mods: %d mod(s) you lack are not shareable - get them from their authors", nns);
    else if (!cnt) hm_set(HJ_NOTHING, "Host mods: nothing to download - the difference is not in pak files the host shares (rows.json, plugins, load order or your own extra mods)");
    else if (!hm_caps_ok(e, wdl, n, &total, why, sizeof why)) hm_set(HJ_FAILED, "Host mods: not offered: %s", why);
    else { g_hj.total = total; hm_set(HJ_OFFER, "Host mods: %d file(s) offered", cnt); }
    LeaveCriticalSection(&g_hm_cs);
    return 0;
}
static void hm_write_text(const wchar_t *path, const char *text) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD w;
    if (h == INVALID_HANDLE_VALUE) { logf_("host mods: cannot write %ls (error %lu)", path, GetLastError()); return; }
    WriteFile(h, text, (DWORD)strlen(text), &w, NULL); CloseHandle(h);
}
// downloads entry i to its final place. 1 = ok (why empty), 0 = failed (why says why; nothing left behind)
// *refused = 1 when the file arrived but failed the pak / content check (a hostile or broken pak): the caller then refuses the whole download
static int hj_fetch(int i, char *why, size_t wcap, int *refused) {
    const HmEntry *e = &g_hj.e[i]; wchar_t dir[MAX_PATH], part[MAX_PATH], fin[MAX_PATH], wp[HM_NAME_MAX]; u8_to_w(e->pak, wp, HM_NAME_MAX);
    hm_fromhost_dir(NULL, NULL, dir); CreateDirectoryW(dir, NULL); hm_fromhost_dir(g_hj.key, NULL, dir); CreateDirectoryW(dir, NULL);
    hm_fromhost_dir(g_hj.key, e->mod, dir); CreateDirectoryW(dir, NULL);
    swprintf(fin, MAX_PATH, L"%ls\\%ls", dir, wp); swprintf(part, MAX_PATH, L"%ls.part", fin);
    SOCKET s = hj_connect(); if (s == INVALID_SOCKET) { snprintf(why, wcap, "cannot reach the host (error %d)", WSAGetLastError()); return 0; }
    char line[200]; snprintf(line, sizeof line, "GET %d\n", i);
    unsigned long long sz = 0;
    if (!hs_send_all(s, line, (int)strlen(line)) || !hs_recv_line(s, line, sizeof line) || sscanf(line, "OK %llu", &sz) != 1 || sz != e->size) {
        closesocket(s); snprintf(why, wcap, "the host answered '%.60s' (announced %llu bytes)", line, (unsigned long long)e->size); return 0; }
    HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { closesocket(s); snprintf(why, wcap, "cannot create the file (error %lu)", GetLastError()); return 0; }
    HmSha sh; int ok = hm_sha_begin(&sh); uint64_t got = 0; static char buf[1 << 16];
    while (ok && got < e->size) {
        int want = (int)((e->size - got) < sizeof buf ? (e->size - got) : sizeof buf), k = recv(s, buf, want, 0);
        if (k <= 0) { ok = 0; snprintf(why, wcap, "connection lost after %llu of %llu bytes", (unsigned long long)got, (unsigned long long)e->size); break; }
        DWORD w; if (!WriteFile(f, buf, (DWORD)k, &w, NULL) || w != (DWORD)k) { ok = 0; snprintf(why, wcap, "disk write failed (error %lu)", GetLastError()); break; }
        hm_sha_add(&sh, buf, (size_t)k); got += (uint64_t)k;
        EnterCriticalSection(&g_hm_cs); g_hj.got += (uint64_t)k; LeaveCriticalSection(&g_hm_cs);
    }
    closesocket(s); CloseHandle(f);
    char hex[65]; if (!hm_sha_end(&sh, hex) && ok) { ok = 0; snprintf(why, wcap, "SHA-256 failed"); }
    if (ok && strcmp(hex, e->sha)) { ok = 0; snprintf(why, wcap, "SHA-256 mismatch (got %.16s..., host announced %.16s...)", hex, e->sha); }
    int ini = 0; unsigned char ih[20];
    if (ok && !hm_check_pak_file(part, &ini, ih, why, wcap)) { ok = 0; *refused = 1; }
    if (ok && ini != e->ini) { ok = 0; *refused = 1; snprintf(why, wcap, "%s Config/*.ini, but the host announced %s", ini ? "carries" : "carries no", e->ini ? "it does" : "it does not"); }
    if (ok && !MoveFileExW(part, fin, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { ok = 0; snprintf(why, wcap, "rename failed (error %lu)", GetLastError()); }
    if (!ok) DeleteFileW(part);
    return ok;
}
static DWORD WINAPI hj_download_thread(LPVOID u) {
    (void)u; int okn = 0, bad = 0, refused = -1; char first_err[200] = ""; static unsigned char fetched[HM_MAX];
    memset(fetched, 0, sizeof fetched);
    for (int i = 0; i < g_hj.n && refused < 0; i++) {                   // g_hj.e / want / have_dl do not change while HJ_DOWNLOADING
        if (!g_hj.want[i] || g_hj.have_dl[i]) continue;
        char why[200] = ""; int ok = 0, ref = 0;
        __try { ok = hj_fetch(i, why, sizeof why, &ref); } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(why, sizeof why, "EXCEPTION 0x%08lX", GetExceptionCode()); }
        logf_("host mods: download %s/%s: %s%s", g_hj.e[i].mod, g_hj.e[i].pak, ok ? "OK, verified (SHA-256, pak, packed types)" : ref ? "REFUSED: " : "FAILED: ", why);
        if (ok) { okn++; g_hj.have_dl[i] = 1; fetched[i] = 1; } else { bad++; if (!first_err[0]) snprintf(first_err, sizeof first_err, "%s/%s: %s", g_hj.e[i].mod, g_hj.e[i].pak, why); }
        if (ref) refused = i;                                           // a pak that fails the content check: refuse the whole download
        EnterCriticalSection(&g_hm_cs); g_hj.files_done++; LeaveCriticalSection(&g_hm_cs);
    }
    if (refused >= 0) {                                                 // remove what this download already wrote; nothing gets activated
        for (int i = 0; i < g_hj.n; i++) if (fetched[i]) {
            wchar_t dir[MAX_PATH], fin[MAX_PATH], wp[HM_NAME_MAX]; u8_to_w(g_hj.e[i].pak, wp, HM_NAME_MAX); hm_fromhost_dir(g_hj.key, g_hj.e[i].mod, dir);
            swprintf(fin, MAX_PATH, L"%ls\\%ls", dir, wp); BOOL del = DeleteFileW(fin); g_hj.have_dl[i] = 0;
            logf_("host mods: download refused - removed %s/%s again (%s)", g_hj.e[i].mod, g_hj.e[i].pak, del ? "deleted" : "delete FAILED");
        }
        EnterCriticalSection(&g_hm_cs); hm_set(HJ_FAILED, "Host mods: download REFUSED, nothing installed - %s", first_err); LeaveCriticalSection(&g_hm_cs);
        return 0;
    }
    // order.txt = the host's load order of every mod with files in this host's folder; active.txt = this host
    static char order[HM_MAX * (HM_NAME_MAX + 2) + 4], src[HM_MAX * 200 + 256]; order[0] = 0;
    int ow = 0, sw = snprintf(src, sizeof src, "# files downloaded from %s (host key %s); SHA-256 checked at download\r\n", g_hj.addr, g_hj.key);
    for (int i = 0; i < g_hj.n; i++) {
        if (!g_hj.have_dl[i]) continue;
        char needle[HM_NAME_MAX + 3]; snprintf(needle, sizeof needle, "%s\r\n", g_hj.e[i].mod);
        if (!strstr(order, needle)) ow += snprintf(order + ow, sizeof order - (size_t)ow, "%s", needle);
        sw += snprintf(src + sw, sizeof src - (size_t)sw, "%s/%s\t%llu\t%s\r\n", g_hj.e[i].mod, g_hj.e[i].pak, (unsigned long long)g_hj.e[i].size, g_hj.e[i].sha);
    }
    wchar_t p[MAX_PATH], d[MAX_PATH];
    if (ow) {
        hm_fromhost_dir(g_hj.key, NULL, d);
        swprintf(p, MAX_PATH, L"%ls\\order.txt", d); hm_write_text(p, order);
        swprintf(p, MAX_PATH, L"%ls\\source.txt", d); hm_write_text(p, src);
        hm_fromhost_dir(NULL, NULL, d); swprintf(p, MAX_PATH, L"%ls\\active.txt", d); { char a[HM_NAME_MAX + 4]; snprintf(a, sizeof a, "%s\r\n", g_hj.key); hm_write_text(p, a); }
    }
    EnterCriticalSection(&g_hm_cs);
    if (bad) hm_set(HJ_FAILED, "Host mods: %d file(s) installed, %d FAILED (%s)", okn, bad, first_err);
    else hm_set(HJ_DONE, "Host mods: %d file(s) installed in Mods\\_FromHost\\%s. Restart the game and join again to load them.", okn, g_hj.key);
    LeaveCriticalSection(&g_hm_cs);
    return 0;
}
// game thread: this PC just got a server connection (joined a host). Ask that host (ip.txt) what it runs.
static void hj_start(void) {
    if (g_hm_mode == HM_OFF || !g_hm_wsa) return;
    char addr[64]; uint32_t ip;
    if (!hj_host_addr(addr, sizeof addr, &ip)) { logf_("host mods: joined a host, but ip.txt holds no usable address - cannot ask it for mods"); return; }
    EnterCriticalSection(&g_hm_cs);
    int busy = g_hj.state == HJ_LISTING || g_hj.state == HJ_DOWNLOADING;
    if (!busy) {
        memset(g_hj.e, 0, sizeof g_hj.e); g_hj.n = 0; g_hj.nns = 0; g_hj.got = 0; g_hj.files_done = 0;
        snprintf(g_hj.addr, sizeof g_hj.addr, "%s", addr); hm_host_key(addr, g_hj.key, sizeof g_hj.key); g_hj.ip = ip; g_hj.port = g_hm_port;
        hm_set(HJ_LISTING, "Host mods: joined %s - asking it (TCP %u) which mods it runs", addr, g_hm_port);
    }
    LeaveCriticalSection(&g_hm_cs);
    if (busy) { logf_("host mods: joined again while a list / download is running - not asked again"); return; }
    HANDLE th = CreateThread(NULL, 0, hj_list_thread, NULL, 0, NULL); if (th) CloseHandle(th);
    else { EnterCriticalSection(&g_hm_cs); hm_set(HJ_FAILED, "Host mods: CreateThread failed (%lu)", GetLastError()); LeaveCriticalSection(&g_hm_cs); }
}
// game thread: the current server connection (UNetDriver::ServerConnection of the world's net driver; NULL = not a joined client)
static void *hj_server_conn(void) {
    void *sc = NULL;
    __try { void *w = world(), *nd = w ? *(void **)((uint8_t *)w + OFF_W_NETDRIVER) : NULL; sc = nd ? *(void **)((uint8_t *)nd + OFF_ND_SERVERCONN) : NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { sc = NULL; }
    return sc;
}

// ----- F2 Mods page rows (modsui.inc appends them after its own actions; clicking one calls hm_menu_action)
enum { HMA_MODE = 1, HMA_MAXP, HMA_UNLOAD, HMA_INSTALL, HMA_SKIP, HMA_QUIT, HMA_INFO };
static char g_hm_lbl[2 * HM_MAX + 12][96]; static int g_hm_act[2 * HM_MAX + 12], g_hm_nl;
static int hm_menu_count(void) {
    hm_init_once(); int n = 0;
#define HM_ROW(kind, ...) do { if (n < (int)(sizeof g_hm_act / sizeof g_hm_act[0])) { snprintf(g_hm_lbl[n], sizeof g_hm_lbl[0], __VA_ARGS__); g_hm_act[n++] = (kind); } } while (0)
    HM_ROW(HMA_MODE, "Host mod download: %s", g_hm_mode == HM_OFF ? "OFF  (click: prompt)" : "PROMPT  (click: off)");
    if (g_mp_cap) HM_ROW(HMA_MAXP, "Max players when hosting: %d  (click: next)", g_mp_cap); else HM_ROW(HMA_MAXP, "Max players when hosting: off  (click: 2)");
    if (g_hm_active[0]) HM_ROW(HMA_UNLOAD, "Host mods from %s load at start (%d)  (click: stop at next start)", g_hm_active, g_hfm_n);
    EnterCriticalSection(&g_hm_cs);
    char sz[24], sz2[24];
    switch (g_hj.state) {
    case HJ_LISTING: HM_ROW(HMA_INFO, "Host mods: asking %s ...", g_hj.addr); break;
    case HJ_OFFER: {
        static char lines[2 * HM_MAX + 3][HM_LINE]; int k = hm_prompt_lines(g_hj.addr, g_hj.e, g_hj.want, g_hj.have_dl, g_hj.n, g_hj.total, g_hj.ns, g_hj.nns, lines, 2 * HM_MAX + 3);
        for (int i = 0; i < k; i++) HM_ROW(HMA_INFO, "%s", lines[i]);
        hm_size_text(g_hj.total, sz, sizeof sz); HM_ROW(HMA_INSTALL, "INSTALL these from %s (%s)", g_hj.addr, sz);
        HM_ROW(HMA_SKIP, "SKIP (change nothing)"); break; }
    case HJ_DOWNLOADING: hm_size_text(g_hj.got, sz, sizeof sz); hm_size_text(g_hj.total, sz2, sizeof sz2); HM_ROW(HMA_INFO, "Host mods: downloading %s of %s ...", sz, sz2); break;
    case HJ_DONE: HM_ROW(HMA_INFO, "%.92s", g_hj.msg); HM_ROW(HMA_QUIT, "Quit the game now (start it again and rejoin)"); break;
    case HJ_NOTHING:
        if (g_hj.nns) { static char lines[2 * HM_MAX + 3][HM_LINE]; int k = hm_prompt_lines(g_hj.addr, g_hj.e, g_hj.want, g_hj.have_dl, g_hj.n, 0, g_hj.ns, g_hj.nns, lines, 2 * HM_MAX + 3);
                        for (int i = 0; i < k; i++) HM_ROW(HMA_INFO, "%s", lines[i]); break; }
        HM_ROW(HMA_INFO, "%.92s", g_hj.msg); break;
    case HJ_FAILED: case HJ_SKIPPED: HM_ROW(HMA_INFO, "%.92s", g_hj.msg); break;
    default: break;
    }
    LeaveCriticalSection(&g_hm_cs);
#undef HM_ROW
    g_hm_nl = n; return n;
}
static const char *hm_menu_label(int i) { return i >= 0 && i < g_hm_nl ? g_hm_lbl[i] : ""; }
static void hm_menu_action(int i) {
    if (i < 0 || i >= g_hm_nl) return;
    int a = g_hm_act[i]; char v[16];
    switch (a) {
    case HMA_MODE: g_hm_mode = g_hm_mode == HM_OFF ? HM_PROMPT : HM_OFF; hm_cfg_write("moddownload", g_hm_mode == HM_OFF ? "off" : "prompt"); break;
    case HMA_MAXP: g_mp_cap = mp_next(g_mp_cap); if (g_mp_cap) snprintf(v, sizeof v, "%d", g_mp_cap); else snprintf(v, sizeof v, "off"); hm_cfg_write("maxplayers", v); break;
    case HMA_UNLOAD: { wchar_t d[MAX_PATH], p[MAX_PATH]; hm_fromhost_dir(NULL, NULL, d); swprintf(p, MAX_PATH, L"%ls\\active.txt", d);
        logf_("host mods: player stopped loading %s at start: %s", g_hm_active, DeleteFileW(p) ? "active.txt deleted" : "active.txt not found"); g_hm_active[0] = 0; break; }
    case HMA_INSTALL: {
        EnterCriticalSection(&g_hm_cs); int go = g_hj.state == HJ_OFFER;
        if (go) { g_hj.got = 0; g_hj.files_done = 0; g_hj.files_all = 0; for (int k = 0; k < g_hj.n; k++) g_hj.files_all += g_hj.want[k] && !g_hj.have_dl[k]; hm_set(HJ_DOWNLOADING, "Host mods: the player clicked Install - %d file(s) to download", g_hj.files_all); }
        LeaveCriticalSection(&g_hm_cs);
        if (go) { HANDLE th = CreateThread(NULL, 0, hj_download_thread, NULL, 0, NULL); if (th) CloseHandle(th); else { EnterCriticalSection(&g_hm_cs); hm_set(HJ_FAILED, "Host mods: CreateThread failed"); LeaveCriticalSection(&g_hm_cs); } }
        break; }
    case HMA_SKIP: EnterCriticalSection(&g_hm_cs); if (g_hj.state == HJ_OFFER) hm_set(HJ_SKIPPED, "Host mods: skipped - nothing was downloaded or changed"); LeaveCriticalSection(&g_hm_cs); break;
    case HMA_QUIT: logf_("host mods: quit requested from the Mods page"); md_console("quit"); break;
    default: break;
    }
}
LN_MODS_ROWS(0100, hostmods, hm_menu_count, hm_menu_label, hm_menu_action)
// game thread, every frame: one toast per state change
static void hm_tick(void) {
    if (!g_hm_init) return;
    static uint64_t next; uint64_t now = GetTickCount64(); if (now < next) return; next = now + 250;
    {   // a new server connection = this PC joined a host: ask it once, 3 s later (the host has accepted the login by then)
        static void *last_sc; static uint64_t seen_at; static int asked; void *sc = hj_server_conn();
        if (sc != last_sc) { last_sc = sc; seen_at = now; asked = 0; if (sc) logf_("host mods: connected to a host as a client"); }
        if (sc && !asked && now - seen_at >= 3000) { asked = 1; hj_start(); }
    }
    EnterCriticalSection(&g_hm_cs);
    int seq = g_hj.seq, st = g_hj.state; char lines[8][LM_LINE_MAX]; int n = 0;
    if (seq != g_hj.shown_seq) {
        g_hj.shown_seq = seq;
        if (st == HJ_OFFER || (st == HJ_NOTHING && g_hj.nns)) n = hm_prompt_lines(g_hj.addr, g_hj.e, g_hj.want, g_hj.have_dl, g_hj.n, g_hj.total, g_hj.ns, g_hj.nns, lines, 8);
        else if (st != HJ_LISTING && st != HJ_IDLE) { snprintf(lines[0], LM_LINE_MAX, "%.98s", g_hj.msg); n = 1; }
    }
    LeaveCriticalSection(&g_hm_cs);
    if (n) toast_show(lines, n, st == HJ_OFFER ? 30 : 15);
}
LN_TICK(0400, hostmods) { hm_tick(); }                    // host mod download: asks a joined host, toasts

// ----- boot: mount Mods\_FromHost\<active host>\<Mod>\*.pak after the player's own mods (modglue.inc calls this before the LAN fingerprint is final)
static void hm_mount_fromhost(void *self, PakMount_t mount, int *position, uint64_t *fp, uint64_t *gp_fp, const char **gpn, int *ngp, int *mounted_mods, const wchar_t *sigsrc, int have_sigsrc) {
    wchar_t d[MAX_PATH], p[MAX_PATH]; hm_fromhost_dir(NULL, NULL, d); swprintf(p, MAX_PATH, L"%ls\\active.txt", d);
    char *t = mods_read_text(p, 4096); if (!t) { logf_("host mods: no Mods\\_FromHost\\active.txt - no host mods load"); return; }
    char key[HM_NAME_MAX] = ""; sscanf(t, "%63s", key); free(t);
    if (!hm_name_ok(key)) { logf_("host mods: active.txt names '%s' - not a valid host folder, ignored", key); return; }
    snprintf(g_hm_active, sizeof g_hm_active, "%s", key);
    hm_fromhost_dir(key, NULL, d); swprintf(p, MAX_PATH, L"%ls\\order.txt", d);
    char *ord = mods_read_text(p, 65536); if (!ord) { logf_("host mods: %ls missing - nothing loads from host %s", p, key); return; }
    logf_("host mods: loading the mods downloaded from host %s (Mods\\_FromHost\\%s), after your own", key, key);
    for (char *ln = strtok(ord, "\r\n"); ln; ln = strtok(NULL, "\r\n")) {
        char mod[HM_NAME_MAX]; if (sscanf(ln, "%63s", mod) != 1 || !hm_name_ok(mod)) { logf_("host mods: order.txt line '%.40s' ignored", ln); continue; }
        int dup = 0; for (int i = 0; i < *position; i++) if (!_stricmp(g_posname[i], mod)) dup = 1;
        if (dup) { logf_("host mods: %s - your own Mods\\%s is loaded; the host's copy is not mounted", mod, mod); continue; }
        if (*position >= MODS_MAX) { logf_("host mods: %s - load position limit reached", mod); break; }
        wchar_t md[MAX_PATH]; hm_fromhost_dir(key, mod, md); swprintf(p, MAX_PATH, L"%ls\\*.pak", md);
        char names[16][HM_NAME_MAX]; int nn = 0; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(p, &fd);
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nn < 16) { char u[MAX_PATH]; w_to_u8(fd.cFileName, u, sizeof u); if (hm_pak_name_ok(u)) snprintf(names[nn++], HM_NAME_MAX, "%s", u); } } while (FindNextFileW(h, &fd)); FindClose(h); }
        qsort(names, (size_t)nn, sizeof names[0], (int (*)(const void *, const void *))_stricmp);
        int ok_paks = 0;
        for (int k = 0; k < nn; k++) {
            wchar_t wp[HM_NAME_MAX], full[MAX_PATH], sig[MAX_PATH]; u8_to_w(names[k], wp, HM_NAME_MAX); swprintf(full, MAX_PATH, L"%ls\\%ls", md, wp);
            int ini = 0; unsigned char ih[20] = { 0 }; char why[160] = "";
            if (!hm_check_pak_file(full, &ini, ih, why, sizeof why)) { logf_("host mods: %s/%s NOT mounted: %s", mod, names[k], why); continue; }
            wcscpy(sig, full); wcscpy(sig + wcslen(sig) - 4, L".sig");
            if (GetFileAttributesW(sig) == INVALID_FILE_ATTRIBUTES && (!have_sigsrc || !CopyFileW(sigsrc, sig, TRUE))) { logf_("host mods: %s/%s NOT mounted: no .sig and none could be created", mod, names[k]); continue; }
            wchar_t fwd[MAX_PATH]; wcscpy(fwd, full); for (wchar_t *c = fwd; *c; c++) if (*c == L'\\') *c = L'/';
            uint8_t r = 0; uint32_t order = mod_read_order(*position, k);
            __try { r = mount(self, fwd, order, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("host mods: EXCEPTION 0x%08lX mounting %s/%s", GetExceptionCode(), mod, names[k]); }
            logf_("host mods: %s/%s -> %s (read order %u)%s", mod, names[k], r ? "mounted" : "NOT mounted", order, ini ? " - carries Config/*.ini" : "");
            if (r) { ok_paks++; *fp = mods_fingerprint_add(*fp, mod, ih); *gp_fp = mods_fingerprint_add(*gp_fp, mod, ih); }
        }
        if (ok_paks) {
            snprintf(g_posname[*position], MOD_NAME_MAX, "%s", mod); snprintf(g_hfm[g_hfm_n], HM_NAME_MAX, "%s", mod);
            if (*ngp < MODS_MAX) gpn[(*ngp)++] = g_hfm[g_hfm_n];
            g_hfm_n++; (*position)++; (*mounted_mods)++;
        }
    }
    free(ord);
    logf_("host mods: %d mod(s) from host %s loaded", g_hfm_n, key);
}

static void hostmods_install(void) {
    char v[32] = ""; wchar_t path[MAX_PATH]; int bad = 0;
    if (setting_get("maxplayers", L"maxplayers.txt", 0, v, sizeof v, path)) { g_mp_cap = mp_parse(v, &bad); if (bad) logf_("max players: '%s' in %ls is not 2-%d or off - using %d", v, path, MP_MAX, MP_DEFAULT); }
    if (g_mp_cap) logf_("max players: %d when hosting (the host included; 'maxplayers = off' in config.txt removes the cap)", g_mp_cap); else logf_("max players: off (no cap)");
    v[0] = 0; if (setting_get("moddownload", L"moddownload.txt", 0, v, sizeof v, path)) g_hm_mode = hm_parse_mode(v);
    v[0] = 0; if (setting_get("moddownload_port", L"", 0, v, sizeof v, path)) { int p = atoi(v); if (p > 0 && p < 65536) g_hm_port = (unsigned)p; else logf_("host mods: moddownload_port '%s' invalid - using %u", v, HM_PORT); }
    logf_("host mods: %s (moddownload = prompt | off), side channel TCP %u", g_hm_mode == HM_OFF ? "OFF - this PC neither offers its mods nor asks a host" : "PROMPT - a host's missing mods are offered; nothing installs without a click", g_hm_port);
    hm_init_once();
}
LN_INIT(0500, hostmods) { hostmods_install(); }

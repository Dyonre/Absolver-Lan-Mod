// ---------------------------------------------------------------------------------------------------------------------
// LAN finder (included by lannative.c after lanmods.inc; protocol and host list in landisc.h).
//   host:   while this PC runs a listen server it answers LAN finder queries on UDP LD_PORT (7778) with its character, map, mod fingerprint.
//   finder: while the game's altar menu is open, or the F1 "LAN games" page is open, it asks every 2 s (broadcast on every adapter + the ip.txt
//           address) and lists the hosts that answer. At an altar F1 opens straight on that list; Enter writes ip.txt and joins like "Join".
// Everything runs on the game thread from ld_tick with non-blocking sockets: no threads, no locks. Config: lanfinder = on (default) | off.
#include <iphlpapi.h>
#include "landisc.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define RVA_PC_ISMENUINSTACK 0x35BEA0u   // AFightingPlayerController::BPF_IsMenuInStack: menu stack TArray<FMenuStackInfos> at +0x1248 (data) / +0x1250 (num), 12-byte items, byte 0 = EMenuEnum
#define OFF_FPC_MENUSTACK    0x1248u
#define MENU_ENUM_ALTAR      12          // EMenuEnum::AltarMenu (SDK dump)
#define OFF_SAVED_CHARANAME  0x958u      // FAbsolverSaveData::m_CharaName (FString, SDK dump; same struct as OFF_SAVED_SPAWNPOINT)

static int g_ld_on = 1, g_ld_faults, g_ld_wsa, g_ld_altar, g_ld_menu_ok = -1;
static SOCKET g_ld_hsock = INVALID_SOCKET, g_ld_fsock = INVALID_SOCKET;
static LdList g_ld_list;
static LdRate g_ld_rate;
static char g_ld_id[12];
static int hm_host_open(void);                                // hostmods.inc (hosting switch)
static uint64_t g_ld_next_query, g_ld_next_poll, g_ld_next_altar, g_ld_answered;
static int g_ld_bind_logged;

static void ld_install(void) {
    char v[16]; wchar_t path[MAX_PATH];
    if (setting_get("lanfinder", L"lanfinder.txt", 0, v, sizeof v, path) && !_strnicmp(v, "off", 3)) { g_ld_on = 0; logf_("lan finder: OFF (%ls) - this PC neither answers nor searches", path); return; }
    WSADATA wd; int rc = WSAStartup(MAKEWORD(2, 2), &wd);
    if (rc) { g_ld_on = 0; logf_("lan finder: WSAStartup failed (%d) - off for this run", rc); return; }
    g_ld_wsa = 1;
    unsigned r = 0; if (rand_s(&r)) r = GetTickCount() ^ GetCurrentProcessId();
    snprintf(g_ld_id, sizeof g_ld_id, "%08X", r);
    logf_("lan finder: on (UDP %d; answers while hosting, searches while an altar or F2 > LAN games is open; id %s)", LD_PORT, g_ld_id);
}
static SOCKET ld_open(unsigned short port, int broadcast) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET) return s;
    u_long nb = 1; BOOL b = TRUE; struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (ioctlsocket(s, FIONBIO, &nb) || (broadcast && setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&b, sizeof b)) || bind(s, (struct sockaddr *)&a, sizeof a)) {
        int e = WSAGetLastError(); closesocket(s); WSASetLastError(e); return INVALID_SOCKET;
    }
    return s;
}
static void ld_close(SOCKET *s) { if (*s != INVALID_SOCKET) { closesocket(*s); *s = INVALID_SOCKET; } }

// ----- host side
// 0 = single player, 1 = this PC hosts (listen server), 2 = joined someone (client). A client world can still show an authority game mode
// while it loads (in game LJ-1d: the joiner then tried to answer LAN searches), so the net driver's server connection decides.
static int ld_net_role(void) {
    void *w = world(); if (!w) return 0;
    void *nd = *(void **)((uint8_t *)w + OFF_W_NETDRIVER); if (!nd) return 0;
    if (*(void **)((uint8_t *)nd + OFF_ND_SERVERCONN)) return 2;
    return *(void **)((uint8_t *)w + OFF_W_AUTHGM) ? 1 : 0;
}
static int ld_hosting(void) { return ld_net_role() == 1; }
static void ld_fstring(const uint8_t *fs, char *out, int cap) {
    const wchar_t *d = *(wchar_t *const *)fs; int num = *(const int *)(fs + 8); out[0] = 0;
    if (d && num > 1 && num < 200) narrow(d, num - 1, out, cap);
}
static void ld_host_info(LdInfo *i) {
    memset(i, 0, sizeof *i);
    snprintf(i->id, sizeof i->id, "%s", g_ld_id); snprintf(i->ver, sizeof i->ver, "%s", GLEAM_VERSION); i->port = 7777;
    void *w = world(), *gi = w ? *(void **)((uint8_t *)w + OFF_W_GAMEINST) : NULL;
    if (gi && object_is_a(gi, api_cls_gi())) {
        void *save = *(void **)((uint8_t *)gi + OFF_GI_CURRENTSAVE);
        if (save) ld_fstring((uint8_t *)save + OFF_SAVE_SAVEDDATA + OFF_SAVED_CHARANAME, i->name, sizeof i->name);
    }
    char path[300] = "";
    if (w && api_object_path(w, path, sizeof path)) {
        i->duel = pvp_path_is_duel(path);
        const char *s = strrchr(path, '/'); s = s ? s + 1 : path; char m[64]; snprintf(m, sizeof m, "%s", s); char *dot = strchr(m, '.'); if (dot) *dot = 0;
        snprintf(i->map, sizeof i->map, "%s", m);
    }
    i->players = 1 + T_peers_now();
    if (strcmp(g_cmd_applied, "auto")) snprintf(i->addr, sizeof i->addr, "%s", g_cmd_applied);     // hostip: the game listens on that address only
    if (g_lm_mode && g_lm_me.present) { i->fp = g_lm_me.fp; i->has_fp = 1; snprintf(i->mods, sizeof i->mods, "%s", g_lm_me.names); }
}
static void ld_host_step(uint64_t now) {
    if (!ld_hosting()) { if (g_ld_hsock != INVALID_SOCKET) { ld_close(&g_ld_hsock); logf_("lan finder: not hosting any more - stopped answering"); } g_ld_bind_logged = 0; return; }
    if (g_ld_hsock == INVALID_SOCKET) {
        static uint64_t retry_at; if (now < retry_at) return;
        g_ld_hsock = ld_open(LD_PORT, 0);
        if (g_ld_hsock == INVALID_SOCKET) { retry_at = now + 1000; if (!g_ld_bind_logged) { g_ld_bind_logged = 1; logf_("lan finder: cannot listen on UDP %d yet (error %d%s) - retrying; until then LAN players do not see this game in their list (joining by address works)", LD_PORT, WSAGetLastError(), WSAGetLastError() == WSAEADDRINUSE ? ": another program uses that port" : ""); } return; }
        logf_("lan finder: hosting - answering LAN searches on UDP %d", LD_PORT);
    }
    LdInfo info; int have = 0;
    for (int k = 0; k < 16; k++) {
        char buf[2048]; struct sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(g_ld_hsock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue; break; }
        char qid[12]; uint32_t ip = ntohl(from.sin_addr.s_addr);
        if (!ld_is_query(buf, n, qid) || !strcmp(qid, g_ld_id)) continue;                       // not ours / our own finder
        if (!hm_host_open() || !jg_allowed_ip(&g_jg, ip) || !ld_rate_ok(&g_ld_rate, now, 20)) continue;   // hosting OFF (F2 > Hosting): not listed
        if (!have) { ld_host_info(&info); have = 1; }
        char r[LD_REPLY_MAX + 1]; int rn = ld_build_reply(&info, r, sizeof r);
        if (rn > 0 && sendto(g_ld_hsock, r, rn, 0, (struct sockaddr *)&from, sizeof from) == rn) {
            g_ld_answered++;
            if (g_ld_answered <= 10 || g_ld_answered % 200 == 0) { char ipt[24], mk[40]; ld_ip_text(ip, ipt, sizeof ipt); jg_mask_addr(ipt, mk, sizeof mk); logf_("lan finder: answered a search from %s (%llu so far): '%s' on %s", mk, (unsigned long long)g_ld_answered, info.name, info.map); }
        }
    }
}

// ----- finder side
static void ld_send_query(void) {
    char q[LD_QUERY_LEN]; ld_build_query(g_ld_id, q, sizeof q);
    uint32_t dst[24]; int nd = 0; dst[nd++] = 0xFFFFFFFFu;
    ULONG sz = 0; GetIpAddrTable(NULL, &sz, FALSE);
    MIB_IPADDRTABLE *t = sz ? (MIB_IPADDRTABLE *)malloc(sz) : NULL;
    if (t && GetIpAddrTable(t, &sz, FALSE) == NO_ERROR)
        for (DWORD k = 0; k < t->dwNumEntries && nd < 23; k++) { uint32_t b = ld_bcast(ntohl(t->table[k].dwAddr), ntohl(t->table[k].dwMask)); int dup = 0; for (int d = 0; d < nd; d++) dup |= dst[d] == b; if (b && !dup) dst[nd++] = b; }
    free(t);
    char ja[64]; uint32_t uip; snprintf(ja, sizeof ja, "%s", g_joinaddr_text); { char *c = strchr(ja, ':'); if (c) *c = 0; }
    if (ld_ip_parse(ja, &uip)) dst[nd++] = uip;                                                        // a VPN without broadcast (Tailscale): ask the ip.txt host directly
    int sent = 0;
    for (int d = 0; d < nd; d++) {
        struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(LD_PORT); a.sin_addr.s_addr = htonl(dst[d]);
        if (sendto(g_ld_fsock, q, LD_QUERY_LEN, 0, (struct sockaddr *)&a, sizeof a) == LD_QUERY_LEN) sent++;
    }
    static int logged; if (logged++ < 3) logf_("lan finder: search sent to %d of %d address(es) (255.255.255.255, %d adapter broadcast(s)%s)", sent, nd, nd - 1 - (ld_ip_parse(ja, &uip) ? 1 : 0), ld_ip_parse(ja, &uip) ? ", the ip.txt address" : "");
}
static void ld_found_toast(const LdHost *h) {
    char lines[1][LM_LINE_MAX]; snprintf(lines[0], LM_LINE_MAX, "LAN game found: %s on %s - press F2 to join", h->info.name[0] ? h->info.name : "a host", h->info.map[0] ? h->info.map : "?");
    toast_show(lines, 1, 6);
}
static void ld_find_step(uint64_t now, int want) {
    if (!want) { if (g_ld_fsock != INVALID_SOCKET) { ld_close(&g_ld_fsock); logf_("lan finder: search stopped (%d host(s) listed)", g_ld_list.n); } return; }
    if (g_ld_fsock == INVALID_SOCKET) {
        g_ld_fsock = ld_open(0, 1);
        if (g_ld_fsock == INVALID_SOCKET) { logf_("lan finder: cannot open the search socket (error %d)", WSAGetLastError()); g_ld_next_query = now + 10000; return; }
        logf_("lan finder: searching the LAN"); g_ld_next_query = 0;
    }
    if (now >= g_ld_next_query) { g_ld_next_query = now + LD_QUERY_MS; ld_send_query(); }
    for (int k = 0; k < 32; k++) {
        char buf[1100]; struct sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(g_ld_fsock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue; break; }   // ICMP "port unreachable" from the ip.txt address: ignore
        LdInfo i; if (!ld_parse_reply(buf, n, &i) || !strcmp(i.id, g_ld_id)) continue;                // our own host answering
        uint32_t ip = ntohl(from.sin_addr.s_addr);
        if (ld_list_note(&g_ld_list, &i, ip, now)) {
            const LdHost *h = NULL; for (int q = 0; q < g_ld_list.n; q++) if (g_ld_list.h[q].from == ip && g_ld_list.h[q].seen == now) h = &g_ld_list.h[q];
            char mk[40], ipt[24]; ld_ip_text(ip, ipt, sizeof ipt); jg_mask_addr(ipt, mk, sizeof mk);
            logf_("lan finder: found '%s' on %s from %s (duel %d, %d player(s), loader %s, mods %s)", i.name, i.map, mk, i.duel, i.players, i.ver,
                  !i.has_fp || !g_lm_me.present ? "not compared" : i.fp == g_lm_me.fp ? "match" : "DIFFER");
            if (h && !g_menu.open) ld_found_toast(h);
        }
    }
}

// ----- altar: the game's own altar menu is in the local controller's menu stack
static int ld_altar_open(void) {
    if (g_ld_menu_ok < 0) {
        static const uint8_t expect[14] = { 0x48, 0x8B, 0x81, 0x48, 0x12, 0x00, 0x00, 0x48, 0x63, 0x89, 0x50, 0x12, 0x00, 0x00 };
        g_ld_menu_ok = !memcmp(g_base + RVA_PC_ISMENUINSTACK, expect, sizeof expect);
        if (!g_ld_menu_ok) logf_("lan finder: BPF_IsMenuInStack bytes differ from this build - the altar does not start a search (F2 > LAN games still does)");
    }
    if (!g_ld_menu_ok) return 0;
    void *pc = L_local_pc(); if (!pc || !object_is_a(pc, api_cls_fightingpc())) return 0;
    const uint8_t *data = *(const uint8_t **)((uint8_t *)pc + OFF_FPC_MENUSTACK); int num = *(const int *)((uint8_t *)pc + OFF_FPC_MENUSTACK + 8);
    if (!data || num <= 0 || num > 64) return 0;
    for (int k = 0; k < num; k++) if (data[k * 12] == MENU_ENUM_ALTAR) return 1;
    return 0;
}

// ----- F1 page
static const char *ld_menu_line(int i) {
    static char lab[MENU_LINE_MAX];
    if (!g_ld_on) return "LAN finder is off (lanfinder = off in config.txt)";
    if (i < 0 || i >= g_ld_list.n) return g_ld_list.n ? "" : "Searching the LAN for games...  (hosts appear here; Enter = search again)";
    ld_label(&g_ld_list.h[i], g_lm_me.present, g_lm_me.fp, lab, sizeof lab); return lab;
}
static void ld_menu_join(int index, uint64_t now) {
    if (index < 0 || index >= g_ld_list.n) { g_ld_next_query = 0; logf_("lan finder: search again (menu)"); return; }
    const LdHost *h = &g_ld_list.h[index];
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sip.txt", g_dir);
    FILE *f = _wfopen(path, L"wb"); if (!f) { logf_("lan finder: cannot write ip.txt"); return; }
    fprintf(f, "# found on the LAN: %s on %s\r\n%s\r\n%s", h->info.name, h->info.map, h->join, h->info.duel ? "none\r\n" : "");   // duel host: bare URL (no co-op CheckPoint)
    fclose(f); joinaddr_refresh();
    logf_("lan finder: join '%s' at %s (%s)", h->info.name, h->join, h->info.duel ? "duel map, bare URL" : "co-op, this save's CheckPoint");
    g_menu.open = 0;                                                  // joining: the list is done (LJ-1d: an open LAN page kept the finder searching after the join); Join reopens it when it asks for a second press
    MenuAct a = { MA_JOIN, 0 }; menu_do(a, now);
}

static void ld_tick(uint64_t now) {
    if (!g_ld_on || g_ld_faults >= 2 || now < g_ld_next_poll) return;
    g_ld_next_poll = now + 50;
    __try {
        ld_host_step(now);
        if (now >= g_ld_next_altar) {
            g_ld_next_altar = now + 250;
            int a = ld_altar_open();
            if (a != g_ld_altar) {
                g_ld_altar = a; logf_("lan finder: altar menu %s", a ? "opened - searching the LAN (F2 = LAN games)" : "closed");
                if (a) { if (!g_menu.open) g_menu.mem_page = MP_LAN; for (int k = 0; k < g_ld_list.n && !g_menu.open; k++) ld_found_toast(&g_ld_list.h[k]); }
                else if (!g_menu.open && g_menu.mem_page == MP_LAN) g_menu.mem_page = MP_MAIN;
            }
        }
        ld_find_step(now, (g_ld_altar && ld_net_role() != 2) || (g_menu.open && g_menu.page == MP_LAN));   // a joined player's altar does not search (the F1 LAN page still does)
        ld_list_expire(&g_ld_list, now);
        g_menu.count[MP_LAN] = g_ld_list.n ? g_ld_list.n : 1;
        if (g_menu.page == MP_LAN && g_menu.sel >= g_menu.count[MP_LAN]) g_menu.sel = g_menu.count[MP_LAN] - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ld_faults++; ld_close(&g_ld_hsock); ld_close(&g_ld_fsock);
        logf_("lan finder: EXCEPTION 0x%08lX (fault %d of 2)%s", GetExceptionCode(), g_ld_faults, g_ld_faults >= 2 ? " - the LAN finder is off for this run (joining by address still works)" : "");
    }
}
static int ld_found(void) { return g_ld_list.n; }
LN_INIT(0400, landisc) { ld_install(); }
LN_TICK(0600, landisc) { ld_tick(GetTickCount64()); }      // answers while hosting, searches at an altar / F1 > LAN games

// ---------------------------------------------------------------------------------------------------------------------
// kick.inc - host-only F1 page "Kick player...". Pure part: kick.h.
// Engine functions used (this game build):
//   AGameSession::KickPlayer(session, APlayerController *, const FText &reason) @0x147BD80: only for a controller with a UNetConnection
//     (+0x408); destroys its pawn (+0x3A8), sends the ClientWasKicked(reason) RPC, destroys the controller (the connection then closes).
//   AFightingPlayerController::ClientWasKicked_Implementation @0x38CCB0 (on the joiner): UThePlainesGameInstance::LogoutAndReturnToMainMenu,
//     then BPF_RaiseError with the reason text = the game's own error popup on the title screen.
//   The session: AGameModeBase::GameSession @+0x3F0 (AGameModeBase::Logout reads it). FText::FromString(const FString &) @0x6B4CC0.
//   UIpConnection::LowLevelGetRemoteAddress(bool port) @0x1E710B0 (Steam connections inherit it) = the joiner's address for the row.
#include "kick.h"
#define RVA_GS_KICKPLAYER     0x147BD80u
#define RVA_FTEXT_FROMSTRING  0x6B4CC0u
#define RVA_IPCONN_REMOTEADDR 0x1E710B0u
#define OFF_GM_GAMESESSION    0x3F0u
static const uint8_t KICK_B_KICK[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF0 };
static const uint8_t KICK_B_TEXT[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x63, 0x7A, 0x08 };
static const uint8_t KICK_B_ADDR[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x89, 0xB0, 0x36, 0x03, 0x00 };
static int g_kick_ok = -1, g_kick_n, g_kick_host, g_kick_faults;
static KickRow g_kick_rows[KICK_MAX];
static WeakPtr g_kick_pc[KICK_MAX];
static KickConfirm g_kick_confirm;
static uint64_t g_kick_next;

static int kick_bytes_ok(void) {
    if (g_kick_ok < 0) {
        g_kick_ok = !memcmp(g_base + RVA_GS_KICKPLAYER, KICK_B_KICK, sizeof KICK_B_KICK) && !memcmp(g_base + RVA_FTEXT_FROMSTRING, KICK_B_TEXT, sizeof KICK_B_TEXT)
                    && !memcmp(g_base + RVA_IPCONN_REMOTEADDR, KICK_B_ADDR, sizeof KICK_B_ADDR);
        logf_("kick: engine functions %s", g_kick_ok ? "verified (KickPlayer, FText::FromString, RemoteAddress)" : "DIFFER from this build - Kick player is off");
    }
    return g_kick_ok;
}
// the connection's address, port dropped, last octet masked; "" when the connection is not an IP connection of this build
static void kick_addr(void *conn, char *out, size_t cap) {
    out[0] = 0; if (!conn) return;
    void **vt = *(void ***)conn; int ip = 0;
    for (int i = 0; i < 400 && !ip; i++) ip = vt[i] == (void *)(g_base + RVA_IPCONN_REMOTEADDR);      // the vtable has the IP override = an IP / Steam connection
    if (!ip) return;
    FStringView s = { NULL, 0, 0 };
    ((void (*)(void *, FStringView *, int))(g_base + RVA_IPCONN_REMOTEADDR))(conn, &s, 0);
    char a[64] = ""; if (s.data && s.num > 1 && s.num < 64) narrow(s.data, s.num - 1, a, sizeof a);
    if (s.data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(s.data);
    jg_mask_addr(a, out, cap);
}
// rebuilds the rows from the world's player controllers: every controller with a net connection = a joiner (the host's own has a local player)
static void kick_refresh(void) {
    void *w = world(); g_kick_n = 0;
    g_kick_host = w && ld_hosting();                                                         // landisc.inc: listen server, not a joined client
    if (!g_kick_host || !kick_bytes_ok()) return;
    WeakPtr *list = *(WeakPtr **)((uint8_t *)w + OFF_W_PCLIST); int n = *(int *)((uint8_t *)w + OFF_W_PCLIST + 8);
    if (!list || n <= 0 || n > 64) return;
    void *me = L_local_pc();
    for (int i = 0; i < n && g_kick_n < KICK_MAX; i++) {
        void *pc = api_weak_get(&list[i]); if (!pc || pc == me) continue;
        void *conn = *(void **)((uint8_t *)pc + OFF_PC_PLAYER); if (!conn) continue;
        KickRow *r = &g_kick_rows[g_kick_n]; memset(r, 0, sizeof *r);
        kick_addr(conn, r->addr, sizeof r->addr); if (!r->addr[0]) continue;              // not a remote player (local split screen, a bot)
        void *ps = *(void **)((uint8_t *)pc + OFF_CTRL_PLAYERSTATE); char why[80];
        const FStringView *nm = ps ? (const FStringView *)cd_path(ps, "PlayerName", why, sizeof why) : NULL;
        if (nm && nm->data && nm->num > 1 && nm->num < 64) narrow(nm->data, nm->num - 1, r->name, sizeof r->name);
        api_weak_set(&g_kick_pc[g_kick_n], pc); g_kick_n++;
    }
}
static int kick_count(void) { return g_kick_n; }
static const char *kick_menu_line(int i) {
    static char lab[MENU_LINE_MAX];
    if (i < 0 || i >= g_kick_n) return g_kick_n ? "" : kick_empty_line(g_kick_host);
    kick_label(&g_kick_rows[i], kick_is_armed(&g_kick_confirm, &g_kick_rows[i], GetTickCount64()), lab, sizeof lab); return lab;
}
static void kick_menu_select(int i, uint64_t now) {
    if (g_kick_faults >= 2) return;
    __try {
        kick_refresh();
        if (i < 0 || i >= g_kick_n) { logf_("kick: nothing to kick (%s)", g_kick_host ? "no joiners" : "not hosting"); return; }
        KickRow r = g_kick_rows[i];
        if (!kick_confirm(&g_kick_confirm, &r, now)) { logf_("kick: '%s' (%s) armed - Enter again within %d s kicks", r.name, r.addr, (int)(KICK_CONFIRM_MS / 1000)); return; }
        void *pc = api_weak_get(&g_kick_pc[i]); void *w = world(); void *gm = w ? *(void **)((uint8_t *)w + OFF_W_AUTHGM) : NULL;
        void *gs = gm ? *(void **)((uint8_t *)gm + OFF_GM_GAMESESSION) : NULL;
        if (!pc || !gs) { logf_("kick: '%s' is gone or there is no game session (pc %p, session %p) - nothing done", r.name, pc, gs); return; }
        static wchar_t reason[] = L"The host removed you from the session.";
        FStringView rs = { reason, (int)(sizeof reason / sizeof reason[0]), (int)(sizeof reason / sizeof reason[0]) };
        static uint8_t text[0x18];                                                            // FText (the game keeps it alive while the RPC is sent); one per kick, never freed
        ((void *(*)(void *, const FStringView *))(g_base + RVA_FTEXT_FROMSTRING))(text, &rs);
        {   // our own notice first (the game's ClientWasKicked error popup does not show on the joiner's title screen): same reliable RPC channel as the mod check
            wchar_t m[96]; int mn = swprintf(m, 96, L"LNKICK|%ls", reason) + 1; FStringView msg = { m, mn, mn };
            __try { ((SendClientMsg_t)(g_base + RVA_PC_CLIENTMESSAGE))(pc, &msg, 0, 0.f); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("kick: EXCEPTION 0x%08lX sending the notice (kick goes on)", GetExceptionCode()); }
        }
        uint8_t ok = ((uint8_t (*)(void *, void *, const void *))(g_base + RVA_GS_KICKPLAYER))(gs, pc, text);
        logf_("kick: '%s' from %s -> AGameSession::KickPlayer returned %d (pawn and controller destroyed, the joiner returns to its title screen with the message)", r.name, r.addr, ok);
        char l[1][LM_LINE_MAX]; snprintf(l[0], LM_LINE_MAX, ok ? "Kicked %s (%s)" : "Could not kick %s (%s): see the log", r.name, r.addr); toast_show(l, 1, 6);
        kick_refresh();
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_kick_faults++; logf_("kick: EXCEPTION 0x%08lX (fault %d of 2)%s", GetExceptionCode(), g_kick_faults, g_kick_faults >= 2 ? " - Kick player is off for this run" : ""); }
}
static void kick_tick(uint64_t now) {
    if (!g_menu.open || g_kick_faults >= 2 || now < g_kick_next) return;
    if (g_menu.page != MP_KICK && !(g_menu.page == MP_MAIN)) return;                          // main page: the entry shows the count
    g_kick_next = now + 500;
    __try { kick_refresh(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_kick_faults++; g_kick_n = 0; logf_("kick: EXCEPTION 0x%08lX listing players (fault %d of 2)", GetExceptionCode(), g_kick_faults); }
    g_menu.count[MP_KICK] = g_kick_n ? g_kick_n : 1;
    if (g_menu.page == MP_KICK && g_menu.sel >= g_menu.count[MP_KICK]) g_menu.sel = g_menu.count[MP_KICK] - 1;
}
LN_TICK(0650, kick) { kick_tick(GetTickCount64()); }

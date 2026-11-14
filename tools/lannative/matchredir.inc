// Matchmaking redirect (glue, included by lannative.c before init()). Pure helpers: matchredir.h.
// With config.txt "matchmaking = <ip>" (or "auto" = the address in ip.txt; default off) and only after the -NoEAC gate:
//   1. the exe's own import table entries for getaddrinfo and gethostbyname (ws2_32 ordinal 52) are patched so the two Sloclap service names resolve to a PRIVATE LAN
//      address (a matchmaking backend running on that PC); every other Sloclap / absolver.com name fails to resolve;
//   2. the exe's connect / sendto (ordinals 4 / 20) refuse destinations that are not LAN-like, so the game can not reach Sloclap's real servers while it runs against
//      the backend (fail closed);
//   3. the game's own online-start gate is opened. UThePlainesGameInstance::BPF_ConnectToServers (the title screen's "press Enter") loads the EasyAntiCheatClient module and
//      returns without connecting when that module reports "not active", which is always the case with -NoEAC. One conditional jump is NOPed so the game starts its
//      online connection (to the LAN backend). Nothing of EasyAntiCheat is touched, loaded or hidden.
// Everything is logged. Without the setting nothing here runs.

#ifndef WSAAPI
#define WSAAPI
#endif
typedef int (WSAAPI *MrGetAddrInfo_t)(const char *, const char *, const void *, void **);
typedef struct { char *h_name; char **h_aliases; short h_addrtype; short h_length; char **h_addr_list; } MrHostEnt;
typedef MrHostEnt *(WSAAPI *MrGetHostByName_t)(const char *);
typedef int (WSAAPI *MrConnect_t)(UINT_PTR, const void *, int);
typedef int (WSAAPI *MrSendTo_t)(UINT_PTR, const char *, int, int, const void *, int);

static MrGetAddrInfo_t g_mr_orig_gai;
static MrGetHostByName_t g_mr_orig_ghbn;
static MrConnect_t g_mr_orig_connect;
static MrSendTo_t g_mr_orig_sendto;
static char g_mr_ip[20];
static int g_mr_blocked_logged;

static void mr_log_blocked(const char *what, const unsigned char *sa, int len) {
    if (g_mr_blocked_logged >= 40) return;
    g_mr_blocked_logged++;
    if (sa && len >= 8 && sa[0] == 2) logf_("matchmaking: %s to %u.%u.%u.%u refused (not a LAN address; the game must not reach Sloclap's servers)", what, sa[4], sa[5], sa[6], sa[7]);
    else logf_("matchmaking: %s to a non-LAN address refused", what);
}

static int WSAAPI hk_mr_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
    if (node && mr_name_match(node)) {
        logf_("matchmaking: getaddrinfo(%s) -> %s (LAN backend)", node, g_mr_ip);
        node = g_mr_ip;
    } else if (node && mr_name_block(node)) {
        logf_("matchmaking: getaddrinfo(%s) refused (another Sloclap name; only the redirector and mentoring names go to the LAN backend)", node);
        SetLastError(11001); return 11001;                                                  // WSAHOST_NOT_FOUND / EAI_NONAME
    }
    return g_mr_orig_gai(node, service, hints, res);
}

static MrHostEnt *WSAAPI hk_mr_gethostbyname(const char *name) {
    static __declspec(thread) MrHostEnt he;
    static __declspec(thread) char *addrs[2], *aliases[1];
    static __declspec(thread) unsigned char ip[4];
    static __declspec(thread) char nm[128];
    if (name && mr_name_match(name)) {
        int o[4];
        if (mr_parse_ip(g_mr_ip, o)) {
            for (int i = 0; i < 4; i++) ip[i] = (unsigned char)o[i];
            strncpy(nm, name, sizeof nm - 1); nm[sizeof nm - 1] = 0;
            addrs[0] = (char *)ip; addrs[1] = NULL; aliases[0] = NULL;
            he.h_name = nm; he.h_aliases = aliases; he.h_addrtype = 2; he.h_length = 4; he.h_addr_list = addrs;
            logf_("matchmaking: gethostbyname(%s) -> %s (LAN backend)", name, g_mr_ip);
            return &he;
        }
    } else if (name && mr_name_block(name)) {
        logf_("matchmaking: gethostbyname(%s) refused (another Sloclap name)", name);
        SetLastError(11001); return NULL;
    }
    return g_mr_orig_ghbn(name);
}

static int WSAAPI hk_mr_connect(UINT_PTR s, const void *name, int namelen) {
    if (!mr_sockaddr_ok((const unsigned char *)name, namelen)) { mr_log_blocked("connect", (const unsigned char *)name, namelen); SetLastError(10013); return -1; }   // WSAEACCES
    return g_mr_orig_connect(s, name, namelen);
}

static int WSAAPI hk_mr_sendto(UINT_PTR s, const char *buf, int len, int flags, const void *to, int tolen) {
    if (to && !mr_sockaddr_ok((const unsigned char *)to, tolen)) { mr_log_blocked("sendto", (const unsigned char *)to, tolen); SetLastError(10013); return -1; }
    return g_mr_orig_sendto(s, buf, len, flags, to, tolen);
}

// Replaces one import of the main module (by name, or by ordinal when name is NULL). Returns 1 and the previous function pointer in *orig.
static int mr_iat_patch(HMODULE mod, const char *dll, const char *name, unsigned ordinal, void *hook, void **orig) {
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA64 *oft = (IMAGE_THUNK_DATA64 *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA64 *ft = (IMAGE_THUNK_DATA64 *)(base + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            int hit;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) hit = !name && (unsigned)(oft->u1.Ordinal & 0xFFFF) == ordinal;
            else hit = name && strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData))->Name, name) == 0;
            if (!hit) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            *orig = (void *)(uintptr_t)ft->u1.Function; ft->u1.Function = (ULONGLONG)(uintptr_t)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

// UThePlainesGameInstance::BPF_ConnectToServers (RVA 0x3BDA70): after the EasyAntiCheatClient module's "is active" virtual call, "test al,al; je <return>" at 0x3BDAE1.
// Only patched when the bytes are exactly 84 C0 0F 84 FF 01 00 00 (this game build).
static int mr_open_online_gate(void) {
    static const unsigned char want[8] = { 0x84, 0xC0, 0x0F, 0x84, 0xFF, 0x01, 0x00, 0x00 };
    uint8_t *p = g_base + 0x3BDAE1;
    if (memcmp(p, want, sizeof want) != 0) { logf_("matchmaking: online gate NOT opened (the bytes at BPF_ConnectToServers+0x71 are not the expected ones: other game build?)"); return 0; }
    DWORD old;
    if (!VirtualProtect(p + 2, 6, PAGE_EXECUTE_READWRITE, &old)) { logf_("matchmaking: online gate NOT opened (VirtualProtect failed)"); return 0; }
    memset(p + 2, 0x90, 6);
    VirtualProtect(p + 2, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + 2, 6);
    logf_("matchmaking: online gate opened at base+0x3BDAE3 (the title screen's connect no longer stops because EAC is off)");
    return 1;
}

static void matchredir_install(void) {
    char val[64] = "", line[256] = ""; const char *why = "off"; wchar_t src[MAX_PATH];
    setting_get("matchmaking", L"matchmaking.txt", 0, val, sizeof val, src);
    { wchar_t path[MAX_PATH]; char text[1024] = ""; swprintf(path, MAX_PATH, L"%sip.txt", g_dir);
      if (read_text_file(path, text, sizeof text)) { const char *p = text; const char *b, *e; while (cfg_next_line(&p, &b, &e)) { cfg_trim(&b, &e); if (b == e || *b == '#') continue; cfg_copy(b, e, line, sizeof line); break; } } }
    if (!mr_target(val, line, g_mr_ip, sizeof g_mr_ip, &why)) {
        if (val[0] && _stricmp(val, "off") != 0) logf_("matchmaking: '%s' -> not active (%s); the game talks to Sloclap's servers as usual", val, why);
        else logf_("matchmaking: off (config.txt 'matchmaking = <ip>' or 'auto' redirects the game's matchmaking lookups to a LAN backend)");
        return;
    }
    HMODULE exe = GetModuleHandleW(NULL);
    int a = mr_iat_patch(exe, "WS2_32.dll", "getaddrinfo", 0, (void *)hk_mr_getaddrinfo, (void **)&g_mr_orig_gai);
    int b = mr_iat_patch(exe, "WS2_32.dll", NULL, 52, (void *)hk_mr_gethostbyname, (void **)&g_mr_orig_ghbn);
    int c = mr_iat_patch(exe, "WS2_32.dll", NULL, 4, (void *)hk_mr_connect, (void **)&g_mr_orig_connect);
    int d = mr_iat_patch(exe, "WS2_32.dll", NULL, 20, (void *)hk_mr_sendto, (void **)&g_mr_orig_sendto);
    logf_("matchmaking: ACTIVE, the two Sloclap service names resolve to %s (getaddrinfo hook %s, gethostbyname hook %s). Private LAN addresses only; "
          "start the matchmaking backend on that PC.", g_mr_ip, a ? "installed" : "NOT installed", b ? "installed" : "NOT installed");
    logf_("matchmaking: LAN-only guard: connect %s, sendto %s (non-LAN destinations are refused)", c ? "installed" : "NOT installed", d ? "installed" : "NOT installed");
    if (!a && !b) { logf_("matchmaking: no lookup hook could be installed, the redirect does nothing"); return; }
    if (!c || !d) { logf_("matchmaking: the LAN-only guard is incomplete, so the online gate stays CLOSED (the game could otherwise reach Sloclap's servers)"); return; }
    mr_open_online_gate();
}

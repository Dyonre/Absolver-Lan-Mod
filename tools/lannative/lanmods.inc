// LAN mod check (included by lannative.c after modglue.inc). Logic and text are in lanmods.h (pure code); this file only talks to the engine.
//   joiner -> host : the join URL carries ?LNMods=<fingerprint of the gameplay mods>?LNModL=<names>      (read_join_url)
//   host           : hooks AGameModeBase::PreLogin and AAdversarialGameMode::PreLogin (PvP does not call the base one), compares, warns on screen,
//                    and, once that joiner has a pawn, sends the host's list back with the engine's own APlayerController::ClientMessage
//   joiner         : hooks APlayerController::ClientMessage_Implementation, recognises "LNMODS|..." and shows the same warning (other messages pass through)
// config "modmismatch = warn | block | off" (default warn). block also refuses the login with a message; off sends and checks nothing.
// Nothing here changes game rules, saves or files; it reads a URL option and shows text.
#define IDX_PRELOGIN_BASE 12                    // AGameModeBase::PreLogin(this, FString &options, FString &address, FUniqueNetIdRepl &, FString &error)
#define IDX_PRELOGIN_ADV  13                    // AAdversarialGameMode::PreLogin (same arguments)
#define IDX_CLIENTMSG     14                    // APlayerController::ClientMessage_Implementation(this, FString &, FName, float)
#define RVA_PC_CLIENTMESSAGE 0x18E7A70u         // APlayerController::ClientMessage(this, const FString &, FName, float): generated wrapper that sends the client RPC
#define RVA_FMEMORY_MALLOC   0x684E70u          // FMemory::Malloc(size, alignment)
typedef void (*PreLogin_t)(void *self, void *options, void *address, void *uid, void *err);
typedef void (*ClientMsg_t)(void *pc, void *msg, uint64_t type, float seconds);
typedef void (*SendClientMsg_t)(void *pc, const FStringView *msg, uint64_t type, float seconds);
static PreLogin_t g_orig_prelogin_base, g_orig_prelogin_adv;
static ClientMsg_t g_orig_clientmsg;
static int g_lm_faults;

// ----- on-screen warning (drawn from the PostRender hook; independent of the F1 menu)
static char g_toast[8][LM_LINE_MAX]; static int g_toast_n; static uint64_t g_toast_until;
static void toast_show(char lines[][LM_LINE_MAX], int n, unsigned seconds) {
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { memcpy(g_toast[i], lines[i], LM_LINE_MAX); if (!strstr(lines[i], "JOIN KEY") && !strstr(lines[i], "joinkey =")) logf_("mods: WARNING %s", lines[i]); }   /* the join key is shown on screen only, never logged */
    g_toast_until = GetTickCount64() + (uint64_t)seconds * 1000; g_toast_n = n;
}
static int toast_active(void) { return g_toast_n > 0 && GetTickCount64() < g_toast_until; }
static void toast_draw(void *canvas) {
    if (!toast_active()) return;
    static const uint8_t expect[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x70 };
    if (memcmp(g_base + RVA_CANVAS_DRAWTEXT, expect, sizeof expect) != 0) return;
    void *e = engine(); void *font = e ? *(void **)((uint8_t *)e + OFF_ENGINE_MEDIUMFONT) : NULL; if (!font) return;
    CanvasDrawText_t draw = (CanvasDrawText_t)(g_base + RVA_CANVAS_DRAWTEXT);
    uint8_t *color = (uint8_t *)canvas + OFF_CANVAS_DRAWCOLOR, saved[4]; memcpy(saved, color, 4);
    static const uint8_t black[4] = { 0, 0, 0, 255 }, white[4] = { 255, 255, 255, 255 }, red[4] = { 60, 80, 255, 255 };   // B,G,R,A
    // scale with the screen: UCanvas::ClipY is at +0x34 (ClipX at +0x30); 1080p = 1.0, 4K = about 2.2; fall back to 1.0 if the value looks wrong
    float clip_y = *(float *)((uint8_t *)canvas + 0x34), sc = 1.f, y = 120.f;
    if (clip_y >= 400.f && clip_y <= 10000.f) { sc = clip_y / 1080.f; if (sc < 1.f) sc = 1.f; if (sc > 3.f) sc = 3.f; y = clip_y * 0.12f; }   // below the health bar
    for (int i = 0; i < g_toast_n; i++, y += 24.f * sc) {
        wchar_t wide[LM_LINE_MAX]; int n = 0; while (g_toast[i][n] && n < LM_LINE_MAX - 1) { wide[n] = (wchar_t)(unsigned char)g_toast[i][n]; n++; } wide[n] = 0;
        FStringView text = { wide, n + 1, n + 1 }; uint8_t info[64]; memset(info, 0, sizeof info);
        memcpy(color, black, 4); draw(canvas, font, &text, 60.f * sc + 2.f, y + 2.f, sc, sc, info);
        memcpy(color, i == 0 ? red : white, 4); draw(canvas, font, &text, 60.f * sc, y, sc, sc, info);
    }
    memcpy(color, saved, 4);
}

static int read_modmismatch(void) {
    wchar_t path[MAX_PATH]; char value[32] = "";
    if (!setting_get("modmismatch", L"modmismatch.txt", 0, value, sizeof value, path)) { logf_("mod check: warn (default; not set in %ls)", path); return 1; }
    if (_strnicmp(value, "off", 3) == 0) { logf_("mod check: OFF (%ls) - no mod information is sent or compared", path); return 0; }
    if (_strnicmp(value, "block", 5) == 0) { logf_("mod check: BLOCK (%ls) - a joiner whose gameplay mods differ is refused", path); return 2; }
    logf_("mod check: warn (%ls)", path); return 1;
}
static void widen(const char *s, wchar_t *w, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) w[i] = (wchar_t)(unsigned char)s[i]; w[i] = 0; }
static void narrow(const wchar_t *w, int len, char *s, int cap) { utw_to_8(w, len, s, cap); }      // UTF-8; control characters become '?'
static int widen_u8(const char *s, wchar_t *w, int cap, int *truncated) { return ut8_to_w(s, w, cap, truncated); }   // UTF-8 value text (rows.json); the byte-wise widen() above is for ASCII names

// ----- host side
static struct { int valid; LmStatus st; uint64_t at; } g_lm_pending;       // a joiner passed PreLogin with different mods and has not been told yet
static void lm_refuse(void *err, const char *text) {
    FStringView *e = err; if (!e || e->num >= 2) return;                      // keep an error the game already set
    wchar_t w[200]; widen(text, w, 200); int n = (int)wcslen(w) + 1;
    wchar_t *buf = ((void *(*)(size_t, unsigned))(g_base + RVA_FMEMORY_MALLOC))((size_t)n * sizeof(wchar_t), 0);
    if (!buf) return;
    memcpy(buf, w, (size_t)n * sizeof(wchar_t));
    if (e->data) ((void (*)(void *))(g_base + RVA_FMEMORY_FREE))(e->data);
    e->data = buf; e->num = e->max = n;
}
// ----- join protection (joinguard.h): decides before the mod check, whatever modmismatch says
static char g_jg_keyfile_note[40];
static int jg_rnd(unsigned *v) { return rand_s(v); }
static int jg_read_first_line(const wchar_t *path, char *out, size_t cap) {
    char t[256]; if (!read_text_file(path, t, sizeof t)) return 0;
    const char *p = cfg_skip_bom(t), *b, *e; if (!cfg_next_line(&p, &b, &e)) return 0; cfg_trim(&b, &e); cfg_copy(b, e, out, cap); return 1;
}
static void jg_config_load(void) {
    char v[96] = ""; wchar_t path[MAX_PATH], keyfile[MAX_PATH];
    memset(&g_jg, 0, sizeof g_jg); g_jg.need_key = 1; g_jg.maxpeers = 4;
    if (setting_get("joinprotect", L"joinprotect.txt", 0, v, sizeof v, path) && !_strnicmp(v, "open", 4)) g_jg.need_key = 0;
    swprintf(keyfile, MAX_PATH, L"%sjoinkey.txt", g_dir);
    int have = 0;
    if (g_cfg_present && cfg_get(g_cfg_text, "joinkey", 0, v, sizeof v)) {
        if (jg_key_valid(v)) { strcpy(g_jg.key, v); have = 1; snprintf(g_jg_keyfile_note, sizeof g_jg_keyfile_note, "config.txt"); }
        else logf_("join protection: joinkey in config.txt must be 4-32 characters A-Z a-z 0-9 - _ ; ignored");
    }
    if (!have && jg_read_first_line(keyfile, v, sizeof v) && jg_key_valid(v)) { strcpy(g_jg.key, v); have = 1; snprintf(g_jg_keyfile_note, sizeof g_jg_keyfile_note, "joinkey.txt"); }
    if (!have) {
        jg_make_key(g_jg.key, jg_rnd);
        FILE *f = _wfopen(keyfile, L"wb"); if (f) { fprintf(f, "%s\r\n", g_jg.key); fclose(f); }
        snprintf(g_jg_keyfile_note, sizeof g_jg_keyfile_note, "joinkey.txt (new)");
    }
    for (int k = 0; g_jg.nallow < JG_ALLOW_MAX && setting_get("allow", L"allow.txt", k, v, sizeof v, path); k++) {
        if (jg_parse_rule(v, &g_jg.allow[g_jg.nallow])) g_jg.nallow++; else logf_("join protection: allow rule '%.40s' is not a.b.c.d or a.b.c.d/bits - ignored", v);
    }
    if (setting_get("maxpeers", L"maxpeers.txt", 0, v, sizeof v, path)) {
        if (!_strnicmp(v, "off", 3) || !_strnicmp(v, "unlimited", 9)) g_jg.maxpeers = 0;
        else { int n = atoi(v); if (n >= 0 && n <= 64) g_jg.maxpeers = n; else logf_("join protection: maxpeers must be 0-64 or off; using 4"); }
    }
    logf_("join protection: %s; %d allow rule(s)%s; at most %d joiner(s) (key source: %s - the key itself is never logged)", g_jg.need_key ? "a join key is required" : "OPEN (no key)", g_jg.nallow,
          g_jg.nallow ? "" : " (any address)", g_jg.maxpeers, g_jg_keyfile_note);
}
// 1 = refuse this login (the error text is already set)
static int jg_on_prelogin(const char *which, void *options, void *address, void *err) {
    const FStringView *o = options, *a = address;
    int olen = (o && o->data && o->num > 1 && o->num < 4000) ? o->num - 1 : 0;
    char addr[80] = "?"; if (a && a->data && a->num > 1 && a->num < 200) narrow(a->data, a->num - 1, addr, sizeof addr);
    JgVerdict v = jg_check(&g_jg, o && o->data ? o->data : L"", olen, addr, T_peers_now(), GetTickCount64());
    char masked[40]; jg_mask_addr(addr, masked, sizeof masked);
    if (v == JG_OK) { logf_("join protection (%s): login from %s accepted", which, masked); return 0; }
    logf_("join protection (%s): login from %s REFUSED: %s", which, masked, jg_verdict_name(v));
    lm_refuse(err, jg_refusal_text(v));
    { char lines[2][LM_LINE_MAX]; snprintf(lines[0], LM_LINE_MAX, "JOIN REFUSED (%s) from %s", jg_verdict_name(v), masked); lines[1][0] = 0; toast_show(lines, 1, 8); }
    return 1;
}
// shown on the host's screen when a listen server starts: the key to give to friends
static void jg_announce_host(void) {
    if (!g_jg.need_key) { char l1[1][LM_LINE_MAX]; snprintf(l1[0], LM_LINE_MAX, "HOSTING WITHOUT A JOIN KEY (joinprotect = open)"); toast_show(l1, 1, 15); return; }
    char lines[3][LM_LINE_MAX];
    snprintf(lines[0], LM_LINE_MAX, "JOIN KEY: %s", g_jg.key);
    snprintf(lines[1], LM_LINE_MAX, "Friends put  joinkey = %s  in LanNative\\config.txt", g_jg.key);
    snprintf(lines[2], LM_LINE_MAX, "(keep it private; change it by editing joinkey.txt)");
    toast_show(lines, 3, 40);
}
static void lm_on_prelogin(const char *which, void *options, void *address, void *err) {
    if (!g_lm_mode || !g_lm_me.present) return;
    const FStringView *o = options, *a = address; LmInfo c; memset(&c, 0, sizeof c);
    int olen = (o && o->data && o->num > 1 && o->num < 4000) ? o->num - 1 : 0;
    if (olen) lm_parse_options(o->data, olen, &c);
    char addr[80] = "?"; if (a && a->data && a->num > 1 && a->num < 200) narrow(a->data, a->num - 1, addr, sizeof addr);
    LmStatus st = lm_compare(&g_lm_me, &c);
    char maddr[40]; jg_mask_addr(addr, maddr, sizeof maddr);
    logf_("mods: join check (%s) from %s: joiner %s fp %016llX '%s', host fp %016llX '%s' -> %s", which, maddr, c.present ? "sent" : "sent no mod info",
          (unsigned long long)c.fp, c.names, (unsigned long long)g_lm_me.fp, g_lm_me.names, st == LM_SAME ? "same" : st == LM_DIFFERENT ? "DIFFERENT" : "UNKNOWN");
    if (st == LM_SAME) return;
    char lines[8][LM_LINE_MAX]; int n = lm_lines(st, &g_lm_me, "Host (you)", &c, "Joiner", lines, 8);
    toast_show(lines, n, 20);
    g_lm_pending.valid = 1; g_lm_pending.st = st; g_lm_pending.at = GetTickCount64();
    if (g_lm_mode == 2) {
        char text[160]; snprintf(text, sizeof text, "Mod mismatch: the host plays with %s. Install the same mods, or ask the host to set modmismatch = warn.", g_lm_me.names[0] ? g_lm_me.names : "no gameplay mods");
        lm_refuse(err, text); logf_("mods: BLOCK: the login from %s was refused", maddr); g_lm_pending.valid = 0;
    }
}
static void jg_guarded(const char *which, void *o, void *a, void *e, int *refused) {
    *refused = 0;
    __try { *refused = jg_on_prelogin(which, o, a, e); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *refused = 1; lm_refuse(e, "Join refused: internal error in the host's join protection."); logf_("join protection: EXCEPTION 0x%08lX - the login was refused (fail closed)", GetExceptionCode()); }
}
static void hk_prelogin_base(void *self, void *o, void *a, void *u, void *e) {
    g_orig_prelogin_base(self, o, a, u, e);
    int refused; jg_guarded("game mode", o, a, e, &refused); if (refused) return;
    if (!g_disabled && g_lm_faults < 3) { __try { lm_on_prelogin("game mode", o, a, e); } __except (EXCEPTION_EXECUTE_HANDLER) { g_lm_faults++; logf_("mods: EXCEPTION 0x%08lX in the join check (fault %d of 3)", GetExceptionCode(), g_lm_faults); } }
}
static void hk_prelogin_adv(void *self, void *o, void *a, void *u, void *e) {
    g_orig_prelogin_adv(self, o, a, u, e);
    int refused; jg_guarded("pvp game mode", o, a, e, &refused); if (refused) return;
    if (!g_disabled && g_lm_faults < 3) { __try { lm_on_prelogin("pvp game mode", o, a, e); } __except (EXCEPTION_EXECUTE_HANDLER) { g_lm_faults++; logf_("mods: EXCEPTION 0x%08lX in the join check (fault %d of 3)", GetExceptionCode(), g_lm_faults); } }
}
// called from hk_serverack (the joiner's controller just acknowledged its pawn): tell that joiner what the host runs
static void lm_serverack_post(void *pc) {
    if (!g_lm_pending.valid || !g_lm_mode || g_lm_faults >= 3) return;
    g_lm_pending.valid = 0;
    if (GetTickCount64() - g_lm_pending.at > 180000) return;                    // stale: that joiner is long gone
    char packed[LM_NAMES_MAX + 40]; wchar_t w[LM_NAMES_MAX + 40];
    if (!lm_pack_msg(&g_lm_me, packed, sizeof packed)) return;
    widen(packed, w, LM_NAMES_MAX + 40); FStringView msg = { w, (int)wcslen(w) + 1, (int)wcslen(w) + 1 };
    __try { ((SendClientMsg_t)(g_base + RVA_PC_CLIENTMESSAGE))(pc, &msg, 0, 0.f); logf_("mods: sent the host's mod list to the joiner (%s)", packed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_lm_faults++; logf_("mods: EXCEPTION 0x%08lX while sending the mod list (fault %d of 3)", GetExceptionCode(), g_lm_faults); }
}

// ----- joiner side
static void hk_clientmsg(void *pc, void *msg, uint64_t type, float seconds) {
    if (!g_disabled && g_lm_mode && g_lm_faults < 3) {
        __try {
            const FStringView *m = msg; LmInfo host;
            if (m && m->data && m->num > 24 && m->num < 600 && lm_unpack_msg(m->data, m->num - 1, &host)) {
                LmStatus st = lm_compare(&g_lm_me, &host);
                logf_("mods: the host reported fp %016llX '%s'; mine is %016llX '%s' -> %s", (unsigned long long)host.fp, host.names, (unsigned long long)g_lm_me.fp, g_lm_me.names,
                      st == LM_SAME ? "same" : "DIFFERENT");
                char lines[8][LM_LINE_MAX]; int n = lm_lines(st, &g_lm_me, "You", &host, "Host", lines, 8);
                if (n) toast_show(lines, n, 25);
                return;                                                           // our own message: do not pass it to the game's chat/HUD
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_lm_faults++; logf_("mods: EXCEPTION 0x%08lX reading a client message (fault %d of 3)", GetExceptionCode(), g_lm_faults); }
    }
    g_orig_clientmsg(pc, msg, type, seconds);
}
// ----- self-test (F11): runs the REAL engine functions with crafted input so the whole check can be tried on one PC (no second player needed).
// LanNative\lmtest.txt, one command per line:   join <URL options>  -> AGameModeBase::PreLogin on this world's game mode (host side, needs a hosted or single-player world)
//                                               joinfrom <ip> <URL options> -> the same PreLogin with <ip>:0 as the peer address
//                                               url                 -> logs the join URL menu Join would send
//                                               msg <text>          -> APlayerController::ClientMessage on this PC's own controller (joiner side, shows the warning)
//                                               tbl, rowget, rowset, rowapply, rowprobe, objrt, objdump, rowdump, rowfull, rowprops, objget, objset -> rows_command
//                                               (rowedit.inc); rowset / objset change live game memory, rowdump / rowfull write files
// No file = F11 does nothing. Nothing is sent over the network.
static LONG lm_report_fault(EXCEPTION_POINTERS *ep, const char *what) {
    uint64_t a = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, base = (uint64_t)(uintptr_t)g_base;
    logf_("mod check self-test: %s faulted: code 0x%08lX at %s+0x%llX, params %llX %llX", what, ep->ExceptionRecord->ExceptionCode, (a >= base && a < base + g_exe_size) ? "exe" : "elsewhere",
          (unsigned long long)(a - base), (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0], (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void rows_command(const char *line);   /* rowedit.inc */
static void lm_selftest(void) {
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%slmtest.txt", g_dir);
    FILE *f = _wfopen(path, L"r"); if (!f) { logf_("mod check self-test: %ls not found", path); return; }
    char line[600];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        wchar_t w[600]; wchar_t addr[48] = L"127.0.0.1:0 (self-test)";
        char fakeaddr[40] = ""; const char *joinopts = NULL;
        if (!strncmp(line, "joinfrom ", 9)) { char ip[40]; if (sscanf(line + 9, "%39s", ip) == 1) { snprintf(fakeaddr, sizeof fakeaddr, "%s:0", ip); const char *sp = strchr(line + 9, ' '); joinopts = sp ? sp + 1 : ""; } }
        if (!strncmp(line, "join ", 5) || joinopts) {
            void *wld = world(); void *gm = wld ? *(void **)((uint8_t *)wld + OFF_W_AUTHGM) : NULL;
            if (!gm) { logf_("mod check self-test: no game mode in this world (host or single-player only)"); continue; }
            if (joinopts) widen(fakeaddr, addr, 48); widen(joinopts ? joinopts : line + 5, w, 600); FStringView opt = { w, (int)wcslen(w) + 1, (int)wcslen(w) + 1 }, a = { addr, (int)wcslen(addr) + 1, (int)wcslen(addr) + 1 }, err = { NULL, 0, 0 };                    /* empty and NULL: the engine frees/replaces this buffer, so it must never point at stack memory */
            logf_("mod check self-test: PreLogin with options '%s'", joinopts ? joinopts : line + 5);
            uint8_t uid[32] = { 0 };                                                            /* an empty FUniqueNetIdRepl, like a LAN client without online identity */
            __try { ((PreLogin_t)(g_base + 0x147F520u))(gm, &opt, &a, uid, &err); }       // goes through our hook exactly like a real join
            __except (lm_report_fault(GetExceptionInformation(), "PreLogin")) { }
            logf_("mod check self-test: PreLogin returned, error message length %d", err.num > 0 ? err.num - 1 : 0);
        } else if (!strncmp(line, "tbl ", 4) || !strncmp(line, "rowget ", 7) || !strncmp(line, "rowset ", 7) || !strncmp(line, "rowapply ", 9) || !strncmp(line, "rowprobe ", 9) || !strncmp(line, "objrt ", 6) || !strncmp(line, "objdump ", 8) || !strncmp(line, "rowdump ", 8) || !strncmp(line, "rowfull ", 8) || !strncmp(line, "rowprops ", 9) || !strncmp(line, "objget ", 7) || !strncmp(line, "objset ", 7)) {
            __try { rows_command(line); } __except (lm_report_fault(GetExceptionInformation(), "row command")) { }
        } else if (!strcmp(line, "url")) {                       // what F7 would send, without joining anyone
            wchar_t u[900]; if (read_join_url(u, 900)) logf_("mod check self-test: the join URL would be %ls", u);
        } else if (!strncmp(line, "msg ", 4)) {
            void *pc = L_local_pc(); if (!pc) { logf_("mod check self-test: no local player controller"); continue; }
            widen(line + 4, w, 600); FStringView m = { w, (int)wcslen(w) + 1, (int)wcslen(w) + 1 };
            logf_("mod check self-test: ClientMessage '%s'", line + 4);
            __try { ((SendClientMsg_t)(g_base + RVA_PC_CLIENTMESSAGE))(pc, &m, 0, 0.f); }
            __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mod check self-test: EXCEPTION 0x%08lX in ClientMessage", GetExceptionCode()); }
        }
    }
    fclose(f);
}

static void lanmods_install(void) {
    g_lm_mode = read_modmismatch();
    jg_config_load();
    g_orig_prelogin_base = (PreLogin_t)hook(IDX_PRELOGIN_BASE, (void *)hk_prelogin_base);       // the join protection needs these even when the mod check is off
    g_orig_prelogin_adv = (PreLogin_t)hook(IDX_PRELOGIN_ADV, (void *)hk_prelogin_adv);
    if (!g_orig_prelogin_base || !g_orig_prelogin_adv) logf_("join protection: PreLogin hook NOT installed (%s) - anybody who can reach this PC can join while you host", g_orig_prelogin_base ? "pvp game mode" : "game mode");
    if (!g_lm_mode) return;
    g_orig_clientmsg = (ClientMsg_t)hook(IDX_CLIENTMSG, (void *)hk_clientmsg);
    logf_("mod check: host check %s, joiner display %s", g_orig_prelogin_base && g_orig_prelogin_adv ? "installed" : "PARTLY MISSING", g_orig_clientmsg ? "installed" : "NOT installed");
}

// the joiner's key for the URL: same setting as the host's key
static int jg_key_for_url(wchar_t *out, size_t cap) { return jg_join_suffix(g_jg.key, out, cap); }

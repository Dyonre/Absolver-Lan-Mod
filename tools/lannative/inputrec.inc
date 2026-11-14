// The input + state RECORDER. Loader feature `input_rec`, default OFF: with it off NOTHING is hooked or written.
// Included by content.inc after defense_ev.inc (uses api_p_on_defense); init from content_init: ir_init(a). Pure encoder / decoder: inputrec.h.
//
// PERSONAL DATA: a recording is the player's own play. It is written ONLY to a subfolder of the loader folder on this PC (input_rec_dir, default
// recordings\), every file starts with a header line that says so; recordings/ and *.jsonl are gitignored. Nothing here uploads or sends anything.
//
// What is recorded (schema in inputrec.h):
//   key   / axis  : hooks on APlayerController::InputKey / InputAxis (engine, 0x1671010 / 0x1670F60), local player controller only. AFightingPlayerController::InputKey forwards
//                   there in every state except one (controller state +0x1348 == 0x15 with a UI action process: the key goes to a widget and is not recorded).
//                   The FIGHTING controller's own functions are NOT hooked on purpose: their prologue bytes stay stock for code that verifies them.
//   obs           : sampled on the game tick at input_rec_hz (default 30), written on change / every 1 s: player + opponent (target, else the last character in a hit with
//                   the player) health, guard, stance, style, attacking, guarding, down, shards, attack frame; distance, lock, stick / WASD direction, fight / round ids.
//   attack / hit / def : the extender events (deduplicated attack starts, resolved hits, defence classification). Parried on_hit events have victim / attacker swapped:
//                   the "def" line has them the right way round.
//   layer         : UAvailabilityLayerNotifyState::NotifyOutside for player / opponent: layer data name on push / pop (PerfectAttackOnly = the gold-link window).
//   pose / layer  : obs also carries x, y, yaw per side ("pp" / "po") and the layer on top per side ("pl" / "ol" = last pushed, not yet popped).
// Cost: QueryPerformanceCounter around every hook body and the sampler, logged every 30 s ("inputrec: cost ...") next to the loader's perf: lines.
// Writes: 64 KB buffer, flushed when 48 KB full, every 2 s, at world change and at exit. Per-file cap input_rec_file_mb (default 64) -> a new part file;
// folder budget input_rec_dir_mb (default 2048): when full the recorder STOPS (it never deletes recordings).
// config.txt: input_rec = on|1 (default off), input_rec_dir (recordings), input_rec_hz (30), input_rec_file_mb (64), input_rec_dir_mb (2048),
//             bot_reads_inputs = on (default OFF; development only, never in a package).
#include "inputrec.h"

static const AbsApi *IR_API;
static int g_ir_on, g_ir_state, g_ir_hz = 30, g_ir_bot_reads;           // state: 0 idle (no file yet), 1 writing, -1 off / failed, -2 stopped (folder budget)
static long long g_ir_file_cap, g_ir_dir_cap, g_ir_dir_bytes, g_ir_file_bytes;
static HANDLE g_ir_h = INVALID_HANDLE_VALUE; static int g_ir_part; static char g_ir_stamp[32], g_ir_char[64], g_ir_char_raw[96];
static IrBuf g_ir_buf; static CRITICAL_SECTION g_ir_cs; static double g_ir_t0, g_ir_last_flush, g_ir_last_sample, g_ir_last_obs, g_ir_last_cost, g_ir_last_round_read;
static IrObs g_ir_prev; static int g_ir_have_prev, g_ir_fight, g_ir_round; static void *g_ir_foe, *g_ir_last_foe_seen; static unsigned g_ir_gen;
static double g_ir_cost_s, g_ir_hook_s; static long long g_ir_frames, g_ir_lines, g_ir_samples;
typedef char (*IrKey_t)(void *, void *, int, float, char);
typedef char (*IrAxis_t)(void *, void *, float, float, int, char);
typedef void (*IrNotify_t)(void *, void *, void *, uint8_t *);
static IrKey_t o_ir_key; static IrAxis_t o_ir_axis; static IrNotify_t o_ir_notify;
typedef struct { FNameRaw k; char name[40]; float sum, last, val; int rel, used; } IrAx;
static IrAx g_ir_ax[16]; static int g_ir_wasd[4];                      // W A S D held
typedef struct { void *ch; char row[32]; double t; } IrAtk;
static IrAtk g_ir_atk[2];                                               // 0 player, 1 opponent: last attack start
typedef struct { char name[64]; int on; } IrLayer;
static IrLayer g_ir_layer[2];

static double ir_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
static double ir_t(void) { return ir_now() - g_ir_t0; }
static const char *ir_who(void *ch) { void *p = cd_local_pawn(); return ch && ch == p ? "p" : ch && ch == g_ir_foe ? "o" : "n"; }

// ----- file
static char g_ir_sub[64] = "recordings";                               // input_rec_dir: subfolder of the loader folder
static void ir_dir(wchar_t *out) { wchar_t w[64]; MultiByteToWideChar(CP_UTF8, 0, g_ir_sub, -1, w, 64); swprintf(out, MAX_PATH, L"%s%ls", g_dir, w); }
static long long ir_dir_size(void) {
    wchar_t d[MAX_PATH], pat[MAX_PATH]; ir_dir(d); swprintf(pat, MAX_PATH, L"%ls\\*.jsonl", d); long long s = 0; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0; do { s += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; } while (FindNextFileW(h, &fd)); FindClose(h); return s;
}
static void ir_write_raw(const char *p, int n) {                        // caller holds g_ir_cs
    if (g_ir_h == INVALID_HANDLE_VALUE || n <= 0) return; DWORD w = 0;
    if (!WriteFile(g_ir_h, p, (DWORD)n, &w, NULL) || (int)w != n) { logf_("inputrec: WRITE FAILED (error %lu) - recorder off", GetLastError()); CloseHandle(g_ir_h); g_ir_h = INVALID_HANDLE_VALUE; g_ir_state = -1; return; }
    g_ir_file_bytes += n; g_ir_dir_bytes += n;
}
static int ir_open_part(void);
static void ir_flush(void) {                                            // caller holds g_ir_cs
    if (!g_ir_buf.n) return;
    if (ir_dir_full(g_ir_dir_bytes + g_ir_buf.n, g_ir_dir_cap)) {
        logf_("inputrec: recordings folder budget %lld MB reached - recording STOPPED (nothing deleted; move old recordings away to continue)", g_ir_dir_cap >> 20);
        g_ir_buf.n = 0; if (g_ir_h != INVALID_HANDLE_VALUE) CloseHandle(g_ir_h); g_ir_h = INVALID_HANDLE_VALUE; g_ir_state = -2; return;
    }
    if (ir_should_rotate(g_ir_file_bytes, g_ir_buf.n, g_ir_file_cap)) { CloseHandle(g_ir_h); g_ir_h = INVALID_HANDLE_VALUE; g_ir_part++; if (!ir_open_part()) { g_ir_buf.n = 0; return; } }
    ir_write_raw(g_ir_buf.b, g_ir_buf.n); g_ir_buf.n = 0; g_ir_last_flush = ir_now();
}
static void ir_put(const char *line, int len) {                         // append one encoded line (any thread)
    if (g_ir_state != 1) return;
    EnterCriticalSection(&g_ir_cs);
    if (!ir_buf_put(&g_ir_buf, line, len) && len >= 0) { ir_flush(); if (g_ir_state == 1 && !ir_buf_put(&g_ir_buf, line, len)) g_ir_buf.dropped++; }
    g_ir_lines++; LeaveCriticalSection(&g_ir_cs);
}
static int ir_open_part(void) {                                         // caller holds g_ir_cs (or is single-threaded at first open)
    wchar_t d[MAX_PATH], p[MAX_PATH], w[160]; char fn[160]; ir_dir(d); CreateDirectoryW(d, NULL);
    ir_file_name(g_ir_stamp, g_ir_char, g_ir_part, fn, sizeof fn); MultiByteToWideChar(CP_UTF8, 0, fn, -1, w, 160); swprintf(p, MAX_PATH, L"%ls\\%ls", d, w);
    g_ir_h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_ir_h == INVALID_HANDLE_VALUE) { logf_("inputrec: cannot create %ls (error %lu) - recorder off", p, GetLastError()); g_ir_state = -1; return 0; }
    g_ir_file_bytes = 0;
    char pkg[96] = ""; { wchar_t pp[MAX_PATH]; swprintf(pp, MAX_PATH, L"%sPACKAGE.txt", g_dir); char t[200]; if (read_text_file(pp, t, sizeof t)) { int i = 0; while (t[i] && t[i] != '\r' && t[i] != '\n' && i < 95) { pkg[i] = t[i]; i++; } pkg[i] = 0; } }
    SYSTEMTIME st; GetLocalTime(&st); char started[40]; snprintf(started, sizeof started, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char h[1024]; int n = ir_enc_header(h, sizeof h, "lannative " __DATE__ " " __TIME__, pkg, started, g_ir_char_raw, g_ir_hz, g_ir_bot_reads); ir_write_raw(h, n + 1);
    if (g_ir_part > 1) { n = ir_enc_event(h, sizeof h, "note", ir_t(), NULL, NULL, NULL, g_ir_part, 0, 0, "continued after the per-file size cap"); ir_write_raw(h, n + 1); }
    logf_("inputrec: recording to %ls (part %d; personal data, local only)", p, g_ir_part); return g_ir_state != -1;
}
static void ir_start_file(void) {                                       // game thread, samples with a local character: waits (max 30 s) for the save's character name
    static double first; char id[48]; sc_char_ids(id, sizeof id, g_ir_char_raw, sizeof g_ir_char_raw);
    if (!first) first = ir_now(); if (!g_ir_char_raw[0] && ir_now() - first < 30.0) return;     // title-screen pawn: no live save yet
    ir_safe_name(g_ir_char_raw, g_ir_char, sizeof g_ir_char);
    SYSTEMTIME st; GetLocalTime(&st); snprintf(g_ir_stamp, sizeof g_ir_stamp, "%04d-%02d-%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_ir_dir_bytes = ir_dir_size(); g_ir_part = 1;
    if (ir_dir_full(g_ir_dir_bytes, g_ir_dir_cap)) { logf_("inputrec: recordings folder already holds %lld MB (budget %lld MB) - not recording", g_ir_dir_bytes >> 20, g_ir_dir_cap >> 20); g_ir_state = -2; return; }
    EnterCriticalSection(&g_ir_cs); g_ir_state = 1; if (!ir_open_part()) g_ir_state = -1; LeaveCriticalSection(&g_ir_cs);
}
static void ir_exit(void) {
    if (g_ir_state != 1 && g_ir_h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_ir_cs); ir_flush(); if (g_ir_h != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_ir_h); CloseHandle(g_ir_h); g_ir_h = INVALID_HANDLE_VALUE; } LeaveCriticalSection(&g_ir_cs);
    logf_("inputrec: closed (%lld lines, %lld dropped, %lld bytes in the last part)", g_ir_lines, g_ir_buf.dropped, g_ir_file_bytes);
}

// ----- hooks
static char hk_ir_key(void *pc, void *key, int ev, float amt, char gp) {
    double c0 = ir_now();
    __try {
        if (g_ir_state >= 0 && key && pc == L_local_pc()) {
            char nm[48]; fname_text(*(FNameRaw *)key, nm, sizeof nm);
            static const char wasd[4][2] = { "W", "A", "S", "D" }; for (int i = 0; i < 4; i++) if (!strcmp(nm, wasd[i]) && (ev == 0 || ev == 1)) g_ir_wasd[i] = ev == 0;
            if (g_ir_state == 1) { char b[160]; ir_put(b, ir_enc_key(b, sizeof b, ir_t(), nm, ev, gp)); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_ir_hook_s += ir_now() - c0;
    return o_ir_key(pc, key, ev, amt, gp);
}
static char hk_ir_axis(void *pc, void *key, float delta, float dt, int ns, char gp) {
    double c0 = ir_now();
    __try {
        if (g_ir_state >= 0 && key && pc == L_local_pc()) {
            FNameRaw k = *(FNameRaw *)key; int i, free_ = -1;
            for (i = 0; i < 16; i++) { if (g_ir_ax[i].used && g_ir_ax[i].k.index == k.index && g_ir_ax[i].k.number == k.number) break; if (!g_ir_ax[i].used && free_ < 0) free_ = i; }
            if (i == 16 && free_ >= 0) { i = free_; memset(&g_ir_ax[i], 0, sizeof g_ir_ax[i]); g_ir_ax[i].k = k; fname_text(k, g_ir_ax[i].name, sizeof g_ir_ax[i].name); g_ir_ax[i].rel = ir_axis_relative(g_ir_ax[i].name); g_ir_ax[i].used = 1; }
            if (i < 16) { if (g_ir_ax[i].rel) g_ir_ax[i].sum += delta; else g_ir_ax[i].val = delta; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_ir_hook_s += ir_now() - c0;
    return o_ir_axis(pc, key, delta, dt, ns, gp);
}
static void hk_ir_notify(void *nt, void *mesh, void *seq, uint8_t *info) {
    o_ir_notify(nt, mesh, seq, info);
    if (g_ir_state != 1) return;
    __try {
        if (!se_valid_ptr(mesh) || !se_valid_ptr(info) || !se_valid_ptr(nt)) return;
        void *ch = *(void **)((uint8_t *)mesh + 0xE0); int s = ch && ch == cd_local_pawn() ? 0 : ch && ch == g_ir_foe ? 1 : -1; if (s < 0) return;
        void *ld = *(void **)((uint8_t *)nt + 0x38); char nm[64] = ""; se_name_of(ld, nm, sizeof nm); int on = info[0x14] != 0;
        if (!nm[0] || (g_ir_layer[s].on == on && !strcmp(g_ir_layer[s].name, nm))) return;
        snprintf(g_ir_layer[s].name, sizeof g_ir_layer[s].name, "%s", nm); g_ir_layer[s].on = on;
        char b[256]; ir_put(b, ir_enc_event(b, sizeof b, "layer", ir_t(), s ? "o" : "p", NULL, NULL, on, info[0x29], 0, nm));   // b = prediction flag (+0x29)
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ----- events
static void ir_on_attack(void *ch, int id, const char *attack, void *u) {
    (void)id; (void)u; if (g_ir_state != 1) return;
    __try {
        const char *w = ir_who(ch); int s = w[0] == 'p' ? 0 : w[0] == 'o' ? 1 : -1;
        if (s >= 0) { g_ir_atk[s].ch = ch; snprintf(g_ir_atk[s].row, sizeof g_ir_atk[s].row, "%s", attack ? attack : ""); g_ir_atk[s].t = ir_t(); }
        char b[160]; ir_put(b, ir_enc_event(b, sizeof b, "attack", ir_t(), w, NULL, attack ? attack : "", 0, 0, 0, NULL));
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void ir_on_hit(const AbsHitInfo *h, void *u) {
    (void)u; if (g_ir_state < 0 || !h) return;
    __try {
        void *p = cd_local_pawn(); if (p && h->victim == p && h->attacker && h->attacker != p) g_ir_last_foe_seen = h->attacker; else if (p && h->attacker == p && h->victim && h->victim != p) g_ir_last_foe_seen = h->victim;
        if (g_ir_state == 1) { char b[200]; ir_put(b, ir_enc_event(b, sizeof b, "hit", ir_t(), ir_who(h->attacker), ir_who(h->victim), h->attack, h->outcome, h->perfect_link, h->damage, NULL)); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void ir_on_defense(const AbsDefenseInfo *d, void *u) {
    (void)u; if (g_ir_state != 1 || !d) return;
    __try { char b[200]; ir_put(b, ir_enc_event(b, sizeof b, "def", ir_t(), ir_who(d->defender), ir_who(d->attacker), d->attack, d->type, d->avoid_type, d->frame_in_attack, NULL)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void ir_on_destroyed(void *ch, int reason, void *u) { (void)reason; (void)u; if (ch == g_ir_foe) g_ir_foe = NULL; if (ch == g_ir_last_foe_seen) g_ir_last_foe_seen = NULL; for (int i = 0; i < 2; i++) if (g_ir_atk[i].ch == ch) g_ir_atk[i].ch = NULL; }

// ----- sampler
static void ir_side(void *ch, int s, IrSide *o) {
    for (int i = 0; i < IR_SIDE_N; i++) o->v[i] = -1; if (!ch) return;
    AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; if (!api_p_char_info(IR_API, ch, &ci)) return;
    o->v[0] = ci.health; o->v[1] = ci.max_health; o->v[2] = ci.guard; o->v[3] = ci.max_guard; o->v[4] = (float)ci.stance; o->v[5] = (float)ci.style;
    o->v[6] = (float)ci.attacking; o->v[7] = (float)ci.guarding; o->v[8] = (float)ci.is_down;
    void *ac = se_comp(IR_API, ch, "m_AttackComponent"); void *f = ac ? api_p_find_function(IR_API, ac, "BPF_GetShards") : NULL; float sh = -1; if (f && api_p_call_params(IR_API, ac, f, NULL, &sh)) o->v[9] = sh;
    o->v[10] = ci.attacking > 0 && g_ir_atk[s].ch == ch ? (float)((ir_t() - g_ir_atk[s].t) * 30.0) : -1.0f;
}
// pose: root component ComponentToWorld (location + rotation quaternion just before it, same read as api_p_test_teleport's keep-yaw path)
static void ir_pose(void *ch, IrPose *q) {
    float l[3]; memset(q, 0, sizeof *q); if (!ch || !api_p_test_get_location(IR_API, ch, l)) return;
    __try { void *root = *(void **)((uint8_t *)ch + OFF_ACTOR_ROOTCOMP); const float *r = (const float *)((uint8_t *)root + OFF_ROOT_C2W_POS - 16);
            float yaw = atan2f(2.f * (r[3] * r[2] + r[0] * r[1]), 1.f - 2.f * (r[1] * r[1] + r[2] * r[2])) * 57.29578f; if (!isfinite(yaw)) return;
            q->x = l[0]; q->y = l[1]; q->yaw = yaw; q->ok = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { q->ok = 0; }
}
static void ir_axes_out(double t) {
    for (int i = 0; i < 16; i++) {
        IrAx *a = &g_ir_ax[i]; if (!a->used) continue; float v = a->rel ? a->sum : a->val;
        if (ir_axis_emit(a->rel, a->last, v)) { char b[160]; ir_put(b, ir_enc_axis(b, sizeof b, t, a->name, v)); a->last = v; }
        a->sum = 0;
    }
}
static int ir_stick_dir(void) {
    float x = (float)(g_ir_wasd[3] - g_ir_wasd[1]), y = (float)(g_ir_wasd[0] - g_ir_wasd[2]);
    for (int i = 0; i < 16; i++) if (g_ir_ax[i].used && !g_ir_ax[i].rel) { if (!strcmp(g_ir_ax[i].name, "Gamepad_LeftX") && fabsf(g_ir_ax[i].val) > fabsf(x)) x = g_ir_ax[i].val; if (!strcmp(g_ir_ax[i].name, "Gamepad_LeftY") && fabsf(g_ir_ax[i].val) > fabsf(y)) y = g_ir_ax[i].val; }
    return ir_dir8(x, y);
}
static void ir_tick(float dt, void *u) {
    (void)dt; (void)u; if (g_ir_state < 0) return;
    double c0 = ir_now(); g_ir_frames++;
    __try {
        if (g_ir_gen != g_se_world_gen) {                               // new world: forget characters, write the map, flush
            g_ir_gen = g_se_world_gen; g_ir_foe = g_ir_last_foe_seen = NULL; memset(g_ir_atk, 0, sizeof g_ir_atk); memset(g_ir_layer, 0, sizeof g_ir_layer); g_ir_have_prev = 0;
            if (g_ir_state == 1) { char mp[300] = ""; void *w = api_p_get_world(IR_API); se_path_of(w, mp, sizeof mp); char b[400]; ir_put(b, ir_enc_event(b, sizeof b, "world", ir_t(), NULL, NULL, NULL, (int)g_ir_gen, 0, 0, mp)); EnterCriticalSection(&g_ir_cs); ir_flush(); LeaveCriticalSection(&g_ir_cs); }
        }
        double now = ir_now();
        if (now - g_ir_last_sample >= 1.0 / g_ir_hz) {
            g_ir_last_sample = now; g_ir_samples++; void *p = cd_local_pawn();
            if (p && g_ir_state == 0) ir_start_file();
            if (p && g_ir_state == 1) {
                double t = ir_t(); ir_axes_out(t);
                void *tgt = NULL; void *ft = api_p_find_function(IR_API, p, "GetTarget"); if (ft) api_p_call_params(IR_API, p, ft, NULL, &tgt);
                void *foe = se_valid_ptr(tgt) ? tgt : g_ir_last_foe_seen;
                if (foe != g_ir_foe) { g_ir_foe = foe; memset(&g_ir_layer[1], 0, sizeof g_ir_layer[1]); if (foe) g_ir_fight++; }
                if (now - g_ir_last_round_read >= 1.0) { g_ir_last_round_read = now; AbsMatchInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; g_ir_round = api_p_match_info(IR_API, &mi) && mi.is_duel ? mi.round : 0; }
                IrObs o; memset(&o, 0, sizeof o); o.fight = g_ir_fight; o.round = g_ir_round; o.lock = se_valid_ptr(tgt) ? 1 : 0; o.dir = ir_stick_dir(); o.d = -1;
                ir_side(p, 0, &o.p); ir_side(g_ir_foe, 1, &o.o); ir_pose(p, &o.pp); ir_pose(g_ir_foe, &o.po);
                if (g_ir_layer[0].on) snprintf(o.pl, sizeof o.pl, "%s", g_ir_layer[0].name); if (g_ir_foe && g_ir_layer[1].on) snprintf(o.ol, sizeof o.ol, "%s", g_ir_layer[1].name);
                if (g_ir_foe) { float a[3], b[3]; if (api_p_test_get_location(IR_API, p, a) && api_p_test_get_location(IR_API, g_ir_foe, b)) o.d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
                    AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; if (api_p_char_info(IR_API, g_ir_foe, &ci)) { snprintf(o.on, sizeof o.on, "%s", ci.name); o.op = ci.is_player; } }
                if (!g_ir_have_prev || ir_obs_changed(&o, &g_ir_prev) || now - g_ir_last_obs >= IR_HEARTBEAT_S) { char b[700]; ir_put(b, ir_enc_obs(b, sizeof b, t, &o)); g_ir_prev = o; g_ir_have_prev = 1; g_ir_last_obs = now; }
            }
        }
        if (g_ir_state == 1 && (ir_buf_due(&g_ir_buf) || now - g_ir_last_flush >= 2.0)) { EnterCriticalSection(&g_ir_cs); ir_flush(); LeaveCriticalSection(&g_ir_cs); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("inputrec: EXCEPTION 0x%08lX in the sampler (skipped)", GetExceptionCode()); }
    double c1 = ir_now(); g_ir_cost_s += c1 - c0;
    if (c1 - g_ir_last_cost >= 30.0) {
        if (g_ir_frames) logf_("inputrec: cost %.1f us/frame (sampler %.1f + hooks %.1f), %.1f us per sample, %.3f ms per second of play, %lld frames, %lld samples, %lld lines, %lld dropped, part %d %lld bytes, state %d",
                               (g_ir_cost_s + g_ir_hook_s) * 1e6 / g_ir_frames, g_ir_cost_s * 1e6 / g_ir_frames, g_ir_hook_s * 1e6 / g_ir_frames, g_ir_samples ? g_ir_cost_s * 1e6 / g_ir_samples : 0.0,
                               (g_ir_cost_s + g_ir_hook_s) * 1e3 / (c1 - g_ir_last_cost), g_ir_frames, g_ir_samples, g_ir_lines, g_ir_buf.dropped, g_ir_part, g_ir_file_bytes, g_ir_state);
        g_ir_last_cost = c1; g_ir_cost_s = g_ir_hook_s = 0; g_ir_frames = g_ir_samples = 0;
    }
}
// bot_reads_inputs: a bot may see the player's live stick direction ONLY with this development flag. Default off, never in a package. -1 = not allowed / unknown.
static int ir_player_dir_for_bot(void) { return g_ir_bot_reads && g_ir_state >= 0 ? ir_stick_dir() : -1; }

static int ir_cfg_on(const char *key) { char v[32] = ""; wchar_t src[MAX_PATH]; return setting_get(key, L"", 0, v, sizeof v, src) && (!_stricmp(v, "on") || !_stricmp(v, "1") || !_stricmp(v, "true")); }
static double ir_cfg_num(const char *key, double dflt) { char v[32] = ""; wchar_t src[MAX_PATH]; return setting_get(key, L"", 0, v, sizeof v, src) && v[0] ? atof(v) : dflt; }
static void ir_init(const AbsApi *a) {
    IR_API = a; g_ir_on = ir_cfg_on("input_rec"); g_ir_bot_reads = ir_cfg_on("bot_reads_inputs");
    if (g_ir_bot_reads) logf_("inputrec: WARNING bot_reads_inputs = on: bots may read the player's inputs. DEVELOPMENT ONLY - never in a release package%s", g_ir_on ? "" : " (no effect: input_rec is off)");
    if (!g_ir_on) { g_ir_state = -1; logf_("inputrec: off (config input_rec not set; nothing hooked, nothing written)"); (void)ir_player_dir_for_bot; return; }
    g_ir_hz = (int)ir_cfg_num("input_rec_hz", 30); if (g_ir_hz < 5) g_ir_hz = 5; if (g_ir_hz > 120) g_ir_hz = 120;
    g_ir_file_cap = (long long)(ir_cfg_num("input_rec_file_mb", 64) * 1048576.0); g_ir_dir_cap = (long long)(ir_cfg_num("input_rec_dir_mb", 2048) * 1048576.0);
    { char v[64] = ""; wchar_t src[MAX_PATH]; if (setting_get("input_rec_dir", L"", 0, v, sizeof v, src) && v[0]) {
        if (ir_subdir_ok(v)) snprintf(g_ir_sub, sizeof g_ir_sub, "%s", v); else logf_("inputrec: input_rec_dir '%s' refused (a LanNative subfolder: letters, digits, _ - and \\ only, no ..); using recordings", v); } }
    if (strchr(g_ir_sub, '\\')) { wchar_t d[MAX_PATH], w[64]; MultiByteToWideChar(CP_UTF8, 0, g_ir_sub, -1, w, 64); *wcschr(w, L'\\') = 0; swprintf(d, MAX_PATH, L"%s%ls", g_dir, w); CreateDirectoryW(d, NULL); }   // one level of parent
    InitializeCriticalSection(&g_ir_cs); g_ir_t0 = ir_now(); g_ir_last_flush = g_ir_last_cost = g_ir_t0;
    o_ir_key = (IrKey_t)api_p_hook_symbol(a, "APlayerController::InputKey", (void *)hk_ir_key);
    o_ir_axis = (IrAxis_t)api_p_hook_symbol(a, "APlayerController::InputAxis", (void *)hk_ir_axis);
    o_ir_notify = (IrNotify_t)api_p_hook_symbol(a, "UAvailabilityLayerNotifyState::NotifyOutside", (void *)hk_ir_notify);
    int ea = api_p_on_attack_start(a, ir_on_attack, NULL), eh = api_p_on_hit(a, ir_on_hit, NULL), ed = api_p_on_defense(a, ir_on_defense, NULL), ex = api_p_on_pawn_destroyed(a, ir_on_destroyed, NULL), et = api_p_on_tick(a, ir_tick, NULL);
    logf_("inputrec: ON hz %d, file cap %lld MB, folder budget %lld MB | hooks key %p axis %p layer %p | events attack %d hit %d defense %d destroyed %d tick %d | output <LanNative>\\recordings (personal data, local only)",
          g_ir_hz, g_ir_file_cap >> 20, g_ir_dir_cap >> 20, (void *)o_ir_key, (void *)o_ir_axis, (void *)o_ir_notify, ea, eh, ed, ex, et);
    if (!o_ir_key || et) { logf_("inputrec: the key hook or the tick is missing - recorder OFF"); g_ir_state = -1; return; }
    atexit(ir_exit); g_ir_state = 0;
}

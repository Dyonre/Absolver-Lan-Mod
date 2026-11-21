// modeclaim.inc - API 2.31 mode_claim / mode_release / mode_holder (docs/API-REFERENCE.md). Rules: modeclaim.h.
// A plugin that runs its own mode on a pawn (skating) claims the pawn instead of fighting the game every frame: the loader switches the native systems
// off and gives them back exactly on release. One owner per pawn. Groups (flags):
//   MOVEMENT  the CharacterMovement component's tick off (no per-frame Flying re-force). Release: tick back as it was, ground modes -> Falling with
//             bForceNextFloorCheck (the CMC lands the pawn wherever the plugin left it, with the plugin's last Velocity), other modes as they were.
//   COLLISION the capsule to QueryOnly (sweeps / traces still hit it). Release: profile, then enabled + object type, then each channel that differs.
//   POSE      the mesh's bPauseAnims on (AnimBP update stops; evaluation and FinalizeBoneTransform still run; the pose stage writes the pose there).
// Every claim logs what it saved; every release reads everything back and logs the difference (0 = restored exactly). Auto-release: the plugin is
// switched off (3 faults) or the world gen moved while the pawn is still alive (a GameState-only change) -> restored; the pawn is destroyed (alone or
// with its world) -> dropped (nothing left to restore), each logged.
// After a release the landing is watched for 3 s (mode, height lost) so a fall-through shows in the log. All calls are reflected (by name), no offsets.
#include "modeclaim.h"
static McTable g_mc;
typedef struct { void *pawn; uint8_t want; float z0; ULONGLONG t0; unsigned gen; char mode[96]; } McWatch;
static McWatch g_mc_w[MC_MAX]; static int g_mc_wn;

// a reflected call with exactly nargs inputs: a signature that differs (another build) is logged and not called, instead of reading past args
static int mc_call(void *obj, const char *fn, void **args, int nargs, void *ret) {
    if (!obj) return 0;
    void *f = api_p_find_function(NULL, obj, fn); if (!f) { logf_("mode: %s not found on %p - step skipped", fn, obj); return 0; }
    SeParam ps[32]; int n = 0, ins = 0;
    __try { n = se_params_of(f, ps, 32); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mode: EXCEPTION 0x%08lX reading the parameters of %s", GetExceptionCode(), fn); return 0; }
    for (int i = 0; i < n; i++) if (!(ps[i].flags & CPF_RETURNPARM)) ins++;
    if (ins != nargs) { logf_("mode: %s takes %d input(s), the service passes %d - not called (different game build?)", fn, ins, nargs); return 0; }
    return api_p_call_params(NULL, obj, f, args, ret);
}
static int mc_get_u8(void *obj, const char *fn, uint8_t *out) { uint64_t r = 0; if (!mc_call(obj, fn, NULL, 0, &r)) return 0; *out = (uint8_t)r; return 1; }
static int mc_set_u8(void *obj, const char *fn, uint8_t v) { void *a[1] = { &v }; return mc_call(obj, fn, a, 1, NULL); }
static int mc_bool(void *obj, const char *name) { int v = -1; return api_p_get_bool(NULL, obj, name, &v) ? v : -1; }   // -1 = not a reflected bool
static void *mc_cmc(void *pawn) { return cc_obj_prop(pawn, "CharacterMovement"); }
static void *mc_caps(void *pawn) { return cc_obj_prop(pawn, "CapsuleComponent"); }
static void *mc_mesh(void *pawn) { return cc_obj_prop(pawn, "Mesh"); }
static float mc_z(void *pawn) { float v[4] = { 0 }; return mc_call(pawn, "K2_GetActorLocation", NULL, 0, v) ? v[2] : 0.f; }
static const char *mc_groups(unsigned f, char *b, int cap) { snprintf(b, (size_t)cap, "%s%s%s", f & MC_MOVEMENT ? "MOVEMENT " : "", f & MC_COLLISION ? "COLLISION " : "", f & MC_POSE ? "POSE" : ""); return b; }

// read the groups in `groups` into s (s->have gets the groups that read completely). Returns 1 when every asked group read.
static int mc_read(void *pawn, unsigned groups, McSnap *s) {
    int ok = 1;
    if (groups & MC_MOVEMENT) {
        void *cmc = mc_cmc(pawn); uint8_t t = 0, *mm = cmc ? (uint8_t *)api_p_prop_ptr(NULL, cmc, "MovementMode") : NULL, *cm = cmc ? (uint8_t *)api_p_prop_ptr(NULL, cmc, "CustomMovementMode") : NULL;
        float *v = cmc ? (float *)api_p_prop_ptr(NULL, cmc, "Velocity") : NULL;
        if (cmc && mm && cm && v && mc_get_u8(cmc, "IsComponentTickEnabled", &t)) { s->tick_on = t != 0; s->move_mode = *mm; s->custom_mode = *cm; memcpy(s->vel, v, sizeof s->vel); s->have |= MC_MOVEMENT; }
        else { logf_("mode: movement state of %p not readable (cmc %p)", pawn, cmc); ok = 0; }
    }
    if (groups & MC_COLLISION) {
        void *c = mc_caps(pawn); int good = c && mc_call(c, "GetCollisionProfileName", NULL, 0, &s->profile) && mc_get_u8(c, "GetCollisionEnabled", &s->coll_enabled) && mc_get_u8(c, "GetCollisionObjectType", &s->obj_type);
        for (int ch = 0; good && ch < MC_CHANNELS; ch++) { uint8_t chn = (uint8_t)ch; uint64_t r = 0; void *a[1] = { &chn }; good = mc_call(c, "GetCollisionResponseToChannel", a, 1, &r); s->resp[ch] = (uint8_t)r; }
        if (good) s->have |= MC_COLLISION; else { logf_("mode: collision state of %p not readable (capsule %p)", pawn, c); ok = 0; }
    }
    if (groups & MC_POSE) {
        void *m = mc_mesh(pawn); int p = m ? mc_bool(m, "bPauseAnims") : -1;
        if (p >= 0) { s->pause_anims = p; s->auto_only = mc_bool(m, "bOnlyAllowAutonomousTickPose"); s->auto_is = mc_bool(m, "bIsAutonomousTickPose"); s->have |= MC_POSE; }
        else { logf_("mode: bPauseAnims of %p not readable (mesh %p)", pawn, m); ok = 0; }
    }
    return ok;
}
// take the groups (native off); returns the groups that were taken
static unsigned mc_take(void *pawn, unsigned groups) {
    unsigned done = 0;
    if (groups & MC_MOVEMENT) { unsigned char off = 0; void *a[1] = { &off }; if (mc_call(mc_cmc(pawn), "SetComponentTickEnabled", a, 1, NULL)) done |= MC_MOVEMENT; }
    if (groups & MC_COLLISION) { if (mc_set_u8(mc_caps(pawn), "SetCollisionEnabled", 1)) done |= MC_COLLISION; }   // ECollisionEnabled::QueryOnly
    if (groups & MC_POSE) { void *m = mc_mesh(pawn); if (m && api_p_set_bool(NULL, m, "bPauseAnims", 1)) done |= MC_POSE; }
    return done;
}
// give the groups back from s (pose, collision, then movement: the CMC's first tick sees the capsule as it was); returns the differences read back
static int mc_give_back(void *pawn, unsigned groups, const McSnap *s, const char *who, const char *why) {
    if (groups & MC_POSE) { void *m = mc_mesh(pawn); if (!m || !api_p_set_bool(NULL, m, "bPauseAnims", s->pause_anims)) logf_("mode: %s: bPauseAnims NOT restored", who); }
    if (groups & MC_COLLISION) {
        // The profile first; then only what still differs: every setter after it (enabled, object type, a channel) renames the profile "Custom" in
        // 4.18 even for an unchanged value, and a named profile had no per-instance overrides to put back.
        void *c = mc_caps(pawn); uint64_t prof = s->profile; void *a[1] = { &prof };
        mc_call(c, "SetCollisionProfileName", a, 1, NULL);
        McSnap now; memset(&now, 0, sizeof now); int idx[MC_CHANNELS], nd = 0;
        if (mc_read(pawn, MC_COLLISION, &now)) {
            if (now.coll_enabled != s->coll_enabled) mc_set_u8(c, "SetCollisionEnabled", s->coll_enabled);
            if (now.obj_type != s->obj_type) mc_set_u8(c, "SetCollisionObjectType", s->obj_type);
            nd = mc_resp_diff(s->resp, now.resp, idx);
        }
        for (int i = 0; i < nd; i++) { uint8_t ch = (uint8_t)idx[i], r = s->resp[idx[i]]; void *a2[2] = { &ch, &r }; mc_call(c, "SetCollisionResponseToChannel", a2, 2, NULL); }
        if (nd) logf_("mode: %s: %d channel response(s) set back one by one after the profile", who, nd);
    }
    if (groups & MC_MOVEMENT) {
        void *cmc = mc_cmc(pawn); unsigned char on = (unsigned char)s->tick_on; void *a[1] = { &on };
        mc_call(cmc, "SetComponentTickEnabled", a, 1, NULL);
        uint8_t m = mc_release_mode(s->move_mode), cm = m == 6 ? s->custom_mode : 0; void *a2[2] = { &m, &cm };
        mc_call(cmc, "SetMovementMode", a2, 2, NULL);
        int fnc = cmc && api_p_set_bool(NULL, cmc, "bForceNextFloorCheck", 1);
        float *v = cmc ? (float *)api_p_prop_ptr(NULL, cmc, "Velocity") : NULL;
        logf_("mode: %s: movement back: tick %d, mode %u -> %u/%u, bForceNextFloorCheck %s, velocity handed over %.0f,%.0f,%.0f", who, s->tick_on, s->move_mode, m, cm, fnc ? "set" : "NOT SET", v ? v[0] : 0.f, v ? v[1] : 0.f, v ? v[2] : 0.f);
        if (g_mc_wn < MC_MAX) { McWatch *w = &g_mc_w[g_mc_wn++]; w->pawn = pawn; w->want = s->move_mode; w->z0 = mc_z(pawn); w->t0 = GetTickCount64(); w->gen = g_se_world_gen; snprintf(w->mode, sizeof w->mode, "%s", who); }
    }
    McSnap back; memset(&back, 0, sizeof back); mc_read(pawn, groups, &back);
    char d[512], t[400]; int n = mc_snap_diff(s, &back, groups & back.have, d, sizeof d); mc_snap_text(&back, t, sizeof t);
    if ((groups & back.have) != groups) logf_("mode: %s: restore check INCOMPLETE - some groups could not be read back", who);
    logf_("mode: %s released (%s): after %s", who, why, t);
    logf_("mode: %s restore check: %d difference(s)%s%s", who, n, n ? " - NOT IDENTICAL: " : " (identical)", d);
    return n;
}

static void mc_release_at(int i, const char *why) {
    McClaim *c = &g_mc.c[i]; char who[96]; snprintf(who, sizeof who, "%s/%s pawn %p", plug_name_of(c->plugin), c->mode, c->pawn);
    __try { mc_give_back(c->pawn, c->flags, &c->saved, who, why); } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mode: EXCEPTION 0x%08lX restoring %s", GetExceptionCode(), who); }
    mc_remove_at(&g_mc, i);
}

// every frame after the plugins' on_tick (0300) and the camera claims (0310)
static void mc_tick(void) {
    for (int i = g_mc.n - 1; i >= 0; i--) {
        McClaim *c = &g_mc.c[i];
        if (!ln_obj_alive(c->pawn)) { logf_("mode: claim %s/%s on pawn %p dropped (the pawn was destroyed%s, nothing to restore)", plug_name_of(c->plugin), c->mode, c->pawn, c->gen != g_se_world_gen ? " with the world" : ""); mc_remove_at(&g_mc, i); continue; }
        // the world gen also moves when only the GameState changes (secore.inc se_world_check): a pawn that is still alive gets everything back
        if (c->gen != g_se_world_gen) { mc_release_at(i, "the world changed, pawn still alive"); continue; }
        if (!plug_guard(c->plugin)) mc_release_at(i, "the plugin was switched off");
    }
    if (!g_mc_wn) return;
    ULONGLONG now = GetTickCount64();
    for (int i = g_mc_wn - 1; i >= 0; i--) {
        McWatch *w = &g_mc_w[i]; int done = 0;
        if (!ln_obj_alive(w->pawn)) { logf_("mode: %s landing watch ended (pawn gone%s)", w->mode, w->gen != g_se_world_gen ? " with the world" : ""); done = 1; }
        else if (mc_find(&g_mc, w->pawn) >= 0) { logf_("mode: %s landing watch ended after %.2f s (the pawn was claimed again: %s)", w->mode, (double)(now - w->t0) / 1000.0, g_mc.c[mc_find(&g_mc, w->pawn)].mode); done = 1; }   // a plugin that releases and re-claims in the same millisecond (e.g. dismount, then carry)
        else {
            void *cmc = mc_cmc(w->pawn); uint8_t *mm = cmc ? (uint8_t *)api_p_prop_ptr(NULL, cmc, "MovementMode") : NULL; float drop = w->z0 - mc_z(w->pawn); double s = (double)(now - w->t0) / 1000.0;
            int ground = w->want == 1 || w->want == 2, landed = mm && (ground ? (*mm == 1 || *mm == 2) : *mm == w->want);
            if (landed || s >= 3.0) {
                logf_("mode: %s %s after %.2f s: mode %u (was %u at the claim), height lost %.1f cm%s", w->mode, landed ? "settled" : "NOT settled", s, mm ? *mm : 255, w->want, drop, drop > 300.f ? " - WARNING: fell more than 3 m (fall-through?)" : "");
                done = 1;
            }
        }
        if (done) g_mc_w[i] = g_mc_w[--g_mc_wn];
    }
}
LN_PRETICK(0320, modeclaim) { __try { mc_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("mode: EXCEPTION 0x%08lX in the claim check", GetExceptionCode()); } }

// ----- API
static int api_p_mode_claim(const AbsApi *self, void *pawn, const char *mode, unsigned flags) {
    int p = plug_of(self); if (p < 0) return -1; const char *pn = plug_name_of(p); char gb[40];
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: mode_claim refused - not on the game thread", pn); return -1; }
    if (!ln_obj_alive(pawn) || !api_p_is_a(self, pawn, "Character")) { logf_("plugin %s: mode_claim refused - %p is not a live Character", pn, pawn); return -1; }
    McDecision d = mc_decide(&g_mc, p, pawn, flags);
    if (d.what == MC_BAD) { logf_("plugin %s: mode_claim refused - flags 0x%X (known: 1 MOVEMENT, 2 COLLISION, 4 POSE)", pn, flags); return -1; }
    if (d.what == MC_FULL) { logf_("plugin %s: mode_claim refused - %d claims in all", pn, MC_MAX); return -1; }
    if (d.what == MC_BUSY) { static int nl; if (nl++ < 20) logf_("plugin %s: mode_claim on %p: held by %s/%s - not yours", pn, pawn, plug_name_of(g_mc.c[d.index].plugin), g_mc.c[d.index].mode); return 0; }
    McClaim *c; if (d.what == MC_NEW) { c = &g_mc.c[g_mc.n]; memset(c, 0, sizeof *c); c->plugin = p; c->pawn = pawn; c->gen = g_se_world_gen; } else c = &g_mc.c[d.index];
    snprintf(c->mode, sizeof c->mode, "%s", mode && *mode ? mode : "mode");
    if (d.what == MC_SAME) return 1;
    char who[96]; snprintf(who, sizeof who, "%s/%s pawn %p", pn, c->mode, pawn); int ok = 1;
    __try {
        if (d.restore) { mc_give_back(pawn, d.restore, &c->saved, who, "groups dropped by a new claim"); c->flags &= ~d.restore; c->saved.have &= ~d.restore; }
        if (d.apply) {
            McSnap s = c->saved; s.have &= ~d.apply;
            if (!mc_read(pawn, d.apply, &s)) { logf_("mode: %s claim of %s refused - state not readable, nothing changed", who, mc_groups(d.apply, gb, sizeof gb)); ok = 0; }
            else {
                unsigned got = mc_take(pawn, d.apply); c->saved = s;
                if (got != d.apply) { logf_("mode: %s claim refused - taking %s failed, giving back what was taken", who, mc_groups(d.apply & ~got, gb, sizeof gb)); if (got) mc_give_back(pawn, got, &s, who, "claim failed"); ok = 0; }
                else c->flags |= d.apply;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mode: EXCEPTION 0x%08lX in mode_claim of %s", GetExceptionCode(), who); ok = 0; }
    if (d.what == MC_NEW && ok) g_mc.n++;
    if (!ok) return -1;
    char t[400]; mc_snap_text(&c->saved, t, sizeof t);
    logf_("mode: %s %s: holds %s; saved %s", who, d.what == MC_NEW ? "claimed" : "claim updated", mc_groups(c->flags, gb, sizeof gb), t);
    return 1;
}
static int api_p_mode_release(const AbsApi *self, void *pawn) {
    int p = plug_of(self); if (p < 0) return -1;
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: mode_release refused - not on the game thread", plug_name_of(p)); return -1; }
    int i = mc_find(&g_mc, pawn); if (i < 0 || g_mc.c[i].plugin != p) return 1;
    if (!ln_obj_alive(pawn)) { logf_("mode: claim %s/%s on pawn %p released after its pawn went (nothing to restore)", plug_name_of(p), g_mc.c[i].mode, pawn); mc_remove_at(&g_mc, i); return 0; }
    mc_release_at(i, g_mc.c[i].gen != g_se_world_gen ? "mode_release (after a world change, pawn still alive)" : "mode_release");
    return 0;
}
static int api_p_mode_holder(const AbsApi *self, void *pawn, char *mode, int cap, unsigned *flags) {
    int p = plug_of(self), i = mc_find(&g_mc, pawn);
    if (mode && cap > 0) snprintf(mode, (size_t)cap, "%s", i >= 0 ? g_mc.c[i].mode : "");
    if (flags) *flags = i >= 0 ? g_mc.c[i].flags : 0;
    return i < 0 ? -1 : g_mc.c[i].plugin == p ? 1 : 0;
}
LN_API(modeclaim) { a->mode_claim = api_p_mode_claim; a->mode_release = api_p_mode_release; a->mode_holder = api_p_mode_holder; }

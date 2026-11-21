// posestage.inc - API 2.32 pose_stage / pose_handoff (docs/API-REFERENCE.md). Bookkeeping: posestage.h;
// pose maths: tools/posekit/posestage.h (inertialization in local space, closed form on game time).
// A plugin that switches its mesh's clips (dynamic montages) turns a stage on for the pawn, then at each switch CUTS the clip (blend 0) and
// calls pose_handoff: from the next pose on, the loader decays the difference between what was on screen and the new clip (no crossfade dip).
// The loader owns one FinalizeBoneTransform detour, installed at start-up BEFORE any plugin loads: plugin detours stack outside it, so a plugin's IK
// (which edits the same read buffer after calling next) always sees the staged pose. No stage = the detour returns right after the game's code.
// Off by itself when the plugin is switched off, the pawn or mesh is gone, or 3 faults in the detour (each logged). Uses modeclaim.inc's mc_call.
#include "posestage.h"
#define PST_CSA 0x6D8    // USkeletalMeshComponent::ComponentSpaceTransformsArray[2] (TArray<FTransform>, 16 B each)
#define PST_READ 0x6FC   // CurrentReadComponentTransforms (0 / 1)
static PstTable g_pst; static volatile int g_pst_live; static volatile unsigned g_pst_frame; static volatile float g_pst_dt = 1.f / 60.f;
static void (*g_pst_next)(void *); static volatile int g_pst_faults;

static void pst_stage_mesh(void *c, PstSlot *s) {
    int ri = *(int *)((uint8_t *)c + PST_READ); if (ri & ~1) return;
    InXf *cs = *(InXf **)((uint8_t *)c + PST_CSA + 16 * ri); int n = *(int *)((uint8_t *)c + PST_CSA + 8 + 16 * ri);
    if (!cs || n != s->st->n) { static int lg; if (lg++ < 5) logf_("pose: %s mesh %p: read buffer has %d bones, the stage %d - frame skipped", plug_name_of(s->plugin), c, n, s->st->n); return; }
    int sw = s->st->switches, ig = s->st->ignored;
    ps_frame(s->st, cs, pst_step_dt(s, g_pst_frame, g_pst_dt));
    if (s->st->switches != sw) { float cm = 0.f, dg = 0.f; in_stats(&s->st->I, &cm, &dg); logf_("pose: %s hand-off %d on mesh %p: offset %.1f cm / %.1f deg at the switch, guard %d", plug_name_of(s->plugin), s->st->switches, c, cm, dg, s->st->I.clamped); }
    if (s->st->ignored != ig) logf_("pose: %s hand-off on mesh %p dropped (no pose shown yet)", plug_name_of(s->plugin), c);
}
static void pst_detour(void *c) {
    g_pst_next(c);
    if (!g_pst_live || g_pst_faults >= 3) return;
    PstSlot *s = pst_find(&g_pst, c); if (!s) return;
    __try { pst_stage_mesh(c, s); }
    __except (EXCEPTION_EXECUTE_HANDLER) { int f = ++g_pst_faults; logf_("pose: EXCEPTION 0x%08lX staging mesh %p (fault %d of 3)%s", GetExceptionCode(), c, f, f >= 3 ? " - pose stages OFF for this run" : ""); }
}
LN_INIT(0110, posestage) {   // after the extender core (0100: symbols); plugins load much later, so their detours stack outside this one
    if (g_safe_mode) return;
    g_pst_next = (void (*)(void *))api_p_hook_symbol(NULL, "USkeletalMeshComponent::FinalizeBoneTransform", (void *)pst_detour);
    logf_("pose: FinalizeBoneTransform stage %s", g_pst_next ? "installed (idle until a plugin turns a stage on)" : "NOT installed - pose_stage refuses");
}

static void pst_off(PstSlot *s, const char *why) {
    logf_("pose: stage of %s on mesh %p off (%s; %d hand-off(s), %d extra call(s) in one frame)", plug_name_of(s->plugin), s->mesh, why, s->st ? s->st->switches : 0, s->dups);
    s->live = 0; MemoryBarrier(); g_pst_live = pst_live_count(&g_pst);
}
// every frame after the plugins' on_tick: the frame counter + game dt the detour uses (real dt x global time dilation), auto-off checks
static void pst_tick(void) {
    g_pst_frame++;
    if (!g_pst_live) return;
    float dil = 1.f; static void *cdo; if (!cdo) cdo = api_p_find_cdo(NULL, "GameplayStatics");
    for (int i = 0; i < PST_MAX; i++) {
        PstSlot *s = &g_pst.s[i]; if (!s->live) continue;
        if (!plug_guard(s->plugin)) { pst_off(s, "the plugin was switched off"); continue; }
        if (!ln_obj_alive(s->pawn) || !ln_obj_alive(s->mesh)) { pst_off(s, "the pawn or its mesh is gone"); continue; }
        if (dil == 1.f && cdo) { void *a[1] = { &s->pawn }; float d = 1.f; if (mc_call(cdo, "GetGlobalTimeDilation", a, 1, &d) && d > 0.f && d <= 20.f) dil = d; }
    }
    g_pst_dt = g_ln_dt * dil;
}
LN_PRETICK(0330, posestage) { __try { pst_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("pose: EXCEPTION 0x%08lX in the stage check", GetExceptionCode()); } }

// ----- API
static int pst_bind(PstSlot *s, void *mesh) {   // parent table by reflection (game thread)
    int nb = 0; if (!mc_call(mesh, "GetNumBones", NULL, 0, &nb) || nb <= 0 || nb > PS_MAX_BONES) { logf_("pose: mesh %p has %d bones (1..%d) - refused", mesh, nb, PS_MAX_BONES); return 0; }
    static uint64_t names[PS_MAX_BONES]; static int par[PS_MAX_BONES];
    for (int i = 0; i < nb; i++) { int ii = i; void *a[1] = { &ii }; names[i] = 0; mc_call(mesh, "GetBoneName", a, 1, &names[i]); }
    for (int i = 0; i < nb; i++) {
        uint64_t me = names[i], p = 0; void *a[1] = { &me }; mc_call(mesh, "GetParentBone", a, 1, &p); par[i] = -1;
        for (int k = 0; p && k < nb; k++) if (names[k] == p) { par[i] = k; break; }
    }
    if (!s->st) s->st = (PsStage *)calloc(1, sizeof(PsStage));
    if (!s->st || !ps_init(s->st, par, nb)) { logf_("pose: mesh %p: bone order not parents-first (or no memory) - refused", mesh); return 0; }
    return nb;
}
static int api_p_pose_stage(const AbsApi *self, void *pawn, int on) {
    int p = plug_of(self); if (p < 0) return -1; const char *pn = plug_name_of(p);
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: pose_stage refused - not on the game thread", pn); return -1; }
    if (!ln_obj_alive(pawn)) { logf_("plugin %s: pose_stage refused - %p is not a live object", pn, pawn); return -1; }
    void *mesh = cc_obj_prop(pawn, "Mesh"); PstSlot *s = mesh ? pst_find(&g_pst, mesh) : NULL;
    if (!on) { if (!s || s->plugin != p) return 0; pst_off(s, "pose_stage off"); return 0; }
    if (!g_pst_next || g_pst_faults >= 3) { logf_("plugin %s: pose_stage refused - the stage hook is not installed / off", pn); return -1; }
    if (!mesh || !ln_obj_alive(mesh)) { logf_("plugin %s: pose_stage refused - %p has no live Mesh", pn, pawn); return -1; }
    if (s) { if (s->plugin != p) { logf_("plugin %s: pose_stage on mesh %p: held by %s", pn, mesh, plug_name_of(s->plugin)); return 0; } return 1; }
    s = pst_free_slot(&g_pst); if (!s) { logf_("plugin %s: pose_stage refused - %d stages in all", pn, PST_MAX); return -1; }
    int nb = pst_bind(s, mesh); if (!nb) return -1;
    s->pawn = pawn; s->plugin = p; s->gen = g_se_world_gen; s->shown = 0; s->dups = 0; s->mesh = mesh; MemoryBarrier(); s->live = 1; g_pst_live = pst_live_count(&g_pst);
    logf_("plugin %s: pose stage on for pawn %p mesh %p (%d bones)", pn, pawn, mesh, nb);
    return 1;
}
static int api_p_pose_handoff(const AbsApi *self, void *pawn, float decay) {
    int p = plug_of(self); if (p < 0) return -1;
    void *mesh = pawn && ln_obj_alive(pawn) ? cc_obj_prop(pawn, "Mesh") : NULL; PstSlot *s = mesh ? pst_find(&g_pst, mesh) : NULL;
    if (!s || s->plugin != p) { static int lg; if (lg++ < 10) logf_("plugin %s: pose_handoff refused - no stage of yours on pawn %p (pose_stage first)", plug_name_of(p), pawn); return -1; }
    if (!s->st->hist_n) return 0;
    ps_handoff(s->st, decay); return 1;
}
LN_API(posestage) { a->pose_stage = api_p_pose_stage; a->pose_handoff = api_p_pose_handoff; }

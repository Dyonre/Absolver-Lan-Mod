// The test_* functions, API 2.6 and 2.8 (included by secore.inc before se_fill_api). Drivers that run a fight WITHOUT a keyboard and leave nothing broken:
//   test_heal / test_set_invincible   put a character right again (health, guard, down state, invincibility)
//   test_attack                        make any AFightingCharacter (player, NPC, training dummy) perform an attack row through the game's own attack order
//   test_guard                         raise / drop the guard of a character
//   test_teleport / test_get_location  place characters next to each other (the training dummy stands ~740 uu from the player)
//   test_spawn_npc                     the loader's NPC spawn chain (AAISpawner::BPF_WantsSpawn), then place the new character
//   test_dodge / test_ai / test_hittable / test_get_forward   dodge, stop / restart an NPC's behaviour tree, the AI hit flag, facing direction
// How an attack is launched (as UAttackBTTask::LaunchAttack does it): the AI's attack task builds a
// DelayedActionAttack {input action, quadrant, flags, attack FName, m_bForceDebugPlay} and hands it to UAttackComponent::PrepareToLaunchAttack, which cancels running attack orders, plays
// an FNetOrderStructAttack through ABaseCharacter::PlayOrder and runs the attack. With m_bForceDebugPlay the combo deck (UComboManager) is bypassed, so ANY row of the attack table plays.
// Pure logic and the object layout: testkit.h. Every native call is byte-checked first and runs inside a guard; game thread only.
#include "testkit.h"

typedef struct { uint32_t rva; uint8_t b[16]; const char *name; int state; } TkFn;
enum { TKF_PREPARE, TKF_DAACTOR, TKF_GUARD, TKF_CANDMG, TKF_PLAYDODGE, TKF_PREPDODGE, TKF_DACTOR, TKF_COUNT };
static TkFn g_tkf[TKF_COUNT] = {
    { 0x3088B0u, { 0x40,0x55,0x53,0x56,0x57,0x41,0x55,0x48,0x8D,0xAC,0x24,0x40,0xFE,0xFF,0xFF,0x48 }, "UAttackComponent::PrepareToLaunchAttack", 0 },
    { 0x2E3090u, { 0x48,0x89,0x5C,0x24,0x10,0x55,0x56,0x57,0x48,0x83,0xEC,0x70,0x0F,0xB6,0xDA,0x48 }, "DelayedActionAttack::DelayedActionAttack", 0 },
    { 0x3644A0u, { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7C,0x24,0x18,0x55 }, "AFightingCharacter::HandleGuard", 0 },
    { 0x35C9A0u, { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x99,0xE0,0x00,0x00 }, "UHealthComponent::CanBeDamaged", 0 },
    { 0x374D80u, { 0x4C,0x8B,0xDC,0x55,0x56,0x57,0x41,0x54,0x49,0x8D,0xAB,0xA8,0xFE,0xFF,0xFF,0x48 }, "AFightingCharacter::PlayDodgeOrder", 0 },
    { 0x376CE0u, { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x18,0x55,0x56,0x41,0x56,0x48,0x8D,0x68,0xA1,0x48 }, "AFightingCharacter::PrepareDodge", 0 },
    { 0x2E3010u, { 0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x88,0x51,0x10,0x48,0x8D,0x05 }, "DelayedAction::DelayedAction", 0 },
};
#define TK_RVA_VFT_DODGE    0x239A320u       // DelayedActionDodge::_vftable_ (slot 0 = its scalar deleting destructor at +0x2E88A0)
#define TK_OFF_INVINCIBLE   0x2A0            // UHealthComponent::m_bIsInvincible (plain bool; CanBeDamaged reads it with cmp byte ptr [rdi+0x2A0],0 at base+0x35CA1F)
#define TK_OFF_ATK_TARGET   0x434            // UAttackComponent::m_Target (UPROPERTY, weak)
#define TK_OFF_ATK_NEXT     0x444            // UAttackComponent::m_NextAttackTarget (weak; PrepareToLaunchAttack takes the aim from it)
static int g_tk_inv_ok;                      // 0 unchecked, 1 the invincibility offset is confirmed, -1 not
static int tk_fn_ok(int i) {
    TkFn *f = &g_tkf[i]; if (f->state) return f->state > 0;
    const HookEntry *he = hr_find(&g_hreg, f->rva); const uint8_t *live = he ? he->orig : g_base + f->rva;
    f->state = memcmp(live, f->b, 16) == 0 ? 1 : -1;
    if (f->state < 0) logf_("testkit: %s bytes differ from this build - that test function is off", f->name);
    return f->state > 0;
}
static int tk_inv_offset_ok(void) {
    if (g_tk_inv_ok) return g_tk_inv_ok > 0;
    static const uint8_t cmp[7] = { 0x80, 0xBF, 0xA0, 0x02, 0x00, 0x00, 0x00 };          // cmp byte ptr [rdi+0x2A0], 0
    g_tk_inv_ok = tk_fn_ok(TKF_CANDMG) && memcmp(g_base + 0x35CA1Fu, cmp, 7) == 0 ? 1 : -1;
    if (g_tk_inv_ok < 0) logf_("testkit: the invincibility flag offset could not be confirmed - invincibility is only set through ServerSetInvicible");
    return g_tk_inv_ok > 0;
}
static const char *tk_who(const AbsApi *self) { return plug_name_of(plug_of(self)); }
static int tk_gate(const AbsApi *self, const char *what, void *ch) {
    if (!tk_ptr_ok(ch)) { logf_("plugin %s: %s refused - not a character", tk_who(self), what); return 0; }
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: %s refused - not on the game thread", tk_who(self), what); return 0; }
    return 1;
}
static void *tk_comp(const AbsApi *self, void *ch, const char *prop) { void **pp = api_p_prop_ptr(self, ch, prop); return pp && tk_ptr_ok(*pp) ? *pp : NULL; }

// ----- state: AbsCharInfo.invincible
static int tk_invincible(const AbsApi *self, void *ch) {
    void *hc = tk_comp(self, ch, "m_HealthComponent"); if (!hc || !tk_inv_offset_ok()) return -1;
    return *((uint8_t *)hc + TK_OFF_INVINCIBLE) != 0;
}

// ----- test_heal: health to full, ghost damage and down state cleared, guard refilled
static int api_p_test_heal(const AbsApi *self, void *ch) {
    if (!tk_gate(self, "test_heal", ch)) return 0; int ok = 0;
    __try {
        void *hc = tk_comp(self, ch, "m_HealthComponent"), *dc = tk_comp(self, ch, "m_DefenseComponent");
        if (!hc) { logf_("plugin %s: test_heal: character has no health component", tk_who(self)); return 0; }
        float mx = se_prop_f(self, hc, "m_fMaxHealth") + se_prop_f(self, hc, "m_fBonusMaxHealth"); float *hp = api_p_prop_ptr(self, hc, "m_fHealth"), *gh = api_p_prop_ptr(self, hc, "m_fGhostDamage");
        api_p_set_bool(self, hc, "m_bIsDown", 0); api_p_set_bool(self, hc, "m_bHitOrderPlayedForCurrentDeath", 0);
        unsigned char *ds = api_p_prop_ptr(self, hc, "m_eDownState"); if (ds) *ds = 0;
        if (hp && mx > 0) *hp = mx;
        if (gh) *gh = 0;
        if (dc) { float *g = api_p_prop_ptr(self, dc, "m_fCurrentGuard"); float gm = se_prop_f(self, dc, "m_fMaxGuard"); if (g && gm > 0) *g = gm; }
        ok = hp != NULL; logf_("plugin %s: test_heal(%p): health -> %.1f", tk_who(self), ch, mx);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_heal: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); ok = 0; }
    return ok;
}
// ----- test_set_invincible: UHealthComponent::ServerSetInvicible(bool) (what the game's own training mode uses) and the flag read back; returns the resulting state (0/1) or -1
static int api_p_test_set_invincible(const AbsApi *self, void *ch, int on) {
    if (!tk_gate(self, "test_set_invincible", ch)) return -1; int r = -1;
    __try {
        void *hc = tk_comp(self, ch, "m_HealthComponent"); if (!hc) return -1;
        void *f = api_p_find_function(self, hc, "ServerSetInvicible"); unsigned char v = on != 0; void *args[1] = { &v };
        if (f) api_p_call_params(self, hc, f, args, NULL);
        r = tk_invincible(self, ch);
        if (r >= 0 && r != (on != 0) && tk_inv_offset_ok()) { *((uint8_t *)hc + TK_OFF_INVINCIBLE) = (uint8_t)(on != 0); r = *((uint8_t *)hc + TK_OFF_INVINCIBLE) != 0; }   // the RPC did not take effect: set the flag directly
        logf_("plugin %s: test_set_invincible(%p, %d) -> %d", tk_who(self), ch, on, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_set_invincible: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); r = -1; }
    return r;
}
// ----- location / teleport
static int api_p_test_get_location(const AbsApi *self, void *actor, float out[3]) {
    (void)self; if (!tk_ptr_ok(actor) || !out) return 0; int ok = 0;
    __try { void *root = *(void **)((uint8_t *)actor + OFF_ACTOR_ROOTCOMP); if (tk_ptr_ok(root)) { memcpy(out, (uint8_t *)root + OFF_ROOT_C2W_POS, 12); ok = isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}
static int api_p_test_teleport(const AbsApi *self, void *actor, const float loc[3], const float *yaw_deg) {      // AActor::K2_TeleportTo(location, rotator): yaw NULL keeps the rotation
    if (!tk_gate(self, "test_teleport", actor) || !loc) return 0; int ok = 0;
    __try {
        void *cls = api_p_find_class(self, "Actor"), *fn = cls ? api_p_find_function_in_class(self, cls, "K2_TeleportTo") : NULL; if (!fn) return 0;
        float rot[3] = { 0, 0, 0 };
        if (yaw_deg) rot[1] = *yaw_deg;
        else { float *r = NULL; void *root = *(void **)((uint8_t *)actor + OFF_ACTOR_ROOTCOMP); (void)r; if (tk_ptr_ok(root)) { const float *q = (const float *)((uint8_t *)root + OFF_ROOT_C2W_POS - 16); rot[1] = atan2f(2.f * (q[3] * q[2] + q[0] * q[1]), 1.f - 2.f * (q[1] * q[1] + q[2] * q[2])) * 57.29578f; } }
        float l[3] = { loc[0], loc[1], loc[2] }; unsigned char ret = 0; void *args[2] = { l, rot };
        ok = api_p_call_params(self, actor, fn, args, &ret) && ret;
        if (!ok) {                                                                            // K2_TeleportTo refuses a spot another capsule blocks: set it directly as a teleport (no sweep, no overlap test)
            void *f2 = api_p_find_function_in_class(self, cls, "K2_SetActorLocationAndRotation"); unsigned char sweep = 0, tele = 1, r2 = 0; uint8_t hr[0x90]; memset(hr, 0, sizeof hr); void *a2[5] = { l, rot, &sweep, hr, &tele };
            ok = f2 && api_p_call_params(self, actor, f2, a2, &r2) && r2; if (ok) logf_("plugin %s: test_teleport: K2_TeleportTo was blocked, used K2_SetActorLocationAndRotation", tk_who(self));
        }
        if (ok && api_p_is_a(self, actor, "FightingCharacter")) { void *nt = api_p_find_function(self, actor, "BPF_NotifyTeleport"); if (nt) api_p_call_params(self, actor, nt, NULL, NULL); }      // what the stock training flow calls after TeleportTo; without it driven attacks do not land
        logf_("plugin %s: test_teleport(%p, %.0f %.0f %.0f, yaw %.0f) -> %d", tk_who(self), actor, l[0], l[1], l[2], rot[1], ok);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_teleport: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); ok = 0; }
    return ok;
}
// ----- attack row animation in memory: OrderAttack::OnStart only RESOLVES the row's m_Anim soft pointer (no load) and stops the order at once when the animation is not in memory
// The game loads the animations of the rows in the active deck; mod rows and forced test plays outside it need this. The animation is rooted (GC keeps it).
// Game thread. Returns 1 loaded now, 0 was in memory already (rooted now), -1 no table / row / animation or the load failed, -2 not in memory and load = 0; path gets the soft path ("" if none).
static int se_row_anim_ensure(const AbsApi *self, const char *table_path, const char *row, int load, char *path, int cap) {
    path[0] = 0; int r = -1;
    __try {
        wchar_t w[100]; void *t = se_table(table_path), *rs = t ? *(void **)((uint8_t *)t + OFF_DT_ROWSTRUCT) : NULL; if (!rs) return -1;
        u8_to_w(row, w, 100); uint8_t *rp = table_row(t, fname_of(w)); uint8_t *prop = rp ? struct_prop(rs, fname_of(L"m_Anim")) : NULL;
        if (!prop || strcmp(prop_kind(prop), "SoftObject")) return -1;
        fname_text(*(FNameRaw *)(rp + *(int *)(prop + 0x44) + 16), path, cap);          // TSoftObjectPtr: FSoftObjectPath.AssetPathName at +16 (rowedit.inc soft_set)
        if (!tk_anim_path_ok(path)) return -1;
        void *o = api_p_find_object(self, path); r = o ? 0 : 1;
        if (!o && !load) return -2;
        if (!o) o = api_p_load_object(self, path);
        if (!o) return -1;
        se_root_flag(o, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: row %s animation: EXCEPTION 0x%08lX", tk_who(self), row, GetExceptionCode()); r = -1; }
    return r;
}
static int api_p_test_attack(const AbsApi *self, void *atk, const char *attack, void *target) {
    if (!tk_gate(self, "test_attack", atk)) return 0;
    int deck = attack && !strcmp(attack, "@deck");                                          // "@deck": the first attack of the character's own deck in its current stance, through the normal combo path
    if (!deck && !tk_attack_name_ok(attack)) { logf_("plugin %s: test_attack: '%s' is not an attack row name", tk_who(self), attack ? attack : "(null)"); return 0; }
    if (!tk_fn_ok(TKF_PREPARE) || !tk_fn_ok(TKF_DAACTOR)) return 0;
    int ok = 0;
    __try {
        uint8_t *ac = tk_comp(self, atk, "m_AttackComponent");
        if (!ac) { logf_("plugin %s: test_attack: %p has no attack component", tk_who(self), atk); return 0; }
        AbsCharInfo ci; memset(&ci, 0, sizeof ci); ci.size = sizeof ci; api_p_char_info(self, atk, &ci);
        uint64_t name = 0; int quad = 0;
        if (deck) {                                                                           // BPF_GetComboAttack(family, quadrant, 0) -> the FName the combo manager expects next
            uint8_t *fam = api_p_prop_ptr(self, ac, "m_eActiveWeapon"); void *f = api_p_find_function(self, ac, "BPF_GetComboAttack");
            quad = tk_quadrant_pick(5, ci.stance); unsigned char q = (unsigned char)quad, fm = fam ? *fam : 0, mir = 0; int idx = 0; uint64_t nm = 0; void *args[5] = { &fm, &q, &idx, &nm, &mir };
            if (!f || !api_p_call_params(self, ac, f, args, NULL) || !nm) { logf_("plugin %s: test_attack(@deck): the deck has no attack in quadrant %d", tk_who(self), quad); return 0; }
            name = nm; char nn[64]; fname_text(*(FNameRaw *)&name, nn, sizeof nn); logf_("plugin %s: test_attack(@deck): family %d quadrant %d -> row %s (mirror %d)", tk_who(self), fm, quad, nn, mir);
        } else {
            void **tbl = api_p_prop_ptr(self, ac, "m_AnimDBXls"); char tp[300] = "";
            if (tbl && tk_ptr_ok(*tbl)) se_path_of(*tbl, tp, sizeof tp);
            uint8_t *row = tp[0] ? api_p_table_row(self, tp, attack) : NULL;
            if (!row) { logf_("plugin %s: test_attack: row '%s' is not in the attacks table (%s) - not launched", tk_who(self), attack, tp[0] ? tp : "no table"); return 0; }
            quad = tk_quadrant_pick(row[0xD0], ci.stance);                                    // AttackDataRow::m_eStartQuadrant
            wchar_t w[40]; u8_to_w(attack, w, 40); FNameRaw fn = fname_of(w); name = *(uint64_t *)&fn;
        }
        {   // the row's animation must be in memory or the order stops at once (rows outside the loaded decks): forced path loads + roots it first; @deck only reports (it tests the game's own loading)
            void **tbl = api_p_prop_ptr(self, ac, "m_AnimDBXls"); char tp[300] = "", rn[64], ap[300];
            if (tbl && tk_ptr_ok(*tbl)) se_path_of(*tbl, tp, sizeof tp);
            fname_text(*(FNameRaw *)&name, rn, sizeof rn);
            int la = tp[0] ? se_row_anim_ensure(self, tp, rn, !deck, ap, sizeof ap) : -1;
            if (la == 1) logf_("plugin %s: test_attack: loaded %s (row %s, rooted)", tk_who(self), ap, rn);
            else if (la == 0) logf_("plugin %s: test_attack: row %s animation in memory (%s)", tk_who(self), rn, ap);
            else if (la == -2) logf_("plugin %s: test_attack(@deck): row %s animation NOT in memory (%s) - the attack will stop at once", tk_who(self), rn, ap);
            else logf_("plugin %s: test_attack: row %s animation '%s' could not be loaded - the attack may stop at once", tk_who(self), rn, ap);
        }
        if (tk_ptr_ok(target)) { api_weak_set((WeakPtr *)(ac + TK_OFF_ATK_TARGET), target); api_weak_set((WeakPtr *)(ac + TK_OFF_ATK_NEXT), target); }
        uint8_t tf[0x60], daa[TK_DAA_SIZE + 0x20]; memset(tf, 0, sizeof tf); memset(daa, 0, sizeof daa);        // an empty TFunction (no storage): AllocatedSize = 0 at +0x40
        ((void *(*)(void *, uint8_t, void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint64_t, uint8_t, uint8_t))(g_base + g_tkf[TKF_DAACTOR].rva))(daa, 0, tf, 0 /* Attack1 */, (uint8_t)quad, 0, 0, 0, name, 0, 5);
        if (!tk_daa_check(daa, 0, quad, name)) { logf_("plugin %s: test_attack: the DelayedActionAttack layout is not what testkit.h expects - not launched", tk_who(self)); return 0; }
        daa[TK_DAA_FORCE] = deck ? 0 : 1;                                                     // m_bForceDebugPlay: skip the combo deck, play this row
        logf_("plugin %s: test_attack(%p, row %s, quadrant %d, target %p, %s)", tk_who(self), atk, attack, quad, target, deck ? "deck path" : "forced path");
        ((void (*)(void *, void *))(g_base + g_tkf[TKF_PREPARE].rva))(ac, daa);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_attack: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); ok = 0; }
    return ok;
}
// ----- test_guard: UDefenseComponent::BPF_SetIsGuarding then AFightingCharacter::HandleGuard(on, cancel, prediction) (what the AI's parry/guard tasks and the player's input end up in)
static int api_p_test_guard(const AbsApi *self, void *ch, int on) {
    if (!tk_gate(self, "test_guard", ch) || !tk_fn_ok(TKF_GUARD)) return 0; int ok = 0;
    __try {
        void *dc = tk_comp(self, ch, "m_DefenseComponent"); if (!dc) { logf_("plugin %s: test_guard: no defense component", tk_who(self)); return 0; }
        void *f = api_p_find_function(self, dc, "BPF_SetIsGuarding"); unsigned char v = on != 0, pred = 0; void *args[2] = { &v, &pred };
        if (f) api_p_call_params(self, dc, f, args, NULL);
        ((void (*)(void *, uint8_t, uint8_t, uint8_t))(g_base + g_tkf[TKF_GUARD].rva))(ch, (uint8_t)(on != 0), 0, 0);
        ok = 1; logf_("plugin %s: test_guard(%p, %d)", tk_who(self), ch, on);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_guard: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); ok = 0; }
    return ok;
}
// ----- test_spawn_npc: npc_spawn (npcs.inc): the loader's spawn chain with the spawner placed AT loc before BPF_WantsSpawn (NULL = 350 uu ahead). The pawn is never teleported afterwards:
// an AI character that was teleported after its spawn never moves again.
static void *api_p_npc_spawn(const AbsApi *self, const char *arch, const float loc[3], const float *yaw, const char *cls);     // npcs.inc
static void *api_p_test_spawn_npc(const AbsApi *self, const char *archetype, const float loc[3]) {
    if (!archetype || archetype[0] != '/') { logf_("plugin %s: test_spawn_npc: needs an archetype asset path like /Game/DB/AI/NPCs/Trickster/Trickster.Trickster", tk_who(self)); return NULL; }
    void *made = api_p_npc_spawn(self, archetype, loc, NULL, NULL);
    logf_("plugin %s: test_spawn_npc(%s) -> %s", tk_who(self), archetype, made ? "a new character" : "NOTHING");
    return made;
}

// ----- test_dodge: what UDodgeBTTask::StartDodge does: PrepareDodge(character, dir, &angle) turns a world direction into the dodge angle, a DelayedActionDodge
// {vtable, +0x10 type 1, +0x20 empty TFunction, +0x70 direction x y, +0x78 z, +0x7C angle} goes to AFightingCharacter::PlayDodgeOrder. The vtable is checked before it is used.
static int api_p_test_dodge(const AbsApi *self, void *ch, const float dir[3]) {
    if (!tk_gate(self, "test_dodge", ch) || !dir || !tk_fn_ok(TKF_PLAYDODGE) || !tk_fn_ok(TKF_PREPDODGE) || !tk_fn_ok(TKF_DACTOR)) return 0; int ok = 0;
    __try {
        void **vft = (void **)(g_base + TK_RVA_VFT_DODGE);
        if (vft[0] != (void *)(g_base + 0x2E88A0u)) { logf_("plugin %s: test_dodge: the DelayedActionDodge vtable is not where this build has it - not launched", tk_who(self)); return 0; }
        float d[3] = { dir[0], dir[1], 0.0f }, ang = 0.0f;
        ((void (*)(void *, float *, float *))(g_base + g_tkf[TKF_PREPDODGE].rva))(ch, d, &ang);
        uint8_t tf[0x60], obj[0xA0]; memset(tf, 0, sizeof tf); memset(obj, 0, sizeof obj);
        ((void *(*)(void *, uint8_t, void *))(g_base + g_tkf[TKF_DACTOR].rva))(obj, 1, tf);          // DelayedAction base: type 1, empty callback
        *(void **)obj = vft; memcpy(obj + 0x70, d, 8); *(float *)(obj + 0x78) = d[2]; *(float *)(obj + 0x7C) = ang;
        uint8_t r = ((uint8_t (*)(void *, void *))(g_base + g_tkf[TKF_PLAYDODGE].rva))(ch, obj);
        ok = r != 0; logf_("plugin %s: test_dodge(%p, dir %.2f %.2f, angle %.1f) -> %d", tk_who(self), ch, d[0], d[1], ang, (int)r);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_dodge: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); ok = 0; }
    return ok;
}

// ----- test_ai: the AI controller's behaviour tree component (FightingAIController::m_BehaviorComp; BrainComponent of the AIController base class is null for these NPCs): StopLogic("testkit") / RestartLogic. Without its tree an NPC does nothing by itself (no attacks, no guard, no movement).
static int api_p_test_ai(const AbsApi *self, void *ch, int enabled) {
    if (!tk_gate(self, "test_ai", ch)) return 0; int ok = 0;
    __try {
        void *ctl = tk_comp(self, ch, "Controller"); void **bp = ctl ? api_p_prop_ptr(self, ctl, "BrainComponent") : NULL; void *brain = bp && tk_ptr_ok(*bp) ? *bp : NULL;
        if (!brain && ctl) { void **mp = api_p_prop_ptr(self, ctl, "m_BehaviorComp"); if (mp && tk_ptr_ok(*mp)) brain = *mp; }
        if (!brain) { char cn[100] = "-"; if (ctl) se_name_of(*(void **)((uint8_t *)ctl + 0x10), cn, sizeof cn); logf_("plugin %s: test_ai(%p): no AI brain (controller %p of class %s, BrainComponent slot %p -> %p)", tk_who(self), ch, ctl, cn, (void *)bp, bp ? *bp : NULL); return 0; }
        if (enabled) { void *f = api_p_find_function(self, brain, "RestartLogic"); ok = f && api_p_call_params(self, brain, f, NULL, NULL); }
        else { void *f = api_p_find_function(self, brain, "StopLogic"); wchar_t why[] = L"testkit"; SeFString fs = { why, 8, 8 }; void *args[1] = { &fs }; ok = f && api_p_call_params(self, brain, f, args, NULL); }
        logf_("plugin %s: test_ai(%p, %s) -> %d", tk_who(self), ch, enabled ? "restart" : "stop", ok);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_ai: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); ok = 0; }
    return ok;
}

// ----- test_hittable: an AI character is only hit while UAIComponent+0x140 (a byte) is nonzero: the CanBeHitted lambda AAISpawner::SpawnAI adds (TFunctionRefCaller<lambda_6ec60eb4...>::Call at base+0x2C2670) returns
// !IsValid(weak ai component) || ai->byte[+0x140] != 0. Stopping the behaviour tree clears it (the practice dummy never has it set), so driven attacks would never land. The offset is confirmed from that
// function's own bytes (cmp byte ptr [rax+0x140], 0 at +0x1A) before it is written.
static int api_p_test_hittable(const AbsApi *self, void *ch, int on) {
    if (!tk_gate(self, "test_hittable", ch)) return -1; int r = -1;
    __try {
        static const uint8_t cmp[7] = { 0x80, 0xB8, 0x40, 0x01, 0x00, 0x00, 0x00 };            // cmp byte ptr [rax + 0x140], 0
        if (memcmp(g_base + 0x2C268Au, cmp, 7) != 0 || g_base[0x2C2670] != 0x40 || g_base[0x2C2671] != 0x53) { logf_("plugin %s: test_hittable: the CanBeHitted lambda is not what this build has - not changed", tk_who(self)); return -1; }
        void **aic = api_p_prop_ptr(self, ch, "m_AIComponent"); if (!aic || !tk_ptr_ok(*aic)) { logf_("plugin %s: test_hittable(%p): not an AI character", tk_who(self), ch); return -1; }
        uint8_t *b = (uint8_t *)*aic + 0x140; uint8_t before = *b; *b = on ? 1 : 0; r = *b;
        logf_("plugin %s: test_hittable(%p, %d): AI component flag %d -> %d", tk_who(self), ch, on, before, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: test_hittable: EXCEPTION 0x%08lX", tk_who(self), GetExceptionCode()); r = -1; }
    return r;
}

static int api_p_test_get_forward(const AbsApi *self, void *actor, float out[3]) {
    (void)self; if (!tk_ptr_ok(actor) || !out) return 0; int ok = 0;
    __try { void *root = *(void **)((uint8_t *)actor + OFF_ACTOR_ROOTCOMP); if (tk_ptr_ok(root)) { const float *q = (const float *)((uint8_t *)root + OFF_ROOT_C2W_POS - 16);       // FQuat x y z w
                out[0] = 1.f - 2.f * (q[1] * q[1] + q[2] * q[2]); out[1] = 2.f * (q[0] * q[1] + q[3] * q[2]); out[2] = 2.f * (q[0] * q[2] - q[3] * q[1]); ok = isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

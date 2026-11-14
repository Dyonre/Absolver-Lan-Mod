// animstyle.h - pure parts of the weapon animation set ("anim style", API 2.17, animstyle.inc): which callers of AFightingCharacter::BPF_GetCombatStyle
// are animation lookups, and the per-character override table. No OS calls: tested offline (test\animstyle_test.c).
//
// Every style-indexed animation table is read with a style ARGUMENT that its caller gets from BPF_GetCombatStyle (0x322E00); the three inlined reads
// (Disarm, InitStyle, ServerUpdateMovementAnimation_Implementation) all end in UFightingMovementComponent::ReplaceMovementAnimation (0x448C60).
// The getter detour answers the override only to the callers below of kind AS_ANIM (identified by the return address); everything else (stats,
// ability, save, menus, the style-unlock Lock / Unlock that write the style back, AI services, unknown callers) gets the real style.
#ifndef ANIMSTYLE_H
#define ANIMSTYLE_H
#include <stdint.h>

enum { AS_KEEP = 0, AS_ANIM = 1 };                     // caller kinds: keep the real style / animation lookup (answered the override)
typedef struct { const char *name; uint32_t rva, size; int kind; } AsCaller;

// RVA + size from the PDB; animstyle.inc checks every name it can find in symbols.bin against the RVA and refuses to
// install the hooks on a mismatch (another game build). Data only: add a caller here (and to the test) when a new lookup shows up as "unknown caller".
static const AsCaller AS_CALLERS[] = {
    { "UPlayerAnim::UpdateDrunkenAlphas",                       0x311FB0,  354, AS_ANIM },   // Stagger (style 4) body layer
    { "UPlayerAnim::UpdateFidgets",                             0x312120,  539, AS_ANIM },
    { "UPlayerAnim::UpdateLookAt",                              0x312860, 3465, AS_ANIM },   // Lost Prospect (style 2) look-at special case
    { "UPlayerAnim::UpdateMoveStatusAnimation",                 0x3135F0, 1799, AS_ANIM },
    { "UAttackComponent::LaunchCancelAttack",                   0x302090, 1077, AS_ANIM },
    { "UDefenseComponent::HandleParried",                       0x335860, 1537, AS_ANIM },
    { "AFightingCharacter::SwitchMovementAnimations",           0x3503E0,  207, AS_ANIM },
    { "AFightingCharacter::AbortPickWeaponAttempt",             0x35AB60,  331, AS_ANIM },
    { "AFightingCharacter::HandleGuard",                        0x3644A0, 1251, AS_ANIM },   // guard idle / prep: the style travels in the guard order payload
    { "APIdleSwitch::OnProcess",                                0x36F670, 1459, AS_ANIM },
    { "AFightingCharacter::PickWeapon",                         0x3749F0,  904, AS_ANIM },
    { "UFightingMovementComponent::LaunchChangeQuadrant",       0x43F4A0,  856, AS_ANIM },   // stance change
    { "UFightingMovementComponent::NewWantedMoveTransition",    0x440130, 2062, AS_ANIM },
    { "OrderCancel::OnStart",                                   0x46A370, 1737, AS_ANIM },
    { "OrderChangeQuadrant::OnStart",                           0x46AA40, 1286, AS_ANIM },
    { "OrderDodge::OnStart",                                    0x46AF50, 2120, AS_ANIM },
    { "OrderHitted::OnStart",                                   0x46B850, 5919, AS_ANIM },   // hit reactions
    { "OrderWeaponAction::OnStart",                             0x46E5C0,  852, AS_KEEP },   // draw / sheathe / pick up / drop: the player's own clips both ways (drawn-only rule)
    { "OrderWeaponAction::OnStop",                              0x470150,  519, AS_KEEP },
    { "UCharacterProgressionUnlockSchoolFightingStyleDB::Lock", 0x366F10,  214, AS_KEEP },   // reads AND writes the style back: must never see an override
    { "UCharacterProgressionUnlockSchoolFightingStyleDB::Unlock", 0x37B560, 205, AS_KEEP },
    { "UStatsComponent::BPF_IsSchoolRewardEquipped",            0x4C2CD0,  474, AS_KEEP },
    { "USocialComponent::InitFromSave",                         0x4D0230,  783, AS_KEEP },
    { "AFightingPlayerController::SendTrackingCharacterSave",   0x3A2FD0, 1920, AS_KEEP },
    { "UIdelSwitchBTTask::ExecuteTask",                         0x2F3970,  936, AS_KEEP },   // AI behaviour tree
    { "AFightingCharacter::execBPF_GetCombatStyle",             0x5A8B90,  128, AS_KEEP },   // Blueprint thunk (menus, camera)
};
#define AS_NCALLERS ((int)(sizeof AS_CALLERS / sizeof AS_CALLERS[0]))

// index of the caller whose code range [rva, rva + size) holds `ra_rva` (a return address minus the game base), -1 = unknown
static int as_caller_of(const AsCaller *t, int n, uint32_t ra_rva) { for (int i = 0; i < n; i++) if (ra_rva - t[i].rva < t[i].size) return i; return -1; }

// per-character overrides. ch NULL = "the local player" (resolved per call against the current local pawn, so a world change keeps it)
// proxy = the style answered to a dwmapi.dll proxy mod in the game folder that calls the getter from its own code (e.g. a deflect keyed on a style id), -1 = the real style
typedef struct { void *volatile ch; volatile int style, proxy, used; } AsOvr;
#define AS_MAX 16

// the entry that applies to `ch` (an explicit one first, then the NULL entry when ch is the local pawn), NULL = none
static const AsOvr *as_lookup(const AsOvr *t, const void *ch, const void *local) {
    if (!ch) return NULL;
    for (int i = 0; i < AS_MAX; i++) if (t[i].used && t[i].ch == ch) return &t[i];
    if (ch == local) for (int i = 0; i < AS_MAX; i++) if (t[i].used && !t[i].ch) return &t[i];
    return NULL;
}
// set (style >= 0) or remove (style < 0) the entry of ch; returns the previous style (-1 = none), -2 = table full / bad style
static int as_set(AsOvr *t, void *ch, int style, int proxy) {
    if (style > 63 || proxy > 63) return -2;
    int free_i = -1;
    for (int i = 0; i < AS_MAX; i++) {
        if (t[i].used && t[i].ch == ch) { int prev = t[i].style; if (style < 0) t[i].used = 0; else { t[i].style = style; t[i].proxy = proxy < 0 ? -1 : proxy; } return prev; }
        if (!t[i].used && free_i < 0) free_i = i;
    }
    if (style < 0) return -1;
    if (free_i < 0) return -2;
    t[free_i].style = style; t[free_i].proxy = proxy < 0 ? -1 : proxy; t[free_i].ch = ch; t[free_i].used = 1;
    return -1;
}
static int as_count(const AsOvr *t) { int n = 0; for (int i = 0; i < AS_MAX; i++) n += t[i].used != 0; return n; }
// drop the explicit (non-NULL) entries, e.g. on a world change (the characters are gone; a new one may reuse the address); returns how many
static int as_drop_explicit(AsOvr *t) { int n = 0; for (int i = 0; i < AS_MAX; i++) if (t[i].used && t[i].ch) { t[i].used = 0; n++; } return n; }

// the answer of one getter call. real = the game's answer; o = as_lookup(); kind = caller kind (-1 unknown caller); proxy_caller = the return address is in the dwmapi.dll proxy mod
static int as_decide(int real, const AsOvr *o, int kind, int proxy_caller) {
    if (!o) return real;
    if (proxy_caller) return o->proxy >= 0 ? o->proxy : real;
    return kind == AS_ANIM ? o->style : real;
}
#endif

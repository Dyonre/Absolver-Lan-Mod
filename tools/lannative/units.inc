// ---------------------------------------------------------------------------------------------------------------------
// units.inc - the loader's units: one #include per line, included once by lannative.c (one translation unit).
//
// The order is the dependency order: a unit may use what the units above it define. Nothing else depends on the position:
// what a unit runs at start-up, per frame, on a page ... is registered in the unit itself (lnhooks.h) with an explicit
// order key. A new unit goes at the END of its list (it may then use everything) unless another unit must see its code.
// tools/lannative/README.md ("Units and registration") is the guide.
// ---------------------------------------------------------------------------------------------------------------------
#include "modglue.inc"
#include "crashrep.inc"
#include "lanmods.inc"
#include "landisc.inc"
#include "rowedit.inc"
#include "plugins.inc"                      // plugin loading and the base AbsApi
#include "secore.inc"                       // script extender core: ProcessEvent hooks, events, reflection, timers, the core AbsApi fields
#include "testkit.inc"                      // API 2.6 TestKit: fight tests without a keyboard
#include "texture.inc"                      // runtime textures
#include "combodeck.inc"                    // combo deck / combat style API
#include "sidecar.inc"                      // API 2.18 per-character metadata store (LanNative\state\<GUID>.txt), used by weaponslot / kindstate
#include "animstyle.inc"                    // API 2.17 weapon animation sets (another style's animations without a style change)
#include "modes.inc"                        // game mode API: travel, console, match state, banners
#include "mapscript.inc"                    // kind "maps": scripted scenes
#include "content.inc"                      // content registry, kinds "items" + "styles"; includes weapons.inc, weaponslot.inc (kindstate.inc), learn.inc and powers.inc where its item code needs them
#include "npcs.inc"                         // kind "npcs" and the NPC API (npc_spawn, on_area, ...)
#include "bots.inc"                         // kinds "bots" + "ranks" and the progression API (xp_add, rank_info, trial_reward, ...)
#include "bottrial.inc"                     // kind "bot_trials" and API 2.16 bot_trial_start / bot_trial_status; applies the "maps" kind
#include "hitpre.inc"                       // kind "poise" and on_hit_pre / damage_mod / poise_set (API 2.19)
#include "overlayapi.inc"                   // overlay_info / _register / _unregister / _set_mode / _raw_mouse: plugins draw Dear ImGui through the loader's overlay (API 2.21)
#include "defense_ev.inc"                   // on_defense (API 2.20): guard / guard break / absorb / parry / deflect / dodge / avoid events
#include "faejinesc.inc"                    // Faejin gold-loop brakes (window shrink, stamina escalation; default OFF, config fe_*)
#include "inputrec.inc"                     // input_rec: input + state recorder, default off
#include "botai.inc"                        // scripted bot, default off
#include "clone.inc"                        // kind "clones": player clones = second local players with their own character, driven by a botai brain
#include "animsets.inc"                     // kind "anim_sets": runtime animation sets (padded style tables + wired clips)
#include "hostmods.inc"                     // host mod download + max players
#include "netinspect.inc"                   // Net Inspector: F2 Mods page rows, host list over the host-mods side channel
#include "keys.inc"                         // key ownership: F2 "Keys" rows, API 2.23 declare_key / input_owner
#include "kick.inc"                         // host-only F1 "Kick player..." (AGameSession::KickPlayer; kick.h)
#include "uirows.inc"                       // API 2.27 ui_toggle / ui_number / ui_button / ui_hint / ui_set: loader-drawn plugin rows (docs/API-REFERENCE.md); before modsui.inc, which lists them
#include "modsui.inc"
#include "logwin.inc"                       // F2 row: the log window (lannative.log: filter, errors only, mod tags)
#include "profiles.inc"                     // mod profiles: Mods\profiles\<name>.txt, F2 rows Save as / Switch to
#include "travelx.inc"                      // API 2.25 add_travel_entry: plugin rows in the F1 Map list
#include "netrole.inc"                      // API 2.26 net_role (mod.json "net" / "joiners_need" are read in manifest.h)
#include "camclaim.inc"                     // API 2.28 camera_claim / camera_release / camera_holder: view-target ownership by priority
#include "gamestate.inc"                    // API 2.29 game_state flags + on_world_ready (no title / menu worlds)
#include "hudhide.inc"                      // API 2.30 hud_hide / hud_show: the game's HUD hidden without plugin object scans; uses camclaim.inc\'s ln_obj_alive
#include "modeclaim.inc"                    // API 2.31 mode_claim / mode_release / mode_holder: one owner per pawn, native movement / collision / pose off + exact restore; uses camclaim.inc\'s ln_obj_alive + cc_obj_prop
#include "posestage.inc"                    // API 2.32 pose_stage / pose_handoff: inertialized clip hand-offs in the loader's FinalizeBoneTransform detour; uses modeclaim.inc\'s mc_call, camclaim.inc's ln_obj_alive + cc_obj_prop
#if defined(__has_include)
#if __has_include("servermode.inc")
#include "servermode.inc"                    // the optional unattended-host module; without the file its hooks do not exist
#endif
#endif
#include "matchredir.inc"

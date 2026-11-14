# Plugin API (AbsolverSE, API version 2)

For things `rows.json` and paks cannot do: new game logic, UI changes, anything that needs code. A plugin is a 64-bit DLL named `plugin.dll` in a mod folder. The loader calls it with one struct, `AbsApi`, declared in `tools/lannative/AbsolverSE.h` (the only header you need). Offline / private play only: the loader does nothing without `-NoEAC`.

**Every function takes the `AbsApi` pointer you were given as its first argument (`self`).** The loader uses it to know which plugin is calling, also from your own hook detours and your own threads. `ABS_API_VERSION` is 2; new functions are only ever appended to the end of the struct, so check `api->size` before using a field newer than the version you built against (table in "API versions" below). Plugins built against API version 1 refuse to load: rebuild them against this header.

Many features also exist as **content kinds** (JSON in `Mods\<mod>\content\<kind>\`, no code): items, styles, powers, NPCs, bots, ranks, bot trials, map scripts, poise. Their full authoring guide is `docs/MODDING-GUIDE.md` (sections 10b-10f, 14, 15); this document lists the fields again where a plugin author needs them.

## Quick start

`examples/PluginTemplate` is the smallest plugin; `examples/HelloPlugin` shows more.

1. Build: `build.bat` in the example (MSVC Build Tools 2022, x64) makes `plugin.dll`.
2. Copy it into `<game folder>\Mods\<Name>\` and start the game once: the loader adds `#<Name>` to `Mods\plugins.txt` and does not load it.
3. Remove the `#`, start again. `Binaries\Win64\LanNative\lannative.log` shows `plugin <Name>: ...` lines.

```c
#include <windows.h>
#include <stddef.h>
#include "AbsolverSE.h"
static const AbsApi *A;
#define HAS(f) (A->size >= offsetof(AbsApi, f) + sizeof(void *))   /* newer fields: check first */
static void tick(float dt, void *u) { /* game thread, every frame */ }
__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    if (api->version != ABS_API_VERSION) return 1;   /* 0 = ok, anything else switches the plugin off */
    A = api; A->log(A, "hello");
    A->on_tick(A, tick, NULL);
    if (HAS(on_hit)) { /* API 2.3 or newer */ }
    return 0;
}
```

Optionally the DLL also exports `void AbsPlugin_Unload(void)`, called at game exit. Keep your copy of `api` for the whole game; the struct stays valid. Strings on this interface are UTF-8; property values use the engine's own text format (the format of `rows.json` values and the SDK dump).

## What you get

| Area | Functions |
|---|---|
| Header fields | `size`, `version`, `game_base` (base address of the game exe), `plugin_name` (your mod folder's name), `plugin_dir` (absolute path, no trailing slash) |
| Log / settings | `log` (printf-style, into `lannative.log` tagged with your name), `config_get` (`plugin.cfg` in your mod folder: `key = value`) |
| Events | `on_tick` (game thread), `on_engine_ready` (once, at the first game tick; immediately if that already happened), `on_object_loaded` (ANY thread), `run_on_game_thread`, `is_game_thread`, `set_timer`, `on_world_loaded(map path)`, `on_key`, `on_damage`; combat and menu events from API 2.3 |
| Hooks | `hook_rva(name, rva, expected_bytes, stolen, detour)`, `hook_symbol(name, detour)` (PDB name from `symbols.bin`), `find_symbol` |
| Calls by name | `on_call(class, function, pre, post, user)`, `replace_native(class, function, thunk, &orig)`, `trace`, `trace_function`, `frame_peek_int` |
| Reflection | `find_object`, `load_object`, `find_class`, `find_function_in_class`, `find_cdo`, `object_class`, `object_name`, `object_path`, `class_name`, `is_a`, `find_property`, `property_offset`, `prop_ptr`, `prop_ptr_path`, `get_property_text`, `set_property_text`, `get_bool`, `set_bool`, `find_function`, `call_function` (raw parameter block), `call_function_params` (builds the block from the function's own parameter list), `for_each_object`, `dump_object` |
| Creation | `construct_object`, `duplicate_object`, `spawn_actor`, `create_widget`, `add_to_viewport`, `remove_from_parent`, `get_world`, `get_local_player_controller` |
| Text and names | `make_text`, `text_to_string`, `set_text`, `fname_make`, `fname_text` |
| Plugin data | `kv_get` / `kv_set` (per plugin), `sidecar_get` / `sidecar_set` (per character) |
| Development | `dump_sdk(name)` -> `LanNative\sdk\<name>.txt` (every class / struct / enum / function with offsets, parameter lists and native addresses, Blueprint classes included), the ProcessEvent tracer |

Class and function names are the engine's SHORT names without the `U`/`A` prefix, exactly as the SDK dump lists them (`StatsComponent`, `BP_CharacterMenu_C`, `BPF_IsFightingStyleUnlocked`).

### Hooks stack
Several plugins (and the loader's own hooks) can hook the same function. The first hook patches the game code; later ones are stacked on top, and the pointer `hook_rva` / `hook_symbol` returns is "the next function in the chain" (the previously installed detour, finally the game's own code). Always call it unless you mean to replace the function. `expected` is always the game's ORIGINAL bytes (for `hook_symbol` the loader knows them); `stolen` is the number of whole-instruction bytes to move (14..24). A hook returns NULL and changes nothing when the live bytes differ. Installation suspends the other threads for an instant, so it is safe at any time; still, prefer `AbsPlugin_Load`.

### Properties and parameters
Properties are read and written either as engine text (`get_property_text` / `set_property_text`; `"name[N]"` = element N of a static array) or through `prop_ptr` (you cast; you know the type from the SDK dump). `prop_ptr_path(obj, "a.b[2].c")` follows structs, object pointers, static arrays and TArray elements (for example `OwningGameInstance.m_CurrentSave.m_SavedData.m_CurrentComboSaveName[1]` from the world); it returns NULL (logged) when a step is missing. `call_function_params(obj, func, args, ret)` takes `args[i]` = pointer to the i-th non-return parameter in declaration order; out parameters are copied back; `ret` receives the return value (may be NULL). Game thread only.

### on_call
`pre(obj, func, params, user)` is called before the function and may change `*params`; return nonzero to SKIP the original (then `post` is not called). `post` follows the original. `cls` may be NULL (any class). Matching is by function name on one loader hook on `UObject::ProcessEvent`; every registration costs a compare per call, so keep callbacks short. Only Blueprint-visible calls pass through here (events, `BPF_` functions called from Blueprints); direct C++ calls do not.

### replace_native
`thunk(void *context, void *frame, void *result)` replaces the native exec function of a UFunction (the pointer the Blueprint VM calls; no byte patching). Call `orig(context, frame, result)` to keep the original behaviour. It works for the game's native `BPF_*` functions but does NOT see native-to-native C++ calls: use `hook_symbol` on the C++ function for those. Inside a thunk, `frame_peek_int(frame, &v)` reads the call's first (int-like) argument without consuming it.

### More helpers
* `on_key(vk, cb, user)`: game-thread callback on each key-down edge of virtual key `vk` while the game window has focus (polled per tick; taps shorter than a frame can be missed).
* `on_damage(cb(victim, amount, flag, user), user)`: after `AFightingCharacter::ApplyDamage(amount, flag)` on the victim. The attacker is not available here; use `on_hit` for that.
* `kv_get(key, out, cap)` / `kv_set(key, value)` (NULL removes): per-plugin store `LanNative\data\<plugin>.txt`. Keep mod data here, never in the game's save format.
* `make_text(utf8, out24)` / `text_to_string(text24, out, cap)`: build / read an FText (24 bytes). Copy it into a Text property with `memcpy(prop_ptr(...), out, 24)`; `set_text(obj, prop, utf8)` does both (the old text is not released, a small leak; culture-invariant, one language).
* `fname_make(utf8, &out)` / `fname_text(fname, out, cap)`: FName from and to a string.
* `object_path`, `dump_object(obj)` (every property as engine text into `LanNative\objdump_<name>.txt`), `duplicate_object(src, outer)`, `find_cdo(class)` (the `Default__Name` object).
* `create_widget(class, owning_player)`, `add_to_viewport(widget, zorder)`, `remove_from_parent(widget)`.
* `set_timer(seconds, repeat, cb, user)`: game-thread timer (see "Timers with ids" for the table and `cancel_timer`).
* `on_world_loaded(cb(map_path, user), user)`: a new world appeared (map travel); `map_path` like `/Game/Maps/.../Name.Name`. It fires for a new world object or a new game state, so it also fires when the engine reuses the previous world's address (log `events: world N <path> (world ..., game state ...)`).

### API versions
`ABS_API_VERSION` stays 2; the minor versions below only appended functions. Check the LAST function you use: `api->size >= offsetof(AbsApi, f) + sizeof(void *)` (the `HAS(f)` macro above).

| Since | Functions |
|---|---|
| 2.1 | `hook_symbol` ... `kv_set` (script extender functions, `on_key`, `on_damage`, `kv_*`, text helpers) |
| 2.2 | items foundation: `create_asset` ... `on_save_written` |
| 2.3 | events: `on_hit`, `on_attack_start`, `on_pawn_spawned`, `on_pawn_destroyed`, `on_death`, `on_menu`, `char_info` |
| 2.4 | `register_content_kind` |
| 2.5 | `create_texture`, `texture_from_file` |
| 2.6 | `test_heal`, `test_set_invincible`, `test_attack`, `test_guard`, `test_teleport`, `test_get_location`, `test_spawn_npc`; `AbsCharInfo` fields `invincible`, `is_down`, `guarding`, `attacking` |
| 2.7 | `add_menu_item`, `reload_content` |
| 2.8 | `test_dodge`, `test_ai`, `test_hittable`, `test_get_forward` |
| 2.9 | powers: `inventory_equip_slot`, `on_power_used`, `power_use` |
| 2.10 | decks and styles: `combo_slot_ensure` ... `fname_text` |
| 2.11 | `set_timer_id`, `cancel_timer` |
| 2.12 | modes: `travel` ... `hud_score` |
| 2.13 | `mapscript_phase`, `mapscript_mesh` |
| 2.14 | NPCs: `npc_spawn` ... `show_message` |
| 2.15 | bots and ranks: `xp_add` ... `npc_difficulty` |
| 2.16 | `bot_trial_start`, `bot_trial_status` |
| 2.17 | animation style: `anim_style_set`, `anim_style`, `real_style`, `on_handled_weapon` |
| 2.18 | `sidecar_get`, `sidecar_set` |
| 2.19 | `on_hit_pre`, `damage_mod`, `poise_set`, `poise_get` |
| 2.20 | `on_defense` |
| 2.21 | overlay: `overlay_info`, `overlay_register`, `overlay_unregister`, `overlay_set_mode`, `overlay_raw_mouse` |

Structs passed to the API (`AbsCharInfo`, `AbsMatchInfo`, `AbsRankInfo`, `AbsTrialReward`, `AbsTrialStatus`) start with a `size` field: set `out->size = sizeof *out` before the call, so older and newer loaders both work. `mod.json` `api` is an integer (2): it cannot ask for a minor version, so check `api->size` in the plugin.

## Tools and diagnostics

| Tool | What |
|---|---|
| `tools/lannative/gen_symbols.py` | makes `symbols.bin` (about 50k hookable PDB symbols with original bytes and stolen length) from the output of `tools/symtool` `dumpall`. Put it in `LanNative\`. `hook_symbol`, `find_symbol` and several loader features need it. Game derived: keep it local |
| `tools/symtool` | `find`, `dumpall`: addresses for hooks from the game's PDB |
| Safe mode | hold Shift while starting, `-SESafe`, or `LanNative\safe.txt`: no mods, no plugins, no extender hooks. Two starts that never reach the first tick turn it on automatically; one clean start turns it off again |
| Crash watch | the loader logs the first 40 access-violation-class exceptions with module + offset (a plugin DLL is named). Many are handled; the last ones before a crash show the culprit |

## Safety and rules

* **Consent.** A plugin only loads if its folder name appears in `Mods\plugins.txt` without a `#`, and the mod is enabled in `loadorder.txt`. A plugin is arbitrary code: only install plugins you trust.
* **Guarded.** Callbacks run inside exception guards. After 3 crashes in a plugin its callbacks are switched off and the log (and the F2 Mods page) says which plugin. Code inside a hook detour is called from game code and is not guarded: keep detours tiny and correct.
* **Build check.** Every address is for Absolver 1.31 (b1.25_575). Hooks compare the original bytes before patching; the API's own engine calls are checked at load (if one differs, plugins are off).
* **Threads.** `on_object_loaded` is called on loader threads: return fast, never call `call_function*`, `load_object`, `for_each_object` or `dump_sdk` from it; queue work with `run_on_game_thread`, which returns 0 when the job was queued and 1 when it was not (no function given, or the queue of 128 pending jobs is full); check the result and retry on a later tick. Most functions added since API 2.2 need the game thread.
* **Never** hide from or disable EasyAntiCheat; the loader does nothing without `-NoEAC`.
* **Saves.** Keep mod data in `kv_set` / the sidecar, never in the game's save format. Back up `SaveGames` before trying a new plugin.
* Game derived output (`symbols.bin`, `LanNative\sdk\*`, `LanNative\trace\*`, `LanNative\objdump_*`) must never be committed or shared.

## Finding things
* The SDK dump (`dump_sdk`) is the map: class sizes, `+offset type name` members, `func Name(params) -> ret native=+RVA`.
* The tracer shows what the game calls when you do something: `trace("on", "FightingStyleSelection,SetState")` (an empty list = everything), do the thing, `trace("dump")`, read `LanNative\trace\trace_NNN.txt`. `trace("off")` / `trace("clear")` stop and empty it. `trace_function(class, function)` also records Blueprint-to-Blueprint and `BPF_` calls that skip ProcessEvent.
* Addresses for hooks: `tools/symtool` (`find`, `dumpall`) or your own disassembler on the game's PDB.

## Events (API 2.3)
Every callback is called on the game thread; the hook behind an event is installed the first time any plugin registers it (shared by all plugins). The loader logs the first 150 events with the prefix `events:`.

| Function | When / what you get |
|---|---|
| `on_hit(cb(const AbsHitInfo *hit, user), user)` | after the game resolved an impact (the authority's hit resolution, which applies the damage): `victim`, `attacker` (may be NULL), `attack_id` (numeric row id, -1 for names like `BT2_01`), `attack` (row name), final `damage`, `outcome` (`ABS_HIT`, `ABS_HIT_GUARDED`, `ABS_HIT_GUARD_BROKEN`, `ABS_HIT_ABSORBED`, `ABS_HIT_NO_EFFECT`), `perfect_link`. Also once per successful parry or dodge with `ABS_HIT_PARRIED` (victim = the parrier, attacker = the parried attacker) or `ABS_HIT_DODGED` (victim = the dodging character), damage 0. One event per damage application (a 3-hit move gives 3 events) |
| `on_attack_start(cb(character, attack_id, attack, user), user)` | once per real attack (the game re-simulates attack starts many times for network prediction; the loader reports each attack once). Fires at the first (predicted) start, before any hit |
| `on_pawn_spawned(cb(character, user), user)` | after `AFightingCharacter::BeginPlay`: the player (title screen and world), every world NPC, the practice dummy. The controller is often not possessed yet (`char_info` `is_player` is 0 for the title-screen player) and values are still defaults |
| `on_pawn_destroyed(cb(character, reason, user), user)` | before `AFightingCharacter::EndPlay`; reason 0 destroyed, 1 level transition, 2 PIE end, 3 removed from world, 4 quit |
| `on_death(cb(victim, killer, attack_id, attack, user), user)` | `AFightingCharacter::ServerKilledBy`; `killer` may be NULL. It is called inside the hit resolution, so `on_death` comes BEFORE the `on_hit` of the killing blow. The practice dummy is invincible (`test_set_invincible(dummy, 0)` lets it die) |
| `on_menu(cb(menu, menu_class, state, user), user)` | after any `EnterState <Name>` / `EnterState_<Name>` event of a menu widget (the menus' Blueprint state machines; for example `BP_CharacterMenu_C` with `Stats`, `CombatDeckFamilies`). `state` = the name without the prefix |
| `char_info(character, AbsCharInfo *out)` | set `out->size = sizeof *out` first; 1 = ok. Fills the fields below |

`AbsCharInfo`: `name` (object name), `path`, `health` / `max_health` (`m_fMaxHealth + m_fBonusMaxHealth`), `guard` / `max_guard`, `stance` (0 FrontLeft, 1 FrontRight, 2 BackRight, 3 BackLeft, -1 unknown), `style` (0 Forsaken, 1 Windfall, 2 Lost Prospect, 3 Kahlt, 4 Stagger, 5 Faejin; game thread only, else -1), `is_player`, `is_dead` (health <= 0); since API 2.6 also `invincible` (-1 unknown), `is_down`, `guarding`, `attacking` (game thread only, else -1).

## Items foundation (API 2.2)
The functions behind the built-in `items` content kind, for plugins that make items themselves. All need the game thread, except `add_to_root` and `table_row`.

| Function | What |
|---|---|
| `void *create_asset(self, void *src, const char *path)` | copy of `src` (for example a stock DataAsset from `load_object`) as `"/Game/Mods/<Mod>/<Name>.<Name>"` in its own package, rooted (the garbage collector keeps it). Only `/Game/Mods/` paths. If `src` is a UClass a fresh instance is constructed. An existing object of that path with the same class is returned (idempotent); NULL otherwise. Create item objects at `on_engine_ready`, BEFORE the save is loaded: object references in the save are only looked up, never loaded |
| `int add_to_root(self, void *obj, int root)` | RootSet flag on (1) / off (0): keeps a runtime-created object alive. 1 = ok |
| `void *table_row(self, table_path, row)` | pointer to the row struct (cast with the SDK dump's offsets) or NULL; `table_path` like `/Game/DB/Shop/EquipmentShop.EquipmentShop` (must be loaded) |
| `int table_add_row(self, table_path, row, copy_of)` | the `rows.json` "add" at run time: clone row `copy_of` as `row`. 1 = ok or exists |
| `int row_set_text(self, table_path, row, prop, text)` | engine text into one row property, validated like `rows.json`. An object reference that is neither in memory nor in a pak is refused (loading it would hang the game); a `create_asset` object is accepted. 1 = ok |
| `int inventory_add(self, void *item, int material, int equip)` | adds `item` to the local player's inventory (`material` 0 = default). Returns the inventory index or -1; `equip` 1 also calls `inventory_equip`. Needs a world |
| `int inventory_equip(self, int index)` | equips inventory entry `index` through the inventory menu when one exists (equipped flags and preset), otherwise through the inventory component. 1 = ok. Cannot fill a power slot: use `inventory_equip_slot` |
| `int inventory_index(self, void *item)` | index of the item in the inventory or -1 (use it to grant once) |
| `int set_text(self, void *obj, const char *prop, const char *utf8)` | `make_text` + store into a Text property. 1 = ok |
| `int on_save_loading(self, cb(void *save, void *user), user)` | before a loaded save resolves its item paths: the last moment to `create_asset` the items the save names |
| `int on_save_written(self, cb(void *save, void *user), user)` | after the save object's item path list was written: `save + 0xB0` is the `TArray<FSoftObjectPath>` of owned items (element size 0x18); read it to keep a sidecar |

The local inventory component is the pawn's `m_InventoryComponent` (`Controller::K2_GetPawn` -> `BaseCharacter::m_InventoryComponent`); `BPF_GetInventoryComponent` is not on the player controller.

`rows.json`: a value for an Object / Class / Interface property whose object is not loaded and whose package is in no pak is skipped with a log line (`rows: ... is not loaded and its package is in no pak`); the rest of the table's edits still apply. Soft object properties are unaffected.

## Content registry (API 2.4)
Mods can ship data instead of code: `Mods\<mod>\content\<kind>\*.json` (guide: `docs/MODDING-GUIDE.md` section 10b). The loader owns the built-in kinds `items`, `styles`, `powers`, `npcs`, `bots`, `ranks`, `bot_trials`, `poise`, `maps`, `weapons`, `anim_sets` and `clones`; plugins can add kinds:

```c
int (*register_content_kind)(const AbsApi *self, const char *kind,
        int (*validate)(const char *json, char *err, int cap, void *user),
        int (*apply)(const char *mod_id, const char *entry_id, const char *json, void *user), void *user);
```
* `kind`: lower case letters, digits, `_`. Returns 0 = registered, 1 = bad arguments or table full (16 kinds, built-ins included), 2 = that kind already has a handler (built-ins win). Call it in `AbsPlugin_Load`.
* Processing happens at engine ready and again right before a save resolves its items (`on_save_loading`): files are read in mod load order, and each entry (an object with an `id` of letters / digits / `_`, up to 63 characters) is passed to `validate` as compact JSON text. `validate` returns 1 if usable, else 0 with the reason in `err`: the entry is logged `content: <kind> <mod>/<file> '<id>' INVALID: <reason>` and skipped.
* Two entries with the same kind and id: the one from the mod later in the load order wins, the log lists both (`content: CONFLICT items 'X': A/... is overridden by B/...`).
* `apply(mod_id, entry_id, json, user)` is called on the game thread for every winning entry: return 0 when done (it is not called again), nonzero to be retried at the next pass. Both callbacks are inside the plugin's crash guard (3 faults switch the plugin off). `mod_id` is the mod's manifest id.
* A file is one entry object, an array of entries, or `{ "<kind>": [ ... ] }`. Unknown kinds (no handler registered) are skipped with a log line.
* A mod's `content\` folder counts like `rows.json` / paks: it makes the mod active and is part of the mod fingerprint compared in LAN play.
* Hot reload calls `apply` again for a changed or new entry: write it idempotent.

### Manifest and load order (mod.json)
`mod.json` has `id`, `version`, `api` (minimum API version, an integer), `requires` (`[{"id":"X","min":"1.0"}]` or plain ids), `after`, `supersedes`, `affects_gameplay` (`docs/MODDING-GUIDE.md` section 9). Mods that are switched off in `loadorder.txt`, need a newer API, reuse an earlier mod's id, or lack a required mod (not installed / off / skipped / older than `min`) are skipped with a reason, dependants too; the rest are ordered by `requires` and `after`, with `loadorder.txt` as tie-break. Plugins and content follow that order. The order is logged once (`mods: load order (n mod(s)): A, B, C`); skipped mods are logged (`mods: SKIPPED X: requires Y which is not installed`), listed in `LanNative\mods.txt` and shown on the game screen for 90 s (red line "MODS SKIPPED").

### items entry
`id`, `base` (stock asset path to copy), `name`, `description`, `icon` (texture path), `icon_file` (an image next to `mod.json`, made into a runtime texture with `texture_from_file`; wins over `icon`), `props` (any property as engine text), `shop` + `shop_copy_of` + `price` (EquipmentShop rows get `m_Equipment`, WeaponShop rows `m_Weapon`), `gear_row` (`{table, copy_of, set}`: own gear-table row, for example `WeaponDataTable`), `moves` (weapons: attack rows allowed in the weapon family's deck only while the item is equipped), `grant` (give it to the loaded character, see "Grants"), `equip` (equip after granting, see "Mod hygiene"), `weapon` (a `weapons` kind id, see "Decks, styles and the weapon slot"). Creates `/Game/Mods/<mod id>/<id>`; shop and gear rows are named `<mod id>_<id>`.

A cooked icon (a `Texture2D` in a pak) works the same through `icon`; prefer it for shipped mods. `icon_file` needs no cooking.

### styles entry
`id`, `db` (a FightingStyleDB asset, usually from a pak), `style_id` (0-63, new styles start at 6), `index` (progression index = button slot, 5-15), optional `name` / `icon` (written into the DB's `m_Name` / `m_StyleSmallIcon`). Optional `gleam_level` (1-99): the style is unlocked only when the game's own answer is yes or the Gleam level (`BPF_GetResourcesLevel`) reaches it (Faejin = 6); without it the style is always unlocked. A later mod's entry with the same `style_id` replaces an earlier mod's (one button). The loader adds a button to the Meditation menu's fighting style page and hooks `UStatsComponent::BPF_IsFightingStyleUnlocked` / `BPF_GetFightingStyleByUnlockIndex` by symbol (needs `LanNative\symbols.bin`).

### Grants
`items` / `powers` entries with `grant` are pending per character. They are re-armed when a character save loads (`on_save_loading`) and checked every 1.5 s in every world except the title `StartMap` until the local pawn and its inventory exist and the inventory item count is the same non-zero value twice in a row (the save's items are in). Then an item that is already owned is logged `already owned` and never added again; otherwise `inventory_add` must return an index (`content: granted <id> -> inventory index N (attempt k)`); a failure stays pending and is retried. Waiting is logged throttled (`content: grant <id>: waiting (no pawn), attempt 1`); when nothing is pending: `content: grants done (...)`. A grant still pending after 4 checks in a world shows as `WARNING: grant <mod>/<id> NOT done yet: <reason>` on the F2 Mods page. Items are equipped on the first grant only (one per slot); powers use `equip_slot` only on a new grant.

## Runtime textures (API 2.5)
* `void *create_texture(self, const void *bgra, int w, int h, const char *path)`: a texture from 32-bit BGRA pixels (straight alpha, row-major, top row first), 1..4096 per side, placed at `path` (`/Game/Mods/<Mod>/<Name>.<Name>`) and rooted, so soft paths (`m_InventoryIcon`) resolve to it. Game thread only; an object already at `path` is returned; NULL on failure (reason logged).
* `void *texture_from_file(self, const char *file_utf8, const char *path)`: PNG / JPEG / BMP / GIF / TIFF (absolute path) decoded with Windows Imaging Component (part of Windows), then `create_texture`.
* A runtime texture works wherever the game shows the icon: item lists, info panel, equipped slot, HUD deck badge, shop tiles.

## Mods page, menu items and hot reload (API 2.7)
* `int add_menu_item(self, label, cb(user), user)`: a line on the F1 menu's **Mods** page (open it with **F2**), shown as `<your mod>: <label>`; `cb` is called on the game thread when the player selects it, inside your guard. 0 = ok, 1 = table full (64 items) / bad arguments.
* `int reload_content(self)`: hot reload of `Mods\*\content` and `rows.json`; the same as the first line of the Mods page and a `LanNative\reload.txt` file (read once a second and deleted; the result goes to `LanNative\reload_result.txt`). Returns the number of changed or new content entries, -1 if nothing could be reloaded (also in safe mode). Game thread. What can and cannot be reloaded: `docs/MODDING-GUIDE.md` section 8b. For your own content kind: `apply` is called again for a changed or new entry, so write it idempotent.

## Powers (API 2.9)
Example: `examples/ContentMods/WideWave`. Game thread; slot / dir 0 = key 1 / D-pad left, 1 = key 3 / D-pad right.
```c
int inventory_equip_slot(const AbsApi *self, int index, int slot);   /* put inventory entry `index` into power slot 0/1; 1 = ok. inventory_equip cannot (it uses slot -1) */
int on_power_used(const AbsApi *self, void (*cb)(void *character, void *item_data, void *actor, int dir, void *user), void *user);
                                                                     /* a power's effect actor was spawned: user (player or NPC), UUsableItemData, the spawned AUsableItem INSTANCE, dir. Patch the instance, never the class defaults. 0 = ok */
int power_use(const AbsApi *self, void *character, int dir);        /* use the power in slot dir without a key (keys 1/3 only switch decks in practice mode). Player (character NULL): the game must say CanUse (NotEquipped / OnCoolDown / InsufficientShards ... are refused and logged), then the launch. NPC: just the launch. No HUD launch event. 1 = launched */
```
Cost (shards), cooldown, effect timing and the hit all stay the game's own: they happen when the animation's `ItemUseNotify` fires.

### powers entry (built-in content kind)
A power is a clone of a stock power `UsableItemData` (made like an item, so `/Game/Mods/<mod>/<id>`, `name`, `description`, `icon` / `icon_file` work as for items; do not reuse an item id) plus data:
* `id`, `base` (required: the stock power to copy, for example `/Game/DB/Items/Shockwave.Shockwave`; it decides the effect actor class, that is the look and the kind of effect), `name`, `description`, `icon` / `icon_file`, `cost` (shards 0-255), `cooldown` (seconds), `target_requirements` (relation bit mask), `props` (any other property as engine text).
* `anims` (`{ "all": path }` or `fl` / `fr` / `br` / `bl` -> AnimSequence path, loaded at engine ready; a path that cannot be loaded keeps the stock animation; the animation's notifies decide when the effect fires, so a replacement needs an `ItemUseNotify`) and `anims_mirror` (4 booleans).
* `attack` (`{ "copy_of": "10001", "set": { "m_fKnockBackDistance": 500 } }`: an own ItemAttack row `<ModId>_<id>` in `/Game/DB/Items/ItemAttack`, written to the spawned effect actor's `m_ItemAttackName`), `radius` (hit sphere radius of the spawned actor in Unreal units; stock Shockwave 280; only for sphere-shaped powers).
* `grant` (see "Grants"; already owned = not added twice), `equip_slot` (0 or 1; only a newly granted power is equipped, so the player's own slot choice stays).

The per-instance overrides are applied in `on_power_used`, so they also apply when an NPC uses the power. Hot reload re-applies name, icon, cost, cooldown, anims, attack values and radius; `base` and `attack.copy_of` need a restart.

## Decks, styles and the weapon slot (API 2.10)
Game thread. Family 0 BareHands, 1 Sword, 2 WarGloves. Decks are the live save's `m_SavedData.m_SavedCombos[family]` (what Meditation > Combat Deck shows).

| Function | What |
|---|---|
| `int combo_slot_ensure(self, family, name, copy_from_index)` | index of the deck `name`; a missing one is appended as a deep copy of deck `copy_from_index` (the 4th deck gets index 3). -1 on error |
| `int combo_load(self, family, name)` | `FightingPlayerController::BPF_LoadComboFromName(name, family, Current)`; 1 = that deck is now current |
| `int combo_current(self, family, name_out, cap)` | current deck name; returns its index or -1 |
| `int set_combat_style(self, character, style)` | local player only (NULL or the local pawn): `BPF_SetPlayerCombatStyle(style, Current)`; 1 = applied |
| `int on_weapon_changed(self, cb(character, old_item, new_item, handled, user), user)` | the local player's equipped weapon (`m_EquippedWeapon`) changed (polled per tick; once with `old_item` NULL when the character appears; `handled` is always 0) |
| `int on_style_changed(self, cb(character, old_style, new_style, user), user)` | the local player's fighting style changed (polled; first call `old_style` -1) |
| `void *prop_ptr_path(self, obj, "a.b[2].c")` | nested property address (see "Properties and parameters") |
| `int fname_make(self, utf8, uint64_t *out)`, `int fname_text(self, fname, out, cap)` | FName helpers; 0 = ok |

### Weapon fields (`weapons` kind, used by `items`)
A weapon kind (content kind `weapons`) carries the weapon-slot fields; an item uses them with `"weapon": "<kind id>"` (its own copies of these fields are then ignored, logged). On an item without `weapon` they are still read, logged as deprecated.
* `deck`: `{ "first_slot": 5, "count": 4, "label": "...", "layout": {...}, "layouts": [...], "seed": [...], "lock_family_decks": false }`: the item's own decks in the family's deck list (from `first_slot`, `count` of them), created at run time if missing and seeded once (deck k from `layouts[k]`; `null` = `layout`; without a layout, `seed` = attack ids chained in the first row's quadrant). Meditation > Combat Deck gets a row `<label>` under the family row. Equipping the item loads the item deck used last; unequipping loads the family deck that was in use. The item decks are unlocked only while the item is equipped; decks 1-4 keep the game's progression. `layout` is a deck per in-game row (FrontRight / FrontLeft / BackRight / BackLeft: up to 3 chained rows each, `"373m"` = placed mirrored) plus `alts` (one alternate per row). `lock_family_decks: true` closes the family's other decks while the item is equipped (default false: a fully locked row blocks keyboard navigation in that menu). A save written while an item deck is current names the family deck instead, so the save works unchanged without the mod.
* `anim_style` (or `anim_set`, an `anim_sets` id): see "Animation style". The old item field `style` is read as `anim_style` (logged).
* `moves_exclusive: true`: the family's attack rows and the item's rows never mix: item rows only while the item's deck is current, other rows never while it is.

## Animation style (API 2.17)
A character can use another style INDEX of the style-indexed animation tables (guard idle / prep, hit reactions, locomotion idles, dodge, stance change, cancel, fidgets) while its fighting style (stats, special ability, save, menus, mentoring) stays what it is.

| Function | What |
|---|---|
| `int anim_style_set(self, character, style_index)` | from now on the character's animation lookups use `style_index` (0..63); -1 removes. `character` NULL = the local player (kept across travel); other characters' entries are dropped at a world change. Game thread. The hooks are installed at the first call with a style. Locomotion is refreshed when the value changes. 1 = ok |
| `int anim_style(self, character)` | the index the animation lookups use now (override, else the real style); -1 = no character |
| `int real_style(self, character)` | the fighting style as the game sees it (`BPF_GetCombatStyle`), never overridden; -1 = none / uninitialised |
| `int on_handled_weapon(self, cb(character, weapon_data, family, user), user)` | the local player's weapon IN HAND changed (drawn, sheathed, disarmed, picked up): `weapon_data` = the `VisibleWeaponData` in hand or NULL, `family` = the active weapon family (0 when nothing is in hand). Polled per tick; fires once when the character appears. 0 = ok |

Limits: the override only applies to the animation code paths; stats, save, menus, AI and the style-unlock code always see the real style. Draw / sheathe clips keep the real style. If your plugin hooks `AFightingCharacter::BPF_GetCombatStyle` itself and its detour is stacked ABOVE the loader's, the override no longer applies (vanilla look). When an override is removed the loader logs per-caller counts (`animstyle: override removed: substituted/calls | ...`).

Weapon field on top: `"anim_style": 7` = `anim_style_set(NULL, 7)` while that weapon is in hand (drawn), removed otherwise.

## Per-character sidecar (API 2.18)
Metadata about a CHARACTER that must not go into the game's save: one file per character, `Binaries\Win64\LanNative\state\<GUID>.txt` (`key=value` lines), keyed by the save's character GUID (`m_SavedData.m_FightingCharacterGuid` as 32 hex digits), so renaming the character keeps the state. A brand-new character without a GUID yet is keyed `noguid_<name>` (logged). The file is written through on every change (temp file + rename). Copies of one save share the GUID and therefore the state. `kv_get` / `kv_set` stay the per-PLUGIN store.

| Function | What |
|---|---|
| `int sidecar_get(self, character, key, out, cap)` | 1 = found; 0 = absent / no character save loaded. `character` NULL = the local player (the only character with a save; any other is refused, logged). Stored as `p.<plugin>.<key>` |
| `int sidecar_set(self, character, key, value)` | NULL removes; key without `=`, CR, LF; 1 = on disk. Game thread |

Loader keys in the same file: `kind.<item id>.deck` (a weapon kind's deck used last), `family.<n>.deck` (the base family's deck for its other weapons), `name` (last name seen).

## NPCs (API 2.14)
Game thread only. The content kind `npcs` is built in (no plugin needed); the functions below also work for plugins on any archetype.

| Function | What it does |
|---|---|
| `void *npc_spawn(self, archetype, loc*, yaw*, class_path*)` | spawn an NPC AT `loc` (NULL = 350 uu in front of the local character; then a local character is needed) facing `yaw` degrees. `archetype` = an ArchetypeAsset path (stock `/Game/DB/AI/NPCs/Trickster/Trickster.Trickster` or a copy `/Game/Mods/<mod>/<id>.<id>`) or the plain id of an `npcs` / `bots` content entry (its `faction`, `aggro` and reactions then apply). The spawner is placed at `loc` before the spawn and the pawn is never teleported afterwards: never teleport an AI NPC yourself either, it does not move again. `class_path` NULL / "" = the generic AI character (boss / miniboss Blueprint classes can be named). Returns the character or NULL |
| `int npc_remove(self, character)` | for an NPC made by `npc_spawn` / `test_spawn_npc`: unspawn it and destroy its spawner (a level's own spawner is put back instead); for any other character the actor is destroyed; 1 = ok |
| `int npc_set_target(self, character, target)` | `UAIFightingComponent::BPF_ForceEnemy(target)`: the NPC goes for that actor (an NPC does not find a far-away player by itself). `target` NULL clears it. 1 = ok |
| `int npc_set_faction(self, character, 0..5)` | `AFightingCharacter::ServerSetFaction`; if that leaves another value the faction byte is written directly (logged). Returns the faction read back or -1 |
| `int faction_can_target(self, a, b, set)` | the factions target table (6 x 6, row = attacker's faction; AI characters default to faction 2): `set` -1 = read only, 0 / 1 = write; returns the value now in the table or -1 |
| `void *npc_archetype(self, character)` | the `ArchetypeAsset` the character runs, or NULL |
| `int on_area(self, center*, center_actor, radius, who, cb(character, entered, user), user)` | one loader-side 0.1 s poll for all registrations: `cb(who, 1, user)` when `who` comes within `radius` uu (2D distance, height ignored) of the point `center` or of `center_actor`, `cb(who, 0, user)` when it leaves again (5 % hysteresis). **`who` NULL = the local player**, not "any character". A character used as `center_actor` / `who` drops the area when it is destroyed; any other actor must outlive the area. Returns a handle (> 0) or 0 |
| `int area_remove(self, handle)` | stop watching; 1 = removed |
| `int show_message(self, utf8, seconds)` | on-screen text for 1..30 s: the loader's toast (up to 3 lines of 96 characters; a newline separates lines). The centred banner and the score line of the modes API (`show_banner`, `hud_score`) are separate |

There is no ground trace: give a spawn spot that has ground (the open world has void edges next to walkable ground).

### npcs entry (built-in content kind)
`Mods\<mod>\content\npcs\*.json` (guide: `docs/MODDING-GUIDE.md` section 10d); the object is `/Game/Mods/<mod>/<id>.<id>` (and `<id>_Deck` for the deck copy), made at engine ready from a copy of `base` (the stock archetype and its deck are never written). At most 32 entries.

| Field | Meaning |
|---|---|
| `id` (required), `base` (required) | the stock `ArchetypeAsset` to copy; its tree, gear, difficulty assets and everything not listed stay stock |
| `name` | label for the logs (the character's display name is not changed) |
| `health` (`hp`), `style` | `m_fHealth` (1..100000), `m_uiCombatStyle` (0..63) |
| `deck` | `FrontLeft` / `FrontRight` / `BackRight` / `BackLeft` (arrays of up to 8 attack rows, same left/right mirror as save decks), `Alt` (up to 4); omitted keys keep the copied deck's rows; `copy_of` = another `AttacksComboDB` asset to copy instead of the base's bare-hands deck. Written into the bare-hands deck |
| `ai` | `attack_cooldown` (s), `can_attack`, `can_defend`, `xp_bonus` (0 = no XP) |
| `props` | any `ArchetypeAsset` property as engine text (like `rows.json` values) |
| `faction`, `aggro` | `faction` is applied 1.5, 4 and 8 s after a spawn. `aggro: true` engages the player (`BPF_ForceEnemy(player)` plus the AI blackboard key `Enemy`) 1.5, 4, 8, 15 and 30 s after the spawn and again when the player enters the NPC's `on_area` radius (log `npcs: <id> engage (...)`) |
| `spawn` | `{ "map": "MainMap_Main", "loc": [x, y, z], "yaw": 0, "near": 1500, "delay": 4 }`: once per world, `delay` seconds after the player comes within `near` uu of `loc`. `map` is a part of the world name. The ground there must be loaded by then: an NPC that is already 300 uu below its spot 1.2 s after the spawn is removed again with a warning and may spawn again on the next visit |
| `on_health_below` | `{ "fraction": 0.5, "do": [...] }`: first hit that takes it to or below that fraction of its max health (once per NPC) |
| `on_death`, `on_area` (`on_player_near`) | `{ "do": [...] }`; `on_area` also needs `"radius"`: the player comes within it of the NPC |
| actions in `do` (at most 6) | `log <text>`, `message <text>`, `heal`, `ai on`, `ai off`, `target player`, `faction <0-5>`, `remove` (on the next tick) |
| `weapon`, `gear`, `ai_level` | not supported in `npcs` entries: accepted, ignored and logged (bots have their own `ai_level` and outfit fields) |

Reactions are bound to every character that uses the entry's archetype, however it was spawned. Hot reload: values, deck rows, reactions and the spawn point are re-applied to the archetype copy (NPCs that already exist keep the values they started with); `base` and `deck.copy_of` need a restart.

## Timers with ids (API 2.11)
The timer table has 256 slots shared by all plugins, at most 128 per plugin; a one-shot slot is freed when it fires and ids are never reused.

| Function | What it does |
|---|---|
| `int set_timer_id(self, seconds, repeat, cb, user)` | like `set_timer`, returns the timer id (> 0) or 0 when nothing was created (`seconds` must be > 0; the reason, "table full" or "plugin limit", is logged the first 20 times) |
| `int cancel_timer(self, id)` | removes one of YOUR timers, also from inside its own callback; 1 = removed, 0 = unknown id / somebody else's / one-shot that already fired |
| `set_timer` | returns 0 = ok, 1 = failed (same table) |

A timer added from inside a timer callback starts counting at the next tick. A repeating timer fires at most once per tick even after a long frame.

## Modes (API 2.12)
Game thread only. `AbsMatchInfo` is declared in `AbsolverSE.h` (set `size` first). Everything here is poll based.

| Function | What it does |
|---|---|
| `int travel(self, map_or_url)` | console `open <map>?listen`. `"/Game/Maps/.../Name"` or `".../Name.Name"` gets `?listen` unless it has options; text starting with `open ` / `servertravel ` is sent as is. 1 = issued; the world changes a moment later (`on_world_loaded`) |
| `int console_cmd(self, cmd)` | `KismetSystemLibrary::ExecuteConsoleCommand` with the local player controller; 1 = issued |
| `int solo_duel(self, on)` | the loader's solo duel switch (`1` = a hosted 1v1 or 3v3 starts with only the local player; `0` stock; `-1` read only; returns the state). Call it BEFORE `travel`. It needs the loader's duel flow (on by default; the `pvpfix` setting must not be `off` or `guard`), which also keeps the game from sending a lone player back to the main map. Without it a lone player waits forever in the arena |
| `int match_info(self, AbsMatchInfo *out)` | `mode` (game mode class), `state` (MatchState text), `is_duel`, `team_count`, `winning_rounds`, `team_score[0..3]`, `round` (= rounds played + 1 while `InProgress`), `winner_team` (once the match is decided), `server_time`, `game_mode`, `game_state`. Returns 0 when the world has no game state |
| `int on_match_state(self, cb(old, new, user), user)` | `MatchState` changed (polled 10x per second); `old` is `""` the first time a world is seen. Typical states: `EnteringMap`, `WaitingToStart`, `Intro`, `StartCountdown`, `InProgress`, `WaitingPostMatch` |
| `int on_round_end(self, cb(const AbsMatchInfo*, winner_team, user), user)` | a team score went up (polled, duel game modes only); scores that drop (new match) do not fire |
| `int set_mode_prop(self, name, text)` | engine-text write of a property on the live game mode AND game state, for those that have it (`m_iNbWinningRounds` exists on both); returns a mask (1 game mode, 2 game state, 0 = none / refused); before / after values are logged. Do it right after `on_world_loaded`, before the match starts |
| `int show_banner(self, utf8, seconds)` | yellow text in the upper middle of the screen for 0.2..60 s; queued (8), shown one after the other. Drawn as canvas text on the loader's menu hook (on by default); plain ASCII / Latin text |
| `int hud_score(self, utf8)` | persistent white line near the top of the screen; `NULL` / `""` removes it. The plugin formats the text (`"YOU 1 : 0 BOTS"`) and calls it again when the score changes |

Notes: `match_info` works in the open world too (game mode `DefaultGameMode_C`, state `InProgress`, `is_duel` 0, team scores -1); use `is_duel` / `mode` to tell. The stock scores only count player kills: a mode that wants first-to-N against bots keeps its own score (`on_death`, `on_hit`). There is no general drawing API for plugins: use `show_banner`, `hud_score` and `show_message`. To respawn a character, use `test_heal` + `test_teleport`.

## Map script (API 2.13)
Content kind `maps` (built in, no plugin needed): `Mods\<mod>\content\maps\*.json`, one scene per entry; example `examples/ContentMods/MapScriptDemo`; guide and field list: `docs/MODDING-GUIDE.md` section 10e. Plugin functions:
* `int mapscript_phase(self, name, on)` sets a named phase flag that lights, meshes and actions test with `"on": "phase:<name>"`.
* `int mapscript_mesh(self, mesh_id, state)` moves a live scene mesh to a named state (`"open"`, `"closed"`).

Both return 1 on success and log. The modes API (`on_round_end`, `match_info`) is the natural source for them in a game mode.

Behaviour: lights, meshes and grids are spawned actors; triggers are a per-tick box test of the local player's position (only the player triggers). Scene fields: `curves`, `lights`, `meshes` (with `material`, `color` / `color_curve` / `brightness` / `param` through a dynamic material instance, `on` conditions: a mesh whose condition is false is hidden and has no collision), `grids`, `triggers`, `events` (start, every n seconds, team score (teams 0+1) >= n, match state; read once per tick), the `cycle` action, and `navmesh`. A scene is built `delay` seconds (default 2) after a matching world has a player, destroyed on travel, and rebuilt after a hot reload of a changed entry.

Limits: 4 scenes; per scene 64 lights, 16 `meshes` entries (96 meshes with grid cells), 8 triggers, 8 events, 12 curves, 6 actions per list; 48 lights live over all scenes (per scene the strongest by intensity x radius are kept; more are skipped and logged).

`navmesh`: `{"center": [x,y,z], "half": [x,y,z]}` (world coordinates, also with origin `player`; every half-size 100..50000) builds a navigation mesh over that box at run time. Duel maps ship none, so the AI has nothing to path on; bots in a bot trial use real paths once it is ready. One navmesh per world (the first scene that has one); a failure is logged (`mapscript: navmesh FAILED ...`) and the AI keeps moving without pathfinding.

## Bots and ranks (API 2.15)
Game thread only; `character` NULL = the local player.

| Function | What it does |
|---|---|
| `int xp_add(self, character, amount)` | `StatsComponent.BPF_AddGlobalXP(GAMEMODE, amount, Current, notify 0)`, what the post-match code calls: levels, overflow into Prestige and grade roll-over are handled by the game. 1..10,000,000; 1 = called |
| `int gm_points_add(self, character, points)` | game-mode points (`UStatsComponent::AddGameModeXP`); every 5 points = one game-mode level = one Rift Coin (small loot box) in the inventory. Returns the Rift Coins added, -1 on failure |
| `int rank_info(self, character, AbsRankInfo *out)` | `level`, `prestige_grade` / `prestige_level`, `visible_level`, `xp` (cumulative), `next_xp` and `next_xp_game` (the game's own threshold, should be equal), `gm_xp` / `gm_level`, `spare_prestige_points` (the "Crystals" readout) |
| `int trial_reward(self, character, ranks_id, won, rounds_won, mentoring, pay, AbsTrialReward *out)` | the stock 1v1 table (500 finish, +4000 win, +1500 for 2+ rounds won, +500 mentoring with a win; 1 game-mode point each) or a `ranks` entry's table; `pay` = 1 grants it (`xp_add` + `gm_points_add`). Returns the XP; `out` (may be NULL) gets `xp`, `points`, `rift_coins`, `paid` |
| `float npc_difficulty(self, npc, set)` | the AI's live difficulty level 0..100 (it rubber-bands during a fight); `set` >= 0 writes it. -1 = not an AI character |

The XP curve is CUMULATIVE: `Req(1) = 2000`, `Req(L) = Req(L-1) + 50 * round(curve(L) * 0.02)`, Req(60) = 672,000, Req(61) = 692,000 (one Prestige grade).

### bots entry (built-in content kind)
`Mods\<mod>\content\bots\*.json` (guide: `docs/MODDING-GUIDE.md` section 10f). A bot is an `npcs` entry (the archetype copy, `spawn`, `aggro` (default true here), `faction`, reactions all work as there) built from a template, a real deck and an AI profile. Spawn one from a plugin with `npc_spawn(self, "<bot id>", loc, &yaw, NULL)` and never teleport it afterwards; `aggro` sends it after the player.

```json
{ "id": "duelist", "profile": "vanilla", "ai_level": 60, "style": "Faejin", "health": 1000,
  "deck": { "weapon_slot": 0, "rows": { "FR": ["197", "169", "212"], "FL": ["210", "014"], "BL": ["191", "206"], "BR": ["198", "196"] },
            "alternates": { "FR": "170", "FL": "015", "BR": "117", "BL": "042" } },
  "aggro": true, "spawn": { "map": "MainMap_Main", "loc": [-33378, -7647, -3545], "near": 1500 } }
```

| Field | Meaning |
|---|---|
| `template` | ArchetypeAsset to copy; default `/Game/DB/AI/Bots/AI_Classic_Hard.AI_Classic_Hard` (the game's unused dev bot: 1000 HP, stamina 250, 5 shards, free play, no attack sequences) |
| `deck` | a deck inline (`{"weapon_slot": 0, "rows": {"FR": [...], "FL": [...], "BR": [...], "BL": [...]}, "alternates": {"FR": "170", ...}}`) or `{"file": "decks/x.json"}` (a file inside the mod folder, in that shape or the shape `rows: [{start, chain: [{id}], alternate: {id}}]`). Up to 8 moves per row. Bare hands (`weapon_slot` 0); sword decks go in `deck_sword`; war gloves are refused |
| `mirror` | default true: rows are stored left/right mirrored like the player's save decks (in-game FR row -> `m_ComboQuadrantsFrontLeftXls`, FL -> `FrontRight`, BR -> `BackLeft`, BL -> `BackRight`; alternates in FR, FL, BR, BL order) |
| `profile` | `vanilla`, `classic_hard` (default): see below |
| `ai_level` | `m_uiInitialDifficultyLevel` 0..100 (the AI blends its easy / hard difficulty assets by this level and rubber-bands it during a fight) |
| `ai_level_fixed` | true: the live level stays at `ai_level` (the bot gets its own copy of its difficulty variation data with every drift at 0). Only that bot is affected. Without it the stock AI gets easier every time it or the player succeeds |
| `resilience` | 0..10 (`m_uiResilience`, written after the profile). 0 = interruptible like a player; 3 = the bot hits through every player attack |
| `style` | `Forsaken`, `Windfall`, `Kahlt`, `Stagger`, `Faejin` or 0..63; Stagger / Faejin also get their special ability DB |
| `health`, `name`, `faction`, `spawn`, `on_health_below`, `on_death`, `on_area` | as in `npcs` |
| `props` | extra archetype values as engine text; `name[i]` writes one element of a static array |
| `deck_sword` | a sword deck (same shape, `weapon_slot` 1). The bot also needs a sword: `outfit.weapon` (a `VisibleWeaponData`, for example `/Game/DB/Weapons/Sword/SwordMachete.SwordMachete`) |
| `stats` | `none` (default); `guide` = the level-60 stat build of Steam guide 3615970885 for the bot's style (84 points): the Will build when the bot has a `deck_sword`, else the non-Will build; `guide_will`, `guide_no_will`, `none`. The archetype gets that build's health / stamina (unless `health` is given) and every spawned pawn gets the points, so hit damage is computed from its Strength / Agility like a player's |
| `outfit` | own copy of the template's equipment selection: `mask elbow shoulder undertop overtop gloves belt trousers shoes hair` -> item path or `null` (empty), `materials` {slot: variant}, `weapon`, `skin_tone`, `sex` |
| `see_forever` | default true: the bot never loses aggro |
| `intro` | the 1v1 intro row the bot plays in a player-like bot trial (for example `004`) |

Profiles (every write is read back and logged `bots: <id>: ... reads back ...`, differences marked `<-- DIFFERS`):
* `vanilla` = typical stock NPC values: no feints, stance change 0 % / 2 s, alternates 0, perfect link 30, runs of 3..5 attacks, cooldown 2 +- 1, open state 2 +- 0.2, guard / dodge / special 50 / 30 / 50, step-back dodges 120 / 150 (cooldown 3), damage x0.9; the template's stock difficulty assets.
* `classic_hard` = the template unchanged.

### ranks entry (built-in content kind)
`Mods\<mod>\content\ranks\*.json`:
```json
{ "id": "bot_trials", "rewards": { "finish": [1, 500], "victory": [1, 4000], "performance": [1, 1500, 2], "mentoring": [1, 500], "xp_scale": 1.0 }, "gm_points": true }
```
Values are `[points, xp(, min rounds won)]`; missing keys keep the stock values. Used by `trial_reward(..., "bot_trials", ...)`. The keys `match`, `ladder` and `prestige` are accepted and ignored.

## Bot trials (API 2.16)
Game thread only. Guide: `docs/MODDING-GUIDE.md` section 15.

| Function | What it does |
|---|---|
| `int bot_trial_start(self, trial_id)` | starts the `bot_trials` entry: `solo_duel(1)` + `travel(map)`, waits for the match, sets the rounds to win, plays the rounds (below), shows the result, travels back and pays the reward there. 1 = started; 0 = unknown id, an unknown bot, or a trial already running |
| `int bot_trial_status(self, AbsTrialStatus *out)` | the running or last trial: `id`, `phase` (`travel wait_match spawn fight round_end end payout done aborted`), `stage` / `stages` (gauntlet), `player_score` / `bot_score` / `rounds_to_win` of this stage, `total_player` / `total_bot`, `finished`, `won`, `xp`, `points`, the stage's `ai_level`, the current `bot` character (NULL between rounds). 1 = there is one |

### bot_trials entry (built-in content kind)
`Mods\<mod>\content\bot_trials\*.json`:
```json
{ "id": "coliseum_duelist", "name": "Duelist in the Coliseum", "map": "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum",
  "bot": "duelist", "rounds_to_win": 3, "ai_level": 60, "rewards": "bot_trials", "menu": true }
```
| Field | Meaning |
|---|---|
| `map` | a 1v1 duel map (`/Game/...`; its pak must be mounted). The map's own game mode is the stage (spawn points, intro, camera, stock HUD) |
| `bot` / `bots` | one `bots` (or `npcs`) id, or a list = a gauntlet fought in order (max 8); every bot must be beaten |
| `rounds_to_win` | 1..9 per bot (default 3); the stock HUD shows that many diamonds |
| `ai_level`, `ai_level_step` | initial difficulty of bot i = `ai_level + i * ai_level_step` (0..100), written to the bot's archetype before each spawn and to the live level after it (use `ai_level_fixed` in the bot entry to keep it there) |
| `rewards` | a `ranks` id for `trial_reward` (default: the stock 1v1 table). `rounds_won` = the player's rounds over the whole trial |
| `player_like` | default true: the bot enters the duel like a second player (below) |
| `intro` | default true: with `player_like`, the stock intro also plays the bot's scene |
| `block_respawn` | re-spawn a bot that is stuck on its way in (default false when `player_like`, else true) |
| `spawn_distance` (350), `round_gap` (3 s), `round_seconds` (180: a round without a knock-out is replayed), `end_delay` (8 s), `return` (`/Game/Maps/MainMap/MainMap_Main`), `menu` (true: F2 Mods page entry "Content: Bot trial: <name>") | |
| `approach_assist` | `auto` (default, the same as `on`) / `on` / `off`, see below |

**Player-like trials (default).** The bot gets a player state on team 1 with its name, so the stock 1v1 HUD names both fighters and every kill scores through the game's own code (log `STOCK SCORE ... OK`; the loader writes the player's score itself only as a fallback). With `intro`, the stock intro plays the bot's scene with its `intro` row; the bot's AI starts at `InProgress`. The bot enters at the arena's other-team player start farthest from the player (no start at least 300 uu away: `spawn_distance` in front of the player). A knocked-out bot respawns after the game's respawn time at the start farthest from the player and runs in; a player knock-out uses the stock respawn and the bot is engaged again. Nobody is healed or moved between kills. The stock match stays `InProgress` until the trial ends itself (a `WARNING the stock match went to ...PostMatch` line would show otherwise).

**Without `player_like`.** Each round the bot is spawned with `npc_spawn`, and the loader counts the rounds from `on_death` (the stock solo duel never scores a kill of an NPC) and writes the player's points into the player's score, so the stock HUD diamonds fill and map script `score` events fire.

**Approach.** Duel maps have no navigation mesh. With `approach_assist` on, a bot farther than 300 uu runs to the player (its fight AI paused; about 360-420 uu/s), without pathfinding unless a map script `navmesh` is ready. Within 300 uu its fight AI takes over again, with the player as lock target and movement towards the player up to 95 uu. Attacks, defence and stance changes stay the AI's; strafing and spacing do not happen in arenas. With a ready navmesh the run follows real paths, a start with only a partial path to the player is skipped, and a run that stalls within 900 uu for 2 s hands over to the fight AI. The bot can be spawned before a runtime navmesh is ready; it then runs without paths and can stall on furniture.
With `block_respawn`, a bot that has not moved 60 uu for 1.5 s on its run-in (and never came within 300 uu) is re-spawned 150 uu from the player, at most once per round (log `approach RUN BLOCKED ...`; otherwise `no progress ... NO re-spawn, the bot keeps chasing`). A bot without any attack 8 s after its spawn gets a ticket dump in the log (`tickets (bot without attacks after 8 s)`).

**Fall guard.** In the trial map the player is put back on the last spot it stood on after a drop of 1000 uu (`FALL GUARD` in the log); more than 3 falls in 10 s abort the trial back to the open world.

**End.** Banners: the bot's name and round, the round winner, and at the end `VICTORY   You 5 - 1 <bot>   +6000 XP` or `<bot> wins 3 - 1   +500 XP`; score line `You 1 - 0 <bot>` (`hud_score`). The reward is paid after the travel back to the open world ("Bot trial reward: +X XP, +N point(s)"); XP granted inside the arena would be lost when the open world loads.

Log lines worth reading: `approach RUN ...`, `APPROACH reached N uu X s after the round start`, `ROUND SUMMARY`, `HIT on the bot ...` and `STAGGER check` (whether a hit on an attacking bot interrupted it; see `resilience`).

## Pre-hit, damage_mod and poise (API 2.19)
`on_hit` reports a hit after the game resolved it. `on_hit_pre(cb, user)` is called BEFORE: one call per impact on a character with a mutable `AbsHitPre` (`size`, `victim`, `attacker` (may be NULL / not a character), `attack_id`, `attack`, `perfect_link`, and five scales). Guard and absorb are decided later by the game, so they are not known yet. Returns 0 = ok, 1 = bad arguments / 16 callbacks, 2 = hooks failed.
* `damage_scale` multiplies the final damage, after the game's own multipliers. It is the value `on_hit` reports, so x2 doubles `on_hit` damage exactly.
* `special_ratio_scale` / `special_ratio` scale or force (`special_ratio` >= 0; -1 = scale only) the attacker's special ("cut") share of the raw damage, clamped to 0..1. The normal part is raw x (1 - share), the special part raw x share, each against its own defence.
* `stun_scale` multiplies the stun frames of the hit (hit, on guard, perfect link); melee attack rows only.
* `knockback_scale` multiplies the knockback distance (hit and guard).
* Out-of-range values (NaN, negative, > 100) are refused and logged, never passed to the game.

`damage_mod(character, key, value)` sets per-character multipliers that `on_hit_pre` starts from (victim `*_taken` x attacker `*_dealt`). Victim keys: `damage_taken`, `stun_taken`, `knockback_taken`. Attacker keys: `damage_dealt`, `stun_dealt`, `knockback_dealt`, `special_ratio`. Values are 0..100; 1 removes the multiplier. `character` NULL = the local player, kept across travel; other characters' entries are dropped when they are removed or the world changes. Example: `damage_mod(NULL, "damage_taken", 1.2f)`.

Poise is built on the game's resilience check: a victim whose resilience is greater than the attack's power (`EAttackPowers` 0 Light, 1 Medium, 2 Strong) absorbs the hit. It still takes the damage but is not interrupted. `poise_set(character, poise, heavy_power)`:
* While the gauge is above 0, resilience is forced to `heavy_power` (-1 = default 2). Lighter hits are absorbed and drain the gauge by their damage. The hit that empties the gauge is still absorbed.
* When the gauge is empty, resilience is forced to 0: the next hit interrupts and refills the gauge.
* A hit at or above `heavy_power` interrupts at once and refills the gauge.
* `poise_get` returns the gauge, or -1 when there is no poise. `poise <= 0` removes it. Poise replaces the character's own resilience while it is set.

Data: the content kind `poise` (`Mods/<mod>/content/poise/*.json`) is matched once per character at its first hit, by its ArchetypeAsset name or path (an `npcs` / `bots` entry id, a stock name such as `Trickster`, or the full path, case-insensitive). The last loaded entry wins:
```json
{ "archetype": "duelist", "poise": 30, "heavy_power": 2 }
```
Cost: hooks are installed at the first use; with nothing configured an impact costs one test, and nothing allocates. The first 120 impacts are logged in full (`hitpre: hit ...`, including whether the resilience branch was taken), then every 100th plus every poise event.
Ordering: the poise gauge is updated after the hit resolution returns. Depending on hook order (plugin load order), `on_hit` can be called before that update, so read `poise_get` from a timer, not from `on_hit`.

## Defence events (API 2.20)
`on_defense(cb(const AbsDefenseInfo *d, user), user)`: one call per successful defence, on the game thread, with the defender, the attacker, the attack row (`attack_id`, `attack`, `startup` frames, -1 for an item attack) and timing. The hooks are installed at the first registration. Returns 0 = ok, 1 = bad arguments / 16 callbacks, 2 = hooks failed.

| `type` | Source | Notes |
|---|---|---|
| `ABS_DEF_GUARD`, `ABS_DEF_GUARD_BREAK`, `ABS_DEF_ABSORB` | the hit resolution (the same outcome as `on_hit`) | absorb = the Kahlt special. Resilience / poise absorbs are not a defence here (see `on_hit_pre`). For a guard break, `defender` is whose guard broke. `perfect_link` = the defended hit was a perfect link |
| `ABS_DEF_PARRY`, `ABS_DEF_DEFLECT` | a successful parry | deflect = the parrier's guard was up (a parry from guard); parry = the special-ability parry. The two are told apart by that flag only |
| `ABS_DEF_DODGE`, `ABS_DEF_AVOID` | a successful avoid | `avoid_type` = EAvoidType: 4 Dodge = dodge; 0 Duck, 1 Jump, 2 LeftSide, 3 RightSide (Windfall), 5 AllHitAvoid, 6 DrunkenSouth (Stagger) = avoid; -1 for other types |

Timing: `time_in_attack` = real seconds from the attacker's `on_attack_start` of the same row to the defence (-1 when no start was seen, another attack started since, or more than 10 s passed); `frame_in_attack` = the same in 1/30 s combat frames, compare with `startup`. `delay` = the game's own prediction delay argument of parry / dodge / avoid (0 otherwise). The loader logs the first 150 events (`defense: #N ...`), then every 100th.
`on_hit` still reports parry / dodge as `ABS_HIT_PARRIED` / `ABS_HIT_DODGED`; `on_defense` gives the roles explicitly.

## Overlay (API 2.21; loader code `tools/lannative/overlay.cpp` + `overlayapi.inc`)

The loader draws Dear ImGui (1.91.5, `third_party/imgui`) over the game: its own menus (F1, F2) and any plugin window, through one hook on the swap chain's Present. Plugins register a draw callback instead of hooking Present themselves; two overlays that hook the same call fight over input and over the render target.

Five functions and one struct, appended at the end of `AbsApi` (check `api->size >= offsetof(AbsApi, overlay_raw_mouse) + sizeof(void *)`); `ABS_API_VERSION` stays 2 and older plugins are unaffected. A plugin must work without its window when the overlay is unavailable.

| Function | Threads | Returns |
|---|---|---|
| `overlay_register(self, draw, user)` | any (game thread, `AbsPlugin_Load`, a draw callback) | `0` ok; `1` bad arguments, the same `(draw, user)` twice, or the table of 16 callbacks (shared by all plugins) is full; `2` overlay unavailable |
| `overlay_unregister(self, draw, user)` | any | `0` removed (the callback may still run once in the frame in progress); `1` not found |
| `overlay_set_mode(self, flags)` | any | `0` ok; `1` bad arguments; `2` unavailable. `flags` = `ABS_OV_*`, `0` = nothing |
| `overlay_raw_mouse(self, &dx, &dy)` | any | `0` ok (the movement since the last call, cleared; zeros when nobody has the mode); `1` bad arguments or unavailable |
| `overlay_info(self, &info)` | any | `0` ok; `1` bad arguments or no frame yet (`info.state` = 0); `2` dead (`info.state` = -1) |

The loader takes a lock that the render thread holds while it draws, so these calls wait for a frame in progress: a slow draw callback delays them and the game's message pump. Install: the overlay is hooked at the first `overlay_register` or non-zero `overlay_set_mode`, never at start-up; registering from `on_engine_ready` or later is recommended. Only the first request does the work; later calls and other threads wait for it and see the same result.

**Modes** (`overlay_set_mode`, per plugin, ORed with the other plugins' modes; setting a mode replaces the plugin's previous one):

| Flag | Effect |
|---|---|
| `ABS_OV_DRAW` | your callbacks run; no input is taken (HUDs, viewport overlays that do their own picking) |
| `ABS_OV_MOUSE` | mouse messages go to ImGui and are kept from the game; a virtual cursor is drawn (the game pins the OS cursor, so the real cursor position is ignored). Check `io.WantCaptureMouse` yourself to tell ImGui windows from the world |
| `ABS_OV_KEYS` | keys and characters go to ImGui; they are kept from the game only while ImGui wants them (`io.WantCaptureKeyboard` or `io.WantTextInput`). F1-F12 (also as `WM_SYSKEYDOWN`) always reach the game, and so does a key release whose press the game saw; a press that was kept from the game takes its release with it |
| `ABS_OV_RAWMOUSE` | raw mouse movement goes to `overlay_raw_mouse` instead of the game and the virtual cursor (fly cameras, drag gizmos); the accumulator is shared, use it from one plugin at a time |
| `ABS_OV_BLOCK_GAME` | keeps all keyboard and mouse input from the game and gives the game a neutral gamepad, while any plugin has it (an editor that freezes the character). It gives ImGui nothing by itself: combine it with `MOUSE` / `KEYS`. Exactly: **kept from the game** are every key press and character except F1-F12, every mouse button, wheel and movement message, raw mouse movement, and the gamepad (the game's `XInputGetState` returns buttons, triggers and sticks at zero; the packet number and the return code are unchanged). **Still reaching the game** are F1-F12, the release of any key or button whose press the game saw (nothing stays held), window, focus and size messages, and everything the game reads outside the window procedure and `XInputGetState` (`GetAsyncKeyState` polling, rumble output). Nothing is blocked while the game window is not the foreground window. ImGui itself never reads the gamepad |
| `ABS_OV_HIDECURSOR` | the virtual cursor is not drawn while any plugin has it (use it with `RAWMOUSE` for a fly camera); a loader menu that is open still shows its cursor |

Frames run while any mode is set or a loader menu is open; a closed overlay costs one flag test per Present. Nothing is taken while the game window is not in the foreground; losing the focus clears ImGui's key and button state. ImGui never reads the gamepad and does not navigate with it (the loader resets the navigation flags every frame); the pad keeps driving the game unless `ABS_OV_BLOCK_GAME` is set. When `MOUSE` ends for every plugin while a button is down, ImGui's button state is cleared and the release of a press the game saw is delivered to the game, so nothing stays held. `overlay_info` reports the caller's own `caller_mode` and `caller_callbacks` (both 0 once the loader released the plugin), so a plugin can poll instead of guessing. A plugin that is switched off (3 faults) or unloaded loses its modes and callbacks.

**Draw callbacks** run on the **render thread**, between `NewFrame` and `Render`, after the loader's own windows, in registration order (no priorities). `user` is yours. Allowed: ImGui calls and reading your own data (copy game data in an `on_tick` callback under your own lock, or post work with `run_on_game_thread`). Not allowed: engine objects, `call_function*`, `load_object`, blocking calls. Every callback runs inside an exception guard and ImGui's stacks are put back after it: a `Begin` without `End`, a pushed style or an exception leaves the loader's windows and the plugins after you intact, and the loader logs `overlay: plugin X left N unbalanced ImGui stack entries - recovered`. The 3-fault counter is the plugin-wide one shared with every other callback; at 3 the loader logs `plugin X: EXCEPTION ... (fault 3 of 3) - its callbacks are now OFF` and `overlay: plugin X released (...)`. There is no event: call `overlay_info` (`state`) or check the log. If a frame itself faults three times the overlay is switched off for the run (`state` -1) and the loader's menus fall back to text.

**Binding your ImGui.** The plugin builds its own copy of Dear ImGui of the **same version** (the loader exports no ImGui functions; this struct is the whole contract) and binds it to the loader's context at the top of every callback. Compare the layout first; a mismatch means draw nothing:

```cpp
static void draw(void *user) {
    AbsOverlayInfo i; memset(&i, 0, sizeof i); i.size = sizeof i;
    if (A->overlay_info(A, &i)) return;
    if (i.imgui_version_num != IMGUI_VERSION_NUM || i.sizeof_imgui_io != (int)sizeof(ImGuiIO) || i.sizeof_imgui_style != (int)sizeof(ImGuiStyle) ||
        i.sizeof_imgui_drawvert != (int)sizeof(ImDrawVert) || i.sizeof_imgui_drawidx != (int)sizeof(ImDrawIdx) || i.sizeof_imgui_wchar != (int)sizeof(ImWchar)) return;
    ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)i.imgui_alloc, (ImGuiMemFreeFunc)i.imgui_free, i.imgui_alloc_user);
    ImGui::SetCurrentContext((ImGuiContext *)i.imgui_context);
    ImGui::Begin("My plugin"); /* ... */ ImGui::End();
}
```

Build your ImGui (version 1.91.5, `third_party/imgui`) with these lines in `imconfig.h`, or force-included the way the loader does (`tools/lannative/overlay_imconfig.h`):

```c
#define IM_ASSERT(_EXPR) ((void)0)        /* the default assert would run outside the loader's recovery; RaiseException is fine too */
```

The loader's own build also defines `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`, `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`, `IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS`, `IMGUI_DISABLE_DEMO_WINDOWS` and `IMGUI_DISABLE_DEBUG_TOOLS`; none of them changes a structure layout, so a plugin may leave them out. Do **not** define `IMGUI_USE_WCHAR32`, `IMGUI_USE_BGRA_PACKED_COLOR`, `ImDrawIdx` / `ImDrawVert` overrides or any other layout option: the check against `overlay_info` (sizes of `ImGuiIO`, `ImGuiStyle`, `ImDrawVert`, `ImDrawIdx`, `ImWchar`) then makes the plugin draw nothing. The context is shared: `io` and style changes are global, so restore what you change; window positions and sizes are **not persisted** by the loader (`io.IniFilename` is NULL; use `SaveIniSettingsToMemory` / `LoadIniSettingsFromMemory` to keep a layout yourself; a per-plugin ini service may be added later without changing these entries). Only ImGui's default font exists and its size follows `ui_scale` (`io.FontGlobalScale`); the loader does not create textures or fonts for plugins yet (a font and texture API can be added later without changing these entries). `d3d_device` / `d3d_context` are given for plugins that draw with D3D themselves (the loader restores the render target after its draw; do not keep the pointers across a resize).

**Hello window**, for a plugin that also runs on loaders without the overlay:

```c
__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    if (api->version != ABS_API_VERSION || api->size < sizeof(uint32_t) * 2) return 1;      /* not a loader this plugin can run on */
    A = api;
    if (api->size >= offsetof(AbsApi, overlay_raw_mouse) + sizeof(void *)) {
        if (api->overlay_register(api, draw, NULL) == 0) api->overlay_set_mode(api, ABS_OV_DRAW | ABS_OV_MOUSE);
    }
    return 0;                                                                                  /* the rest of the plugin works without a window */
}
```

`overlay_unregister` and `overlay_set_mode(self, 0)` undo it. Log lines (`LanNative\lannative.log`): `plugin X: overlay_register -> N`, `overlay: plugin X mode 0xA -> 0xB` (only when the mode changes), `overlay: Present / ResizeBuffers hooked`, `overlay: ImGui 1.91.5 up on window ...`, `overlay: first frame drawing`, `overlay: EXCEPTION in the overlay frame (n of 3)`.

### Loader menus

`menu_style = imgui` (default) or `text` in `LanNative\config.txt` (or `menu_style.txt`) selects how the loader's own F1 / F2 menus are drawn; `text` keeps the plain text menus and does not install the overlay for them. It does not affect plugins: a plugin that registers still installs the overlay. The F1 / F2 key bindings and the keyboard navigation are the same in both styles; the overlay adds mouse clicks. The Mods page shows the load order, each mod with its state and the reason it was skipped, content warnings (including pak shadowing), conflicts, the content registry, plugin faults, the last reload and the reload / plugin menu actions.

## Functions for automated checks (`test_*`, API 2.6 and 2.8)
For plugins that check a mod in the running game without a keyboard (for example: spawn an NPC, let it attack, compare the `on_hit` damage with the expected value). A *character* is an `AFightingCharacter`: the player, an NPC, the practice dummy. Everything needs the game thread, is checked against this game build before its first use (a mismatch switches only that call off, logged) and is guarded.

| Function | What it does |
|---|---|
| `test_heal(self, character)` | health and guard to full, ghost damage and the down state cleared; 1 = ok |
| `test_set_invincible(self, character, on)` | `HealthComponent::ServerSetInvicible` (what the practice mode uses); the flag is read back; returns 0 / 1 or -1 |
| `test_attack(self, attacker, "004", target)` | the character performs attack row `004` (any row of the attacks table, also names like `BT2_01`) aimed at `target` (NULL = straight ahead) through the game's own attack order: no keyboard, no deck needed. Row `"@deck"` = the first attack of its own deck in its current stance through the normal combo path. 1 = launched (confirm with `on_attack_start` / `on_hit`) |
| `test_guard(self, character, on)` | raise (1) / drop (0) the guard |
| `test_dodge(self, character, dir[3])` | starts a dodge towards the WORLD direction (x, y; z ignored); a dodge that is active when an attack connects ends in `ABS_HIT_DODGED`. 1 = launched |
| `test_teleport(self, actor, loc[3], yaw*)` | places an actor (yaw NULL keeps the rotation; falls back to a plain move when another capsule blocks the spot) and notifies the character of the teleport, as the stock training flow does. Never use it on an AI NPC: place NPCs with `npc_spawn` |
| `test_get_location(self, actor, out[3])`, `test_get_forward(self, actor, out[3])` | world position; unit vector the actor faces |
| `test_spawn_npc(self, archetype_path, loc*)` | the same as `npc_spawn(self, archetype_path, loc, NULL, NULL)`; returns the character |
| `test_ai(self, character, enabled)` | stop (0) / restart (1) the NPC's behaviour tree: a stopped NPC stands still and only does what `test_attack` / `test_guard` / `test_dodge` tell it |
| `test_hittable(self, character, on)` | an AI character can only be hit while its AI is running; `on` = 1 sets the flag the hit check reads so a stopped NPC takes driven attacks, 0 clears it. Returns the resulting value or -1 |
| `AbsCharInfo` (2.6 fields) | `invincible`, `is_down`, `guarding`, `attacking` (set `out->size = sizeof *out`) |

How a driven attack works: the attack is launched with the game's debug flag that skips the combo deck, so any attack row plays; it starts in the row's start quadrant, else the character's stance. Whether it can hit is decided later by the victim (an AI character only while its AI runs or `test_hittable` is set) and by plain geometry.

Limits:
* Driven attacks do not land on the **practice dummy**: use a spawned NPC as the target of hit checks; the dummy is fine for start-up and reset checks.
* `test_guard` on the **player** is cleared by the player's own input handling the next frame (`AbsCharInfo.guarding` reads 0): let an NPC be the defender.
* The player's facing follows the camera; a teleport does not turn the player. Place the other character in front of the player along `test_get_forward` instead.
* Whether a driven dodge avoids an attack depends on timing and direction; many dodges simply make the attack miss without a dodge event.
* `test_spawn_npc` / `npc_spawn` work for the host / single player only.
* Teleporting the player far (to coordinates from another session or world) can drop it through ground that is not streamed in yet: keep player moves short and spawn NPCs near the player instead.

## Mod hygiene
No API functions; loader behaviour every mod meets. User-facing rules: `docs/MODDING-GUIDE.md` section 14.
* **Pak shadowing.** When two mounted paks hold the same asset path with different content, the loader logs `mods: WARNING <winner pak> (read order N) shadows <loser pak> (M): ...`, writes it under "Pak shadowing" in `LanNative\mods.txt` and shows a `WARNING:` line on the F2 Mods page.
* **`supersedes`** (`mod.json`): the named paks of other mods are not mounted while the superseding mod is loaded and ships a pak.
* **Equip rule** (`items` `equip`): an item is equipped only when it is granted for the first time, only one mod item per slot (first in load order wins), and never over a weapon the loader did not equip. Every refusal is logged (`content: equip X skipped (slot S): ...`).
* **NPC aggro** (`npcs` / `bots` `aggro`): the engage sequence described under "npcs entry"; `on_area` is also attached for an `aggro` entry that has a `radius` but no reactions.

## More documentation
* `docs/sdk/README.md`: task-oriented guide with copy-paste examples, a plugin from zero, logging, safe mode, hot reload and the traps list.
* `docs/sdk/API-REFERENCE.md`: per-function reference generated from the header (`python tools/sdk/gen_api_reference.py` after every header change).
* `docs/sdk/LUA.md`: gameplay logic in Lua instead of C.
* Scaffolds: `tools/sdk/New-Plugin.ps1`, `tools/sdk/New-ContentMod.ps1`.
* `docs/MODDING-GUIDE.md`: paks, `rows.json`, `mod.json`, and every content kind without code.

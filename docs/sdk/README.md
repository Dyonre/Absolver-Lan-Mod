# Build a mod on the Absolver script extender

Task-oriented guide. Reference: `docs/sdk/API-REFERENCE.md` (generated from `AbsolverSE.h`), behaviour details: `docs/PLUGIN-API.md`, data mods: `docs/MODDING-GUIDE.md`, Lua: `docs/sdk/LUA.md`.
Offline / private play only (game 1.31, UE 4.18.3). The loader does nothing without `-NoEAC`.

## 1. Pick the kind of mod

| I want to... | Use | Start with |
|---|---|---|
| change numbers of moves / stats, add attack rows | `rows.json` | `examples/FastJab` |
| add items, weapons, mask clones, shop entries | content kind `items` (no code) | `New-ContentMod.ps1 -Kind items` |
| add a fighting style button | content kind `styles` (no code) | `New-ContentMod.ps1 -Kind styles` |
| new files (animations, meshes, icons, maps) | a `.pak` cooked for UE 4.18.3, plus rows/content that point to it | MODDING-GUIDE section 4 |
| gameplay logic without writing C | a Lua script (needs the LuaHost plugin, which is not in the release zip: you build it once from `tools/luahost`) | `docs/sdk/LUA.md`, `examples/ContentMods/LuaDemo` |
| new logic, UI, events, automated checks, your own content kind | a plugin DLL (C/C++) | `New-Plugin.ps1` |

Scaffolds (never touch the game folder; default output `work\sdk-out`):

```powershell
cd <repo>
powershell -ExecutionPolicy Bypass -File tools\sdk\New-Plugin.ps1 -Name MyPlugin -Build        # plugin.c build.bat plugin.cfg mod.json, then compiles
powershell -ExecutionPolicy Bypass -File tools\sdk\New-ContentMod.ps1 -Name MyMask -Kind items  # mod.json + content\items\MyMask.json  (-Kind weapon | styles, -RowsJson)
python tools\sdk\gen_api_reference.py                                                           # refresh docs\sdk\API-REFERENCE.md after AbsolverSE.h changes
```

Both scaffolds take `-Dest <folder>` (parent folder of the new mod) and `-Force` (overwrite generated files).

## 2. Project layout of a mod

One folder in `<game>\Mods\` (game = `<game>`). The folder name is the mod's name everywhere. Everything is optional; combine freely:

```
Mods\MyMod\
  mod.json            id, version, api, requires, after, affects_gameplay
  rows.json           table / data-asset edits
  content\items\*.json, content\styles\*.json, content\<your kind>\*.json
  scripts\*.lua       Lua scripts (run by LuaHost)
  plugin.dll          (+ plugin.cfg: key = value, read with config_get)
  *.pak               directly in the folder (not a subfolder)
  icon.png            for "icon_file"
```

`mod.json` (all keys optional, `id` defaults to the folder name):

```json
{ "id": "MyMod", "name": "My Mod", "version": "0.1.0", "api": 2, "author": "you", "description": "...",
  "requires": [ { "id": "Lib", "min": "1.0" } ], "after": ["OtherMod"], "affects_gameplay": true }
```

`requires` missing/off/too old = this mod (pak, rows, content, plugin) is skipped and shown on screen and in `mods.txt`. Load order = requires/after, then `Mods\loadorder.txt` (last line wins; `-Name` switches a mod off). Cosmetic-only mods set `"affects_gameplay": false`.

## 3. Content kinds and every field

A file in `content\<kind>\` holds one entry object, an array, or `{ "<kind>": [ ... ] }`. Every entry needs `id` (letters, digits, `_`, max 63). Bad entries are skipped and logged `content: <kind> <mod>/<file> '<id>' INVALID: <reason>`. Same id in two mods: the later one in load order wins (`content: CONFLICT`). Unknown kinds are skipped with a log line; unknown fields are logged and ignored.

### `items` (clones a stock item into a new one under `/Game/Mods/<mod id>/<id>`)

| Field | Meaning |
|---|---|
| `id` | required |
| `base` | stock asset path to copy, `/Game/DB/Equipment/Masks/SillyFace_Mask_DataAsset.SillyFace_Mask_DataAsset`, `/Game/DB/Weapons/Sword/SwordPike.SwordPike`. Must already be in a pak (never an object that is not) |
| `name`, `description` | display text (one language; also written into the gear row) |
| `icon` | texture path (a cooked texture in your pak is best for released mods) |
| `icon_file` | `"icon.png"` next to `mod.json` (PNG/JPEG/BMP/GIF/TIFF, up to 4096 px a side): becomes a runtime texture; wins over `icon` |
| `props` | `{ "m_Prop": "engine text" }`, any property as `rows.json`-style text |
| `shop`, `shop_copy_of`, `price` | add a row to a shop table (`EquipmentShop`, `WeaponShop`, ...) cloned from `shop_copy_of`, with the price |
| `gear_row` | `{ "table": "WeaponDataTable", "copy_of": "w006", "set": { "m_eGearRarity": "Rare" } }`: the item gets its own row `<mod id>_<id>` in that `/Game/DB/Gear/` table |
| `moves` | weapons: `["321", ...]` attack rows allowed in the weapon family's deck only while the item is equipped (the rows still need `m_bWith<Family>` True via rows.json) |
| `grant` | give it to the loaded character once its inventory is in (an owned item is not added twice) |
| `equip` | equip after granting |

```json
{ "items": [ { "id": "MyMask", "base": "/Game/DB/Equipment/Masks/SillyFace_Mask_DataAsset.SillyFace_Mask_DataAsset",
               "name": "My Mask", "grant": true, "equip": true } ] }
```

### `styles` (a button in Meditation > Combat Style)

| Field | Meaning |
|---|---|
| `id` | required |
| `db` | a `FightingStyleDB` asset (usually from your pak) |
| `style_id` | 0-63; new styles start at 6 (0 Forsaken ... 5 Faejin) |
| `index` | progression index = button slot, 5-15 |
| `name`, `icon` | optional; written into the DB's `m_Name` / `m_StyleSmallIcon` |
| `gleam_level` | optional, 1-99: the style is unlocked only when the game says so or the character's Gleam level reaches this value (Faejin = 6). Without it the style is always unlocked |

Needs `LanNative\symbols.bin` (hooks `BPF_IsFightingStyleUnlocked` / `BPF_GetFightingStyleByUnlockIndex` by symbol). Other kinds (`powers`, `npcs`, `maps`, `bots`, `ranks`, `bot_trials`) are described in MODDING-GUIDE sections 10c-10f and 15; plugins can add their own (`register_content_kind`, `docs/PLUGIN-API.md`).

### `rows.json` (short form; full: MODDING-GUIDE sections 5-6)

```json
{ "format": 1, "edits": [
  { "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 6 } },
  { "table": "attacks", "add": "MyMod_Kick", "copy_of": "004", "set": { "m_RealAttackName": "INVTEXT(\"Kick\")" } },
  { "object": "/Game/DB/Guard/GuardDB.GuardDB", "ops": [ { "append": "m_GuardAnimations.m_Array[0].m_Array", "value": "AnimSequence'\"/Game/Animations/MyMod/G.G\"'" } ] } ] }
```

Name new rows with your mod prefix; prefer `append` to `set`/`insert` on shared lists.

## 4. A plugin from zero to running

1. `New-Plugin.ps1 -Name MyPlugin -Build` (the header `AbsolverSE.h` comes from `-Sdk <folder>`, else the `GLEAM_SDK` environment variable, else `tools\lannative` of a repository clone; the `sdk` folder of the release zip works as that folder, so no clone is needed; needs MSVC Build Tools 2022 x64; `build.bat` calls `vcvars64.bat` from `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`). Output `work\sdk-out\MyPlugin\plugin.dll`.
2. Copy `plugin.dll`, `mod.json`, `plugin.cfg` to `<game>\Mods\MyPlugin\`. Start the game (offline, `-NoEAC` through the launcher). The loader appends `#MyPlugin` to `Mods\plugins.txt` and refuses to load it (a plugin is arbitrary code).
3. Remove the `#`, start again. Look in `Absolver\Binaries\Win64\LanNative\lannative.log` for `plugin MyPlugin: ...`.

Hand-written skeleton (the scaffold has the same plus config and logging):

```c
#include <windows.h>
#include <stddef.h>
#include "AbsolverSE.h"               /* build.bat adds /I tools\lannative */
static const AbsApi *A;
#define HAS(f) (A->size >= offsetof(AbsApi, f) + sizeof(void *))   /* newer fields: check first */

static void tick(float dt, void *u) { /* game thread, every frame, keep it cheap */ }
static void hit(const AbsHitInfo *h, void *u) { A->log(A, "hit %s dmg %.1f outcome %d", h->attack, h->damage, h->outcome); }

__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    if (api->version != ABS_API_VERSION || api->size < sizeof(AbsApi)) return 1;   /* 0 = ok */
    A = api;
    A->on_tick(A, tick, NULL);
    if (HAS(on_hit)) A->on_hit(A, hit, NULL);
    return 0;
}
```

Every API call takes `A` as first argument. Recipes:

```c
/* react to a Blueprint event by name; return nonzero from pre to skip the original */
static void after(void *obj, void *fn, void *params, void *u) { /* ... */ }
A->on_call(A, "BP_CharacterMenu_C", "EnterState FightingStyle", NULL, after, NULL);

/* hook a native C++ function by PDB name (needs LanNative\symbols.bin); call the returned "next" function */
static int (*next_unlocked)(void *self, int idx, int mode, int mentor);
static int my_unlocked(void *self, int idx, int mode, int mentor) { return idx == 6 ? 1 : next_unlocked(self, idx, mode, mentor); }
next_unlocked = A->hook_symbol(A, "UStatsComponent::BPF_IsFightingStyleUnlocked", my_unlocked);

/* read/write properties */
float hp; char buf[64];
A->get_property_text(A, obj, "m_fHealth", buf, sizeof buf);
float *p = (float *)A->prop_ptr(A, obj, "m_fHealth");              /* you know the type from the SDK dump */

/* a content kind of your own */
static int val(const char *json, char *err, int cap, void *u) { return 1; }
static int app(const char *mod, const char *id, const char *json, void *u) { A->log(A, "apply %s/%s", mod, id); return 0; }  /* write it idempotent: hot reload calls it again */
A->register_content_kind(A, "mykind", val, app, NULL);             /* in AbsPlugin_Load */

/* Mods page (F2) entry, persistent data */
A->add_menu_item(A, "do the thing", cb, NULL);
A->kv_set(A, "lastrun", "1"); A->kv_get(A, "lastrun", out, sizeof out);   /* LanNative\data\<plugin>.txt, never the game's save */
```

Find names and offsets in the SDK dump: call `A->dump_sdk(A, "mine")` (game thread) and read `LanNative\sdk\mine.txt` (every class / function with offsets, parameter lists, native addresses). Names are short without the `U`/`A` prefix. To learn what the game calls when you do something: `A->trace(A, "on", "FightingStyleSelection,SetState")`, do it, `A->trace(A, "dump", NULL)`, read `LanNative\trace\trace_NNN.txt`. The dump and traces are game-derived: never commit or share them.

The `test_*` functions (`test_heal`, `test_set_invincible`, `test_attack`, `test_guard`, `test_dodge`, `test_ai`, `test_hittable`, `test_spawn_npc`, ...) let your own plugin drive fights without a keyboard for automated checks; see `docs/PLUGIN-API.md`.

## 5. Logging and diagnosing

* `LanNative\lannative.log` (rotating): all loader lines, tags `mods:`, `rows:`, `content:`, `events:`, `plugin <Name>:`. Log generously (the scaffold does); log the first few calls of each callback.
* `LanNative\mods.txt` (load order, conflicts, split assets), `rows.txt` (per-mod edit results), `launcher.log`.
* Crash watch: the loader logs the first 40 access violations with module + offset (your DLL is named). The last ones before a crash show the culprit. The game's crash report is in `%LOCALAPPDATA%\Absolver\Saved\Crashes`.
* After 3 faults in your callbacks the loader switches your plugin off and logs which one; code inside a hook detour is NOT guarded.
* F2 = Mods page: load order, skipped mods with reasons, conflicts, content registry state, plugin faults, last reload.
* Run once with `-NoMods` to confirm a problem is a mod.

## 6. Safe mode

Hold **Shift** at start, pass `-SESafe`, or create `LanNative\safe.txt`: no mods, no plugins, no extender hooks. Two starts in a row that never reach the first game tick turn it on automatically; one clean start turns it off. If the game will not start after your change, start in safe mode, then bisect with `-Name` in `loadorder.txt`.

## 7. Hot reload

Re-reads `Mods\*\content\**\*.json` and `rows.json` without a restart. Trigger: the first line of the F2 Mods page, create `LanNative\reload.txt` (read and deleted within a second), or `A->reload_content(A)`. Works: rows.json value changes, item `name`/`description`/`icon`/`icon_file`/`props`/`price`/`gear_row.set`/`grant`, new items, style `name`/`icon`, `apply` of plugin kinds for changed/new entries. Needs a restart: item `base`/`shop`/`gear_row.table`/`moves`, style `db`/`style_id`/`index`, removed edits, data-asset ops, new mods. Full table: MODDING-GUIDE section 8b. A syntax error rejects the whole set and keeps the old edits.

## 8. Traps

1. **Native calls vs UFunction calls.** `on_call` / `trace` / `replace_native` only see calls through `ProcessEvent` / the Blueprint VM. Native code calling a C++ function directly (for example `UStatsComponent::BPF_IsFightingStyleUnlocked` from the menu code) is invisible to them: use `hook_symbol` on the C++ function. `trace_function` helps find which kind a call is.
2. **The class `WidgetTree` is a template.** The Meditation menu is built when it opens; the objects under `...BP_CharacterMenu_C:WidgetTree` are templates. Hook the `EnterState ...` event post and take the live widget from the menu instance's property (`*prop_ptr(menu, "FightingStyleTable")`). A widget you create lacks the archetype overrides (visibility, focus flags) and the menu keeps resetting it to hit-test-invisible: force Visibility 0 on it and its ancestors on a short timer.
3. **School item type.** An inventory item granted with a zeroed `m_eSchoolItemType` (0 = a mentor/school item) is deleted on the next load. Always use 4 (None); `inventory_add` does. `BPF_EquipOutfit` alone desyncs the menu: use `inventory_equip` (`BPF_Equip`).
4. **rows.json and objects that are not in a pak.** A value naming a runtime-created or unloaded object hangs the boot (no first tick). The loader skips such values with a log line (`rows: ... is not loaded and its package is in no pak`); do not rely on that, reference only stock or packed assets. Soft object properties are unaffected. Create runtime objects at `on_engine_ready` (before the save loads) so saved paths resolve by find-only.
5. **Header-size rule.** Editing a COOKED `.uasset` (UAssetAPI): the header size must stay identical (no added names/imports, same-length renames only) or the game stops with "Serialization Error: Corrupt data found". UE 4.18 also uses the name-map hashes: a renamed name needs both `NonCasePreservingHash` and `CasePreservingHash` recomputed, otherwise e.g. material texture parameters silently load as None. New packages under `/Game/Mods/<Mod>/` are free of this.
6. **30 fps.** Animations made at 60 fps must be resampled to 30 fps or HUD gauges and timings stretch. Combat time is frames x 1/30 s advanced by real delta time: use the `dt` the game passes, never a fixed step.
7. **Threads.** `on_object_loaded` runs on loader threads: return fast, no `call_function*`, `load_object`, `for_each_object`, `dump_sdk`; queue with `run_on_game_thread`. `load_object` and creating assets need the game thread.
8. **Hooks.** `expected` bytes are always the game's original bytes, also when the function is already hooked. Always call the returned "next" function unless you mean to replace it. Detours are not crash-guarded: keep them tiny.
9. **Events.** `on_attack_start` is deduped (the game re-simulates `OrderAttack::OnStart` 6-30x); `on_death` fires BEFORE the `on_hit` of the killing blow; `on_pawn_spawned` can run before the controller is possessed (`is_player` 0 for the title-screen player); `on_hit` has no attacker for plain `on_damage`.
10. **Content hot reload** is not a restart: write `apply` idempotently; removed entries stay until restart.
11. **Removing a mod** must leave the game bootable: items vanish from the list and slots show empty. Do not rely on the item being in the save.
12. **Game-derived output** (`symbols.bin`, `LanNative\sdk\*`, `trace\*`, extracted assets, screenshots, cooked paks of game assets) is never committed or shared. Plugins load only when listed in `plugins.txt`.

## 9. Check, package and install a mod

Build the check tools once: `tools\lannative\test\build_tools.bat` (MSVC Build Tools 2022 x64) makes `reccheck.exe`, `modscan.exe` and `ueapply.exe` in `tools\lannative\test\`. `Pack-Mod.ps1` and `Install-Mod.ps1` use `reccheck.exe` when it is there.

| Step | Command | What it does |
|---|---|---|
| check | `tools\lannative\test\reccheck.exe <Mods folder> [--dump] [--alone]` | rows.json + records (flats / templates / base) + manifests with the loader's own code: load order, skipped mods, bad fields, fields several mods set; `--dump` = the final value of every field and who set it; `--alone` = do not check `requires` (one mod on its own) |
| package | `powershell -ExecutionPolicy Bypass -File tools\sdk\Pack-Mod.ps1 -ModDir <mod folder> [-Out work\packages] [-Secret <text>]` | `<id>-<version>.zip` with the folder `<id>\` and `files.sha256`. Refuses loose cooked files (`.uasset` / `.uexp` / `.ubulk` / `.umap`), `.sig`, saves, `.env` / key files, executables and scripts (`.exe`, `.bat`, `.ps1`, any `.dll` other than `plugin.dll`), build outputs (`.obj`, `.lib`, `.pdb`, ...), personal data, and any file containing a `-Secret` text (8 characters or more); runs `reccheck --alone`. `mod.json` needs an `id` |
| install | `powershell -ExecutionPolicy Bypass -File tools\sdk\Install-Mod.ps1 -Zip <zip> -ModsDir <Mods folder> [-AllowPlugin]` | one folder per mod; checks every hash; an older copy is moved to `ModsReplaced\`, never deleted; `plugin.dll` only enabled (a line in `plugins.txt`) with `-AllowPlugin`; then checks the whole Mods folder with `reccheck`. Refuses the real game folder (`steamapps\common\Absolver`): there, copy the mod folder into `<game>\Mods\` yourself |
| remove | `... Install-Mod.ps1 -Remove <id> -ModsDir <Mods folder>` | moves the folder to `ModsRemoved\` |

Lua mods: `docs/sdk/LUA.md`.

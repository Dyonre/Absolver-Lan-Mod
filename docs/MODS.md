# How a mod works

Absolver mods have usually been paks that replace game files: one mod per file, the last one wins, and nothing checks whether two mods fit
together. With the loader a mod is a folder. It can change single fields of game tables (several mods can change the same table), create new
game objects from JSON, run code, declare what it needs, and be switched on and off from the game. The loader runs only with `-NoEAC`
(offline / LAN).

## Folder
One folder per mod in `<game>\Mods\`. Every part is optional:

```
Mods\MyMod\
  mod.json              id, version, dependencies, flags
  rows.json             edits of game tables and data assets
  content\<kind>\*.json data the loader turns into game objects (docs/CONTENT.md)
  plugin.dll            native plugin (64-bit) + plugin.cfg for its settings
  scripts\*.lua         Lua scripts (run by the LuaHost plugin, docs/LUA.md)
  *.pak                 packed game files (cooked for UE 4.18.3), mounted by the loader
```

Paks placed directly in `Absolver\Content\Paks` (or a subfolder of it) are not mods of the loader: in Modded starts it mounts only the
game's own `pakchunk0`-`2` from there, and the Mods tab counts the skipped ones (`LanNative\mods.txt` lists them). Put each one in its own folder under `Mods\`
(`AbsolverModLauncher.exe --migrate` moves the ones directly in `Content\Paks`; move paks in subfolders by hand), or set `content_paks = all` in `LanNative\config.txt` to load them as well. Vanilla
starts load them as before.

## mod.json
```json
{ "id": "MyMod", "name": "My Mod", "version": "1.0.0", "api": 2, "author": "...", "description": "...",
  "requires": [ { "id": "Lib", "min": "1.0" } ], "after": ["Other"], "incompatible_with": ["Rival"],
  "supersedes": ["zzzOldMod.pak"], "affects_gameplay": true, "settings": [ ... ] }
```
| Key | Meaning |
|---|---|
| `id` | letters, digits, `_ - .`; default = folder name; a second mod with the same id is skipped |
| `version` | dotted number, compared by `requires.min` |
| `api` | lowest plugin API version the mod needs (2) |
| `loader` | the loader version the mod was built for, for example `"5.4"` (the SDK scaffolds fill it in). A mod built for an older major version, for a newer loader, or without the field is marked "may be out of date" in the Mods tab (the tab header counts them); the mod still loads |
| `requires` | mods that must be present, enabled and at least `min`; otherwise this mod is skipped |
| `after` | load after these mods when they are present |
| `incompatible_with` | mod ids that must not load together with this one |
| `supersedes` | pak file names of other mods that this mod replaces; they are not mounted while this mod ships a pak |
| `affects_gameplay` | `false` = cosmetic only (looks, textures); left out of the LAN mod check. Default `true` |
| `shareable` | `true` = a host may offer this mod's `.pak` files to joiners whose mods differ (pak-only gameplay mods: `affects_gameplay` not `false`; `docs/HOSTING.md` section 4). Default `false` |
| `net` | who needs the mod in a LAN game, shown as a tag on the Mods tab: `host_only` (only the host runs it), `local` (it affects only the player who has it), `vanilla_visible` (players without it still see its effect through the game's own sync) or `all` (every player needs it). A label only: the loader enforces nothing; a plugin can check `net_role` itself |
| `joiners_need` | `false` = joiners can play without this mod: it is left out of the LAN mod check and not offered by the host mod download, like a cosmetic mod, but stays a gameplay mod everywhere else. Default `true` |
| `settings` | fields shown as a form on the Mods tab (F1); the values go to the mod's `plugin.cfg` |
| `name`, `author`, `description` | display only |

## Load order
1. Mods switched off on the Mods tab (or with a line `-Name` in `Mods\loadorder.txt`) are skipped.
2. Mods whose `requires`, `api` or `incompatible_with` cannot be met are skipped, together with every mod that needs them.
3. The rest is ordered by `requires` and `after`; `loadorder.txt` breaks ties (a later line wins). New folders are added at the end (highest priority).

The same order applies to paks, `rows.json`, content and plugins. It is written to `LanNative\lannative.log` and `LanNative\mods.txt`;
skipped mods and their reason are also shown on screen.

## Start sequence
1. The game loads `version.dll`. Without `-NoEAC` the loader stops here.
2. Settings are read; safe mode (Shift held, `-SESafe`, or two failed starts in a row) skips everything below.
3. Mods are scanned and ordered; their paks are mounted.
4. Plugins load. A `plugin.dll` loads only if its folder is listed without `#` in `Mods\plugins.txt`; the loader adds `#Name` the first time
   it sees a new plugin. It calls `int AbsPlugin_Load(const AbsApi *api)`; return 0 to stay loaded.
5. On the first game tick content entries are applied and `on_engine_ready` callbacks run.
6. `rows.json` edits are applied each time the game loads the table or asset they target.
7. At exit `AbsPlugin_Unload()` is called if the plugin exports it.

## Conflicts
The Mods tab and `mods.txt` list paks that overwrite each other's files, `rows.json` fields set by several mods (the later mod wins that field)
and duplicate content ids (the later mod wins). Content and `rows.json` reload without a restart from the first line of the Mods tab.

## Loader window (F1)
F1 opens the loader window; F2 opens the world menu instead (each key switches to the other window and closes its own). Esc or the window's X closes the loader window, Q / E or LB / RB (gamepad) switch tabs.
* **Tabs**: Mods (load order, on / off switches, buttons for each mod's actions, the profile bar, a Details section that opens when something needs a look), Keybinds, Hosting, Settings, Log, and one tab for each plugin that asks for one (`ui_tab`, `docs/API-REFERENCE.md`); a mod without a tab keeps its settings form and rows on the Mods tab.
* **Hosting**: a Hosting ON / OFF switch (`docs/HOSTING.md`), the host mod download setting, the player limit and the Net Inspector.
* **Settings**: the loader's own options from `LanNative\config.txt` in a form (`docs/SETTINGS.md`); they apply after a restart. Number fields take typed numbers or - / + steps within the setting's range.
* **Bundled mod**: the package ships Cheats (`Mods\Cheats`); the installer copies it and switches it on in `Mods\plugins.txt` the first time. Remove the line to switch it off.
* **Profiles**: a profile is a named copy of the on / off + order set, `Mods\profiles\<name>.txt` in `loadorder.txt`'s format. Type a name in the
  Profile box at the top of the page and choose `Save as` (a new profile) or `Update` (the name of an existing profile: it is overwritten, the old
  file is kept as `<name>.txt.bak`); the box starts with the current profile, else the last one used since the game started. The profile rows switch to a profile (`loadorder.txt` is rewritten,
  the old one kept as `loadorder.txt.bak`; it takes effect after a restart) or save the current set under a suggested name. Names: letters,
  digits, space, `-`, `_`, up to 40 characters, no space at either end. A profile applies to the mods installed now: mods it does not name keep
  their state and go last, names of mods that are not installed are left out. Up to 32 profiles are listed. No profile exists by default;
  rename or delete a profile by its file.
* **Keybinds tab**: every action a plugin declares with `bind_action`, and every `on_key` key, has a row. Choose it and press the new key, mouse button
  or gamepad button (Esc cancels, Delete unbinds, Backspace restores the default). An input that another action uses in an overlapping context
  is shown as a warning, never refused. Changed binds are saved in `LanNative\binds.txt`, one line each: `Mod/action = K` (`Mod` = the plugin's folder name; or `F8`, `Mouse 4`,
  `Pad A`, `none`). Actions are grouped under the mod that owns them. Keys that the loader, the game, Steam or a mod read directly are listed below as fixed keys.
* **Log tab**: the last 3000 lines of `LanNative\lannative.log`, with a filter box (text or mod
  name), `Errors only` and each line's mod tag. It is read-only. While a text box has the keyboard, the menus
  ignore keys.
* **Tags**: the Holds column lists what a mod contains and its `net` / `joiners_need` tags (hover for their meaning).

## Saves
Objects a mod creates live under `/Game/Mods/<mod id>/`. Items granted to a character are saved by that path; when the mod is removed they
are dropped at the next save. Plugins keep their own data outside the game's save files (`kv_get` / `kv_set`, `sidecar_get` / `sidecar_set`).

## LAN
Paks, `rows.json`, `content\` and allowed `plugin.dll` files of every mod with `affects_gameplay` true and `joiners_need` not `false` form a fingerprint that host and joiner compare (`modmismatch`,
`docs/SETTINGS.md`). A plugin runs only on the PC that has it; other players see its effects only where the game itself sends them.
Pak-only gameplay mods marked `shareable` can be downloaded from the host (`docs/HOSTING.md` section 4). The Net Inspector rows on the Hosting tab show,
for every character in the world, the body, animations and outfit pieces it uses and which mod or pak provides them; a joiner also sees
the host's view and which of those assets it is missing.

## Starter mod
A data mod: `Mods\FasterJab\rows.json` (`examples/FastJab` also adds a new move)
```json
{ "format": 1, "edits": [ { "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 8 } } ] }
```
`LanNative\rows.txt` lists each edit as applied or skipped with the reason.

A plugin (template: `examples/PluginTemplate`; scaffold: `tools/sdk/New-Plugin.ps1 -Name MyPlugin -Build`, needs MSVC Build Tools 2022 x64):
```c
#include <windows.h>
#include <stddef.h>
#include "AbsolverSE.h"
static const AbsApi *A;

static void on_hit(const AbsHitInfo *h, void *user) { A->log(A, "hit %s damage %.1f", h->attack, h->damage); }

__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    if (api->version != ABS_API_VERSION || api->size < offsetof(AbsApi, on_hit) + sizeof(void *)) return 1;
    A = api;
    A->on_hit(A, on_hit, NULL);
    return 0;
}
```
Copy `plugin.dll` (and the `mod.json` that `New-Plugin.ps1` writes; it is optional) to `Mods\MyPlugin\`, start the game once, remove the `#` before `MyPlugin` in `Mods\plugins.txt`, start
again. Lines tagged `plugin MyPlugin:` appear in `LanNative\lannative.log`. Every API function takes `A` as its first argument; check
`api->size` before using a function newer than your header. Reference: `docs/API-REFERENCE.md`.

# Changes

## 5.2.0

**LAN play: no join key any more.** 5.2.0 has no join key, no failed-login lock-out and no `maxpeers` limit; anybody who can reach a host on the network can find and join its game. On a shared or public VPN network (a public Radmin or Hamachi room) set `allow` lines in `LanNative\config.txt` so that only your friends can join (`docs/HOSTING.md`). An `allow` line that is not valid now closes the host (every join refused, the line named on screen) instead of being skipped; `#` starts a comment and up to 64 lines are read. Old `joinkey`, `joinprotect` and `maxpeers` lines and `joinkey.txt` are ignored with a log line.

**Mod paks in `Content\Paks` no longer load in Modded starts.** The loader mounts only the game's own paks (`pakchunk0`-`2`) and the `Mods` folder; the Mods page counts the skipped paks and `LanNative\mods.txt` lists them. Move each one into its own folder under `Mods\` (`AbsolverModLauncher.exe --migrate` moves the ones directly in `Content\Paks`) or set `content_paks = all` to load them as before. Vanilla starts load them as before.

**LAN**
* LAN games list: at any altar (or F1 > `LAN games...`) the loader searches the network and lists the hosts that answer, with map, players and whether the mods match; Enter joins. The search uses UDP 7778 (padded queries, at most 20 smaller replies per second per host); `lanfinder = off` turns it off.
* Hosts add `?bIsLanMatch` to every listen travel, so a host whose Steam is online can be joined by address; a joiner no longer acts as a host.
* F1 > `Kick player...` (host): removes a joined player with the game's own kick.
* Player limit `maxplayers` (default 6, the host included; `2`-`16` or `off`).
* Host mod download: a host offers the paks of its pak-only gameplay mods marked `"shareable": true` in `mod.json`; a joiner whose mods differ sees the offer and installs them only on request (`moddownload = prompt | off`, TCP 7779). A downloaded pak with a packed path that leaves the game folders refuses the whole download.
* Autoleave waits 15 seconds after any travel request, so a host's map change no longer sends joiners home.

**Menus and keys**
* One input owner: while a plugin window has the keyboard, the menus leave it alone, and while a menu or another window has it, plugins' `on_key` callbacks get only F keys. One mouse cursor when the game shows its own.
* Gamepad in the F1 / F2 menus (Back + Start opens F1; `pad_menu`).
* Map list: categories and type-to-search.
* Key registry: the loader's keys, every plugin key and keys the game or Steam take are listed on the Mods page; a key with two owners is logged as a clash. `keep_keys` (default `F11`) keeps keys from the game in Modded starts (with the ImGui menus, the default).
* Plugin API 2.23: `declare_key`, `input_owner`.

**Mods page (F2)**
* Net Inspector: for every character in the world, the body, animations and outfit pieces it uses and which mod or pak provides them; a joiner also sees the host's view and what it is missing.

**Fixes and internals**
* `call_function_params` refuses an unreadable argument pointer (for example an `args` array shorter than the function's inputs) instead of crashing, and logs the function's inputs.
* The host-mods side channel refuses a unit verb named `LIST` or `GET`.
* The loader's features register themselves (`units.inc`, `lnreg.h`, `lnhooks.h`; `tools/lannative/README.md`). The log lists every registration table after the version line.
* The launcher's window and the installer describe the new `Content\Paks` rule.

## 5.1.0

**Starting the game**
* Steam's Play button now asks how to start: Modded (the loader and the `Mods` folder, EasyAntiCheat off) or Vanilla (the original EasyAntiCheat launcher, no loader, online play works). Playing online no longer needs an uninstall.
* A Vanilla start renames `Binaries\Win64\version.dll` to `version.dll.off`; the next Modded start renames it back. `Uninstall.bat` removes either file.
* "Remember my choice" saves the answer in `LanNative\launch.txt`; holding Shift while the game starts asks again. `-Modded` or `-Vanilla` in the Steam launch options skips the question.
* Paks placed directly in `Content\Paks` are no longer moved into `Mods` at every start, so they load unchanged in both modes. `AbsolverModLauncher.exe --migrate` still moves them on request.

**Docs**
* Short reference docs replace the long guides: `docs/MODS.md` (how a mod works, a starter mod), `docs/CONTENT.md` (`rows.json` and the content kinds), `docs/SETTINGS.md` (config keys, switches, keys), `docs/ADVANCED.md` (hooks, names, threads, failure handling, limits), `docs/API-REFERENCE.md` (grouped by topic) and `docs/LUA.md`.
* Examples: `FastJab` (a data mod) and `PluginTemplate` (a plugin) remain; the other examples were removed.
* The release zip carries these docs instead of `docs/MODDING-GUIDE.md`.

The loader itself is unchanged apart from its version number.

## 5.0.0

The LAN mod (LanNative 4.3.1) grew into a full mod loader and script extender. It is the same `version.dll`, with the LAN play of 4.3.1 still built in. Inside the code, the log and the file properties it is called Gleam Loader. LanNative 4.3.1 stays available as tag `v4.3.1`.

### New since LanNative 4.3.1

**Installing**
* `Install.bat` / `Uninstall.bat`: find the game through Steam, install the loader and a small launcher (the original `Absolver.exe` is kept as `Absolver.exe.eac`), create the `Mods` folder, and check GitHub once for a newer release (`-NoUpdateCheck` skips it). The game no longer has to be patched by hand.
* `-NoMods` in the Steam launch options starts the game without any mod.

**Mods** (`docs/MODS.md`)
* A mod is a folder in `<game folder>\Mods\`; `Mods\loadorder.txt` sets the order and switches mods on and off.
* `rows.json`: edit table rows and data assets (move stats, frame data, shop rows, decks and more). Every edit is checked first; a bad edit is skipped on its own and logged.
* Pak mods: `.pak` files in a mod folder are mounted in load order; conflicts and split assets are reported.
* Content without code: items, fighting styles, powers, NPCs, map scripts, bots and bot trials from JSON files in `content\`.
* `mod.json`: name, version, required API, `requires`, `after`, `incompatible_with`, `supersedes`, gameplay flag and a list of settings.
* Lua scripts (through the LuaHost plugin, built from `tools/luahost`) and C plugins (`docs/MODS.md`, `docs/API-REFERENCE.md`, scaffold scripts in `tools/sdk`).
* Offline checkers `reccheck` (rows.json, mod.json, load order, paks) and `modscan` (pak conflicts).

**In game**
* F1 menu (hosting, joining, NPC spawning, loader settings) and F2 Mods page: switch mods on and off, reorder them, edit each mod's settings, see conflicts, hot reload. The menus are drawn with Dear ImGui (`menu_style = text` for the plain text menu).

**LAN play** (`docs/HOSTING.md`)
* Hosting and joining moved from F6 / F7 to the F1 menu (`Host`, `Join address`, `Join`, `Bring joiners to me`).
* Join protection: a join key by default, an optional address allow list, a player limit and a lockout after failed tries.
* Both sides compare their gameplay mods when a player joins (`modmismatch = warn | block | off`).
* The Forsaken parry fix stays on by default (`parryfix = off` turns it off).

### Changes since Gleam Loader 0.2.2 (the last loader release, published separately)
* F2 Mods page: switch mods on and off, reorder them, show conflicts, and edit per-mod settings declared in `mod.json`.
* `mod.json`:
  * `incompatible_with` is honoured; only mods that end up loading exclude others, and mutual pairs skip the later mod in load order.
  * A single string is accepted wherever a list is expected (`"requires": "Lib"`).
  * Two mods whose `after` entries form a loop both load, with a warning, instead of both being skipped.
  * A mod that requires a mod skipped by a cycle is reported as such, not as part of the cycle.
  * Lists longer than 8 entries are cut with a warning.
* A `plugin.dll` that is not a 64-bit Windows DLL is refused with a log line, and a failed plugin load never opens a Windows error box (a broken `plugin.dll` could stop the game's start).
* Plugin API 2.22: `for_each_property` lists a class's or struct's properties.
* The overlay no longer stacks a second Present hook on top of another module's patch.
* Weapon kinds without their own deck label use "Weapon".
* Removed: the developer test modes for bots, the old `staff_deck` sidecar migration, and the developer screenshot runner.
* The installer checks this repository for new releases.

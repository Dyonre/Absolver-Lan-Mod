# Changes

## 5.0.0

The LAN mod (LanNative 4.3.1) grew into a full mod loader and script extender. It is the same `version.dll`, with the LAN play of 4.3.1 still built in. Inside the code, the log and the file properties it is called Gleam Loader. LanNative 4.3.1 stays available as tag `v4.3.1`.

### New since LanNative 4.3.1

**Installing**
* `Install.bat` / `Uninstall.bat`: find the game through Steam, install the loader and a small launcher (the original `Absolver.exe` is kept as `Absolver.exe.eac`), create the `Mods` folder, and check GitHub once for a newer release (`-NoUpdateCheck` skips it). The game no longer has to be patched by hand.
* `-NoMods` in the Steam launch options starts the game without any mod.

**Mods** (`docs/MODDING-GUIDE.md`)
* A mod is a folder in `<game folder>\Mods\`; `Mods\loadorder.txt` sets the order and switches mods on and off.
* `rows.json`: edit table rows and data assets (move stats, frame data, shop rows, decks and more). Every edit is checked first; a bad edit is skipped on its own and logged.
* Pak mods: `.pak` files in a mod folder are mounted in load order; conflicts and split assets are reported.
* Content without code: items, fighting styles, powers, NPCs, map scripts, bots and bot trials from JSON files in `content\`.
* `mod.json`: name, version, required API, `requires`, `after`, `incompatible_with`, `supersedes`, gameplay flag and a list of settings.
* Lua scripts (through the LuaHost plugin, built from `tools/luahost`) and C plugins (`docs/PLUGIN-API.md`, `docs/sdk/README.md`, scaffold scripts in `tools/sdk`).
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
* Plugin API 2.22: `for_each_property` lists a class's or struct's properties.
* The overlay no longer stacks a second Present hook on top of another module's patch.
* Weapon kinds without their own deck label use "Weapon".
* Removed: the developer test modes for bots, the old `staff_deck` sidecar migration, and the developer screenshot runner.
* The installer checks this repository for new releases.

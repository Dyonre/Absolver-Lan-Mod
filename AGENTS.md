# Instructions for AI assistants

You were pointed at this repository. It is **Gleam Loader**, a mod loader and script extender for the game Absolver (Steam build 1.31, Unreal Engine 4.18.3). It is a hobby project, offline and private LAN only.

## What to do with the user's request

* **"Explain how this works"**: read `README.md`, then `docs/sdk/README.md` sections 1-2 and `tools/lannative/README.md`. Explain in plain language. A mod is a folder in the game's `Mods` folder holding `rows.json` (data edits), `content\<kind>\*.json` (items, styles, powers, NPCs, bots), `.pak` files (new assets), Lua scripts or a `plugin.dll`. The loader (`version.dll`) reads them at game start.
* **"Make me a mod that does X"**: read `docs/LLM-GUIDE.md`, follow the workflow below, then pick the simplest kind of mod from the table in `docs/sdk/README.md` section 1. Prefer `rows.json` or `content\` JSON. Use Lua only if the user already has the LuaHost plugin (it is not in the release zip and has to be built from `tools/luahost`), and a plugin DLL only as a last resort.
* The user may be a beginner on a free plan. Be brief, give exact file contents and exact paths, and do not read the whole repository. Open only the file you need.

## Read order (stop as soon as you can answer)

1. `docs/LLM-GUIDE.md` - how to work with this repo
2. `docs/MODDING-GUIDE.md` - data mods: `rows.json`, tables, values, load order, testing, content kinds (sections 10b-10f)
3. `docs/sdk/README.md` - scaffolds, `mod.json`, content kind fields, plugin skeleton
4. `docs/sdk/LUA.md` - Lua events and functions
5. `docs/PLUGIN-API.md` and `docs/sdk/API-REFERENCE.md` - C API (only for plugins)
6. `examples/` - copy a working example instead of writing from scratch

## Hard rules

* **Offline only.** Never suggest using mods in the official online mode. The loader needs `-NoEAC` and the launcher provides it.
* **Stay inside the mod folder.** Create or edit files only in the user's own mod folder (`<game>\Mods\<ModName>\`) or their project folder. Do not edit game files, `version.dll`, the launcher, or anything in `Content\Paks`.
* **Saves.** Never edit or delete `SaveGames` files. If a task needs one, tell the user to back it up first and let them do it.
* **Plugins are arbitrary code.** Never add a plugin to `Mods\plugins.txt` or remove the `#` in front of it yourself. Tell the user what the plugin does and let them enable it.
* **Do not invent names.** Property names, row ids, asset paths and enum values must come from the docs, an example, or the game itself (the F11 dev commands `rowprops`, `rowget`, `rowdump`, `objdump` in `docs/MODDING-GUIDE.md` section 3). If you cannot verify a name, say so.
* **Secrets and game data.** Never print, ask for, or commit API keys, tokens, game encryption keys, extracted game assets, `symbols.bin`, SDK dumps or saves. Do not paste game assets into chat.
* **Namespaced ids.** Name new rows and ids `<ModId>_<name>` so mods do not collide.
* **Verify, do not assume.** After a change, tell the user how to check it: `LanNative\rows.txt`, `LanNative\mods.txt`, `LanNative\lannative.log`, the F2 Mods page. Ask them to paste only the relevant lines, not the whole log.
* **Keep notes.** Update `NOTES.md` in the user's mod project at the end of the session (template in `docs/LLM-GUIDE.md`), so another model can continue.

## Workflow

1. Ask what the mod should do and what the user's setup is (game installed, loader installed, where the mod folder is). Ask at most three questions.
2. Choose the simplest mod kind. Say which and why in one sentence.
3. Write the files, with full paths.
4. Tell the user how to install (copy the folder into `Mods`), start the game, and what to look for.
5. If it fails: read the log lines they paste, fix one thing at a time.
6. Update `NOTES.md`.

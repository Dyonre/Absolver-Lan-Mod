# Absolver LAN Mod

A mod loader, script extender and LAN host for **Absolver** (Steam build 1.31). Offline and private LAN play only. Inside the code, the log
and the file properties it is called **Gleam Loader**. It started as **LanNative** (still available as tag `v4.3.1`).

## Disclaimer

This is a hobby project, provided as is. I take no responsibility for anything that happens from using it. Not affiliated with Sloclap. No
support is offered; if you want changes, fork it.

Much of the code and documentation was written with the help of AI tools.

## What it does

* **Mods**: one folder per mod in `<game>\Mods\`: `rows.json` edits of game tables, new items / powers / NPCs / styles / map scenes from
  JSON, paks, Lua scripts and native C plugins. Dependencies and load order from `mod.json`; conflicts are listed.
* **In game**: **F1** menu (hosting, joining, LAN games list, kick, map list with categories and search, options), **F2** Mods page
  (switch mods on and off, reorder, settings, conflicts, reload, Net Inspector, key owners). Keyboard, mouse or gamepad.
  `-NoMods` starts without mods.
* **Plugin API**: hooks on game functions, calls by name, properties, events, spawning, timers, menus, an ImGui overlay.
* **LAN play**: host and join from the F1 menu over a LAN or VPN, a list of LAN games at every altar, LAN duels, an optional address
  allow list, a player limit, a host kick, a mod check when a player joins and a download of the host's shareable pak mods.
* **Fixes**: the Forsaken "cursed parry" fix, on by default.

## Multiplayer and VPNs

The official online mode is not supported and almost certainly will not work with this loader (it needs EasyAntiCheat off). Do not use mods
online; choose Vanilla when the game starts to play online without the loader. To play with friends use a private LAN, or a VPN /
virtual LAN over the internet. That is your responsibility: only play with people you trust, never port-forward the game to the
internet, and read the terms of the VPN software you choose. There is no join key: on a shared or public VPN network set `allow` lines
so that only your friends can join.
See `docs/HOSTING.md`.

## Installation

1. Close the game. Download the latest zip from the **Releases** page and unzip it anywhere.
2. Run `Install.bat`. It finds the game through Steam, copies the loader in, keeps the original `Absolver.exe` as `Absolver.exe.eac`, and
   creates a `Mods` folder. It also asks GitHub whether a newer release exists (nothing is downloaded; `Install.bat -NoUpdateCheck` skips
   it). The loader itself never goes online.
3. Press Play in Steam as usual. A window asks how to start: **Modded** (the loader and your mods, offline and private LAN only) or
   **Vanilla** (the unmodified game with EasyAntiCheat, for the official online mode). Tick "Remember my choice" to skip the window; hold Shift while the game starts to see it again (`docs/SETTINGS.md`).

**Mod paks placed directly in `Content\Paks` do not load in Modded starts**: the loader mounts only the game's own paks and the `Mods`
folder. Put each one in its own folder under `Mods\` (`AbsolverModLauncher.exe --migrate` moves them), or set `content_paks = all` in
`LanNative\config.txt` to load them as before. They still load in Vanilla starts.

To uninstall, run `Uninstall.bat`. Steam's "Verify integrity of game files" also undoes the install.

Features that hook game functions by name need a `symbols.bin` made from your own copy of the game, placed in
`Binaries\Win64\LanNative\` (`docs/ADVANCED.md`): plugins using `hook_symbol`, fighting-style buttons, `"learn": "always"` rows, hit / damage /
defence events, powers, weapon kinds and NPC targeting. Without it those are off and the rest works.

Antivirus software may flag `version.dll` because it is an unsigned DLL that patches the game in memory. See [SECURITY.md](SECURITY.md).

## Documentation

| File | Content |
|---|---|
| [docs/MODS.md](docs/MODS.md) | how a mod works, `mod.json`, load order, a starter mod |
| [docs/CONTENT.md](docs/CONTENT.md) | `rows.json` and the content kinds |
| [docs/API-REFERENCE.md](docs/API-REFERENCE.md) | plugin API, generated from `tools/lannative/AbsolverSE.h` |
| [docs/SETTINGS.md](docs/SETTINGS.md) | `config.txt` keys, command-line switches, keys |
| [docs/ADVANCED.md](docs/ADVANCED.md) | hooks, finding names, threads, failure handling, limits |
| [docs/LUA.md](docs/LUA.md) | Lua scripts through the LuaHost plugin |
| [docs/HOSTING.md](docs/HOSTING.md) | hosting and joining |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | changes per release |

`examples/` holds a starter data mod (`FastJab`) and a plugin template (`PluginTemplate`); `tools/` the loader source, launcher, installer, SDK scaffolds and LuaHost.

## License

MIT, see [LICENSE](LICENSE). Third-party notices are in `tools/release/THIRD-PARTY-NOTICES.txt`.

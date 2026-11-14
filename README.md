# Gleam Loader

A mod loader and script extender for **Absolver** (Steam build 1.31). Put a mod in a folder, start the game, and it loads. Mods can change move stats, add items, weapons, fighting styles, powers, NPCs and bots, run Lua scripts, or load plugins written in C. Offline and private LAN play only.

## Disclaimer

This is a hobby project, provided as is. I take no responsibility for anything that happens from using it. Not affiliated with Sloclap. No support is offered; if you want changes, fork it.

## Multiplayer and VPNs

The official online mode is not supported and almost certainly will not work with this loader (it needs EasyAntiCheat off). Do not use mods online. If you want to play with friends, use a private LAN connection; to play over the internet, use a VPN or virtual LAN. That is your responsibility: only play with people you trust, keep the join key private (the loader requires one by default), never port-forward the game to the internet, and read the terms of whatever VPN software you choose. See `docs/HOSTING.md`.

## Installation

1. Close the game. Download the latest zip from the **Releases** page and unzip it anywhere.
2. Double-click `Install.bat`. It finds the game through Steam, copies the loader in, keeps the original `Absolver.exe` as `Absolver.exe.eac`, and creates a `Mods` folder. It also asks GitHub whether a newer release exists (nothing is downloaded; `Install.bat -NoUpdateCheck` skips it). The loader itself never goes online.
3. Press Play in Steam as usual.

To add a mod, put its folder in `<game folder>\Mods\`. If a mod stops the game from starting, add `-NoMods` to the game's Steam launch options.

To uninstall (or to play the official online mode), run `Uninstall.bat`. Steam's "Verify integrity of game files" also undoes the install; run `Install.bat` again afterwards.

Basic mods (`rows.json`, paks, items and other content mods) work right after install. A few features that hook game functions by name, such as plugins that use `hook_symbol` or fighting-style buttons, also need a `symbols.bin` made from your own copy of the game; without it the loader logs that these are off and everything else keeps working (see `docs/PLUGIN-API.md`).

Antivirus software may flag `version.dll` because it is an unsigned DLL that patches the game in memory. See [SECURITY.md](SECURITY.md).

## Making mods

Using an AI assistant? Point it at this repository and see [docs/LLM-GUIDE.md](docs/LLM-GUIDE.md). Otherwise start with [docs/MODDING-GUIDE.md](docs/MODDING-GUIDE.md).

## Where things are

* `docs/` - guides: `MODDING-GUIDE.md` (data mods), `sdk/README.md` (scaffolds and plugins), `sdk/LUA.md` (Lua), `PLUGIN-API.md` (C API), `HOSTING.md` (LAN)
* `examples/` - small working mods and plugins to copy from
* `tools/` - loader source, launcher, installer, scaffolding and packaging scripts

## License

MIT, see [LICENSE](LICENSE). Third-party notices are in `tools/release/THIRD-PARTY-NOTICES.txt`.

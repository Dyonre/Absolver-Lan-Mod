# Absolver LAN Mod

A mod loader, script extender and LAN host for **Absolver** (Steam build 1.31). Put a mod in a folder, start the game, and it loads. Offline and private LAN play only.

It started as **LanNative**, a LAN-only mod, and grew into a full mod loader: the same `version.dll`, the same LAN hosting, plus everything below. Inside the code, the log and the file properties the loader is called **Gleam Loader**. LanNative 4.3.1 is still available as tag `v4.3.1` on the Releases page.

## Disclaimer

This is a hobby project, provided as is. I take no responsibility for anything that happens from using it. Not affiliated with Sloclap. No support is offered; if you want changes, fork it.

## What it does

**Mods**
* Data mods: change table rows and data assets (move stats, frame data, shop prices, decks and more) with a `rows.json`, checked before each edit reaches the game (`docs/MODDING-GUIDE.md` sections 5-6).
* Pak mods: new or replaced game files, mounted from each mod's folder in a load order you control (section 4).
* Content without code: new items, fighting styles, powers, NPCs, map scripts and bots from JSON files (sections 10b-10f).
* Lua scripts and C plugins for new game logic (`docs/sdk/LUA.md`, `docs/PLUGIN-API.md`).
* `mod.json` for a mod's name, version, requirements, incompatibilities and settings (section 9).

**In game**
* **F1** menu: hosting, joining and the loader's settings. **F2**: the Mods page, where mods are switched on and off, reordered and configured, and conflicts are listed (section 8b).
* Hot reload of mods, a mod check when a player joins, and `-NoMods` to start without mods if one stops the game from starting.

**LAN play** (from LanNative)
* Host and join from the F1 menu, over a LAN or a VPN.
* Join protection: a join key by default, an address allow list and a player limit (`docs/HOSTING.md`).
* The fix for the Forsaken "cursed parry" bug, on by default (`parryfix = off` in `LanNative\config.txt` turns it off).

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

## Coming from LanNative

* Hosting and joining moved from F6 / F7 to the **F1** menu (`Host`, `Join address`, `Join`).
* Settings stay in `LanNative\config.txt` and the host address in `LanNative\ip.txt`, in the same folder as before.
* Joining now needs the host's join key unless the host sets `joinprotect = open` (`docs/HOSTING.md` section 2).

## Making mods

Start with [docs/MODDING-GUIDE.md](docs/MODDING-GUIDE.md).

## Where things are

* `docs/` - guides: `MODDING-GUIDE.md` (data mods), `sdk/README.md` (scaffolds and plugins), `sdk/LUA.md` (Lua), `PLUGIN-API.md` (C API), `HOSTING.md` (LAN)
* `examples/` - small working mods and plugins to copy from
* `tools/` - loader source, launcher, installer, scaffolding and packaging scripts

## License

MIT, see [LICENSE](LICENSE). Third-party notices are in `tools/release/THIRD-PARTY-NOTICES.txt`.

# Content mods (data only, no plugin.dll)

Drop a folder from here into `<game>\Mods\` (offline / private play, needs the LanNative script extender build with the content registry).
The loader's content registry (`tools/lannative/content.inc`, guide `docs/MODDING-GUIDE.md` section 10b) reads
`Mods\<mod>\content\<kind>\*.json`; there is no code in these mods. All ship a `mod.json` (id, version, api); `requires`, `after` and `affects_gameplay`
are optional (MODDING-GUIDE section 9).

* `ExampleMask` - `content/items/mask.json`: a clone of the SillyFace mask, granted and equipped once a world is loaded.
* `WideWave` - `content/powers/widewave.json`: a new power, a Shockwave clone with a 450 radius (stock 280), own ItemAttack row, 3 shards, 4 s cooldown, own name,
  the stock Exhaust animation as stand-in, granted and equipped in power slot 0 (key 1). Guide: `docs/MODDING-GUIDE.md` section 10c.
* `MapScriptDemo` - `content/maps/demo.json`: a map script placed relative to the player in the open world: a light that cycles colour and pulses, a second light
  behind a `phase:party` flag, a cube gate that slides up with collision off when the player steps into a trigger box (and closes when leaving), a "GATE OPEN" banner.
  Guide: `docs/MODDING-GUIDE.md` section 10e.
* `CloneDemo` - `content/clones/duelist.json`: player-clone bots, second local players with their own character, driven by a bot brain (F1 > Mods: Clones).
  Brains need `bot_ai = on` and a brain preset mod.
* `LuaDemo` - `scripts/`: example for the Lua host (`docs/sdk/LUA.md`): your Jab (row 004) does 3x damage. Needs the LuaHost plugin listed in `Mods/plugins.txt`.
* `RecordsDemoA`, `RecordsDemoB` - `rows.json`: the records data layer. A's flat retunes several moves; B overrides one of A's flats, so every row that inherits it
  changes. Need `records = on` in `LanNative/config.txt`.

## One attacks-table pak per install

A pak that carries a whole copy of a table (for example `DB/Attacks/attacks.uasset`) is read from ONE pak only, the one with the highest read order, so two copies
never merge: the older copy can hide the newer rows.
* A mod can list the paks it replaces in `mod.json` (`"supersedes": ["<old pak>.pak"]`): the loader then does not mount them (log `mods: WARNING ... superseded`).
* Without that hint the loader still tells you: `mods: WARNING <pak A> (read order N) shadows <pak B> (M): k file(s) differ, e.g. ...` in the log, in
  `LanNative\mods.txt` ("Pak shadowing") and as a `WARNING:` line on the F2 Mods page.
* Pak read order = `10000 + 1000 * position-in-the-load-order + pak index`; the load order is `requires` / `after` first, then `loadorder.txt`
  (see MODDING-GUIDE section 9). A mod that comes later wins.

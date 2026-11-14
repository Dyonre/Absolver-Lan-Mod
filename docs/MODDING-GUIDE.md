# Making a mod for the Absolver mod loader

This guide is for people who want to **make** mods. If you only want to install and run mods, read `README.txt` in the release zip (also `tools/installer/README.txt` in the repository).

Before you start: install Gleam Loader (the release zip). Your mods go in the `Mods` folder next to `Absolver.exe` in the game folder, for example `steamapps\common\Absolver\Mods\` inside your Steam library. The installer creates it.

It applies to Absolver 1.31 (Unreal Engine 4.18.3).

Offline / private LAN play only. The loader refuses to do anything unless the game was started with `-NoEAC` (the launcher does that). Never use mods online.

---

## 1. Your first mod in five minutes

Goal: make the basic Jab start 4 frames sooner and hit harder, and add a new move.

In the base game the Jab (row `004`) has `m_iFrameBuildUp` 10 (frames until it hits) and `m_fDamageMax1` 38.2. Other mods can change these values, so the numbers you see in game may differ.

1. Make a folder in the game's `Mods` folder: `Mods\FastJab\`
2. Put one file in it, `rows.json`:

```json
{
  "format": 1,
  "edits": [
    { "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 6, "m_fDamageMax1": 55.0 } },

    { "table": "attacks", "add": "FastJabPlus", "copy_of": "004", "learn": "always", "set": {
        "m_Name": "FastJabPlus",
        "m_RealAttackName": "INVTEXT(\"Fast Jab Plus\")",
        "m_eStartQuadrant": "BackRight",
        "m_iFrameBuildUp": 4
    } }
  ]
}
```

3. Start the game. That is all: no pak, no tool, no cooking.

The new move sets `m_Name` to its own row name (the game identifies a move by it) and `"learn": "always"`, which makes it appear in the deck editor without editing your save.

What happened: when the game loaded its attack table, the loader changed row `004` (the Jab) and added a new row named `FastJabPlus` that starts as a copy of the Jab. Check `LanNative\rows.txt` (in `Absolver\Binaries\Win64\`): it says how many edits applied and lists any that failed, with the reason.

A copy of this example is in `examples\FastJab`. `examples\HeavyJab` is a second mod that changes the same Jab; run both and look at `LanNative\mods.txt` to see how the loader reports and resolves the clash (section 8).

---

## 2. What a mod is

A mod is **one folder** in `<game folder>\Mods\`. Inside it can be any of:

| File | What it is for |
|---|---|
| `rows.json` | Changes to the game's **data**: table rows (moves, stats) and data assets (animation databases). Section 5 and 6. |
| `*.pak` | New or replaced **files**: animations, icons, textures, maps, blueprints. Section 4. |
| `mod.json` | Information about the mod (id, version, dependencies, "cosmetic only"). Section 9. |
| `content\<kind>\*.json` | Data-only content: new items, fighting styles, powers, NPCs, map scripts, bots. Sections 10b-10f. |
| anything else | Ignored (put your README here). |

A mod can be only a `rows.json`, only a pak, only `content\` files, or several of them (a style mod is typically a pak with animations plus a `rows.json` with the data that uses them).

Rules:

* The folder name is the mod's name everywhere (logs, load order, the LAN warning). Use letters, digits, `_`, `-`, `.`; at most 95 characters (a longer folder name is skipped with a warning). Keep the `id` in `mod.json` equal to the folder name. Item and other content ids (section 10b) are stricter: letters, digits and `_` only.
* Paks must sit **directly** in the mod folder (not in a subfolder). If a mod has several paks they are mounted in file-name order, later name wins.
* You do not need a `.sig` file: the loader creates a missing one next to the pak (a copy of the game's own signature file, which the game accepts for any pak).
* Up to 256 mods.

### Load order

`Mods\loadorder.txt` is created and kept up to date by the loader:

```
# Absolver mod load order. Last line wins when two mods change the same file.
MyArena
FastJab
-HeavyJab
```

The **last line wins**. A `-` in front of a name switches the mod off. New folders are added at the end (highest priority) the next time the game starts. If two mods set the same field, the later one wins; mods that only add things (rows, list elements) all take effect.

### Which kind of mod do I need?

| I want to... | Use |
|---|---|
| change numbers of a move (speed, damage, range...), add a move | `rows.json` with `table` edits (section 5) |
| add animations to the guard / dodge / hit / idle databases | `rows.json` with `object` edits (section 6) |
| add new animations, icons, textures, a map | a pak (section 4), plus a `rows.json` if something must point to them |
| replace a texture only | a pak, and `"affects_gameplay": false` in `mod.json` (section 9) |
| add an item, style, power, NPC, map script or bot | `content\<kind>\*.json` (sections 10b-10f) |
| change game logic (blueprint graphs, code) | not possible with data edits; see "Limits", section 12 |

Prefer `rows.json` over replacing a table in a pak: a pak that contains the whole `attacks` table can never be combined with another mod that also touches it, a `rows.json` can.

---

## 3. Finding what to change

### Tables and properties

The game's data lives in **tables** (rows of properties) and **data assets** (one object with properties). Their paths look like `/Game/DB/Attacks/attacks` (the table's package) and the object `/Game/DB/Attacks/attacks.attacks`.

* F11 `rowprops` / `rowdump` (below) list every property of a row with its current value.
* To explore anything else, use the developer commands (below) or extract the game files with a tool such as FModel (you need the game's AES key from the community; do not publish extracted game files).

### Developer commands (F11)

The loader has a small developer console driven by a file, so you can look inside the running game:

1. Create `Absolver\Binaries\Win64\LanNative\lmtest.txt`. Put one command per line.
2. Create `LanNative\devkeys.txt` (any content); it switches on the developer keys F9 / F10 / F11.
3. Start the game, load into the world, and press **F11**. The commands run in order.
4. Results appear in `LanNative\lannative.log` (lines starting `rows:`) and, for dumps, in files next to it.

Without `lmtest.txt`, F11 does nothing. Delete both files when you are done.

| Command | What it does |
|---|---|
| `tbl <table>` | Is the table loaded? How many rows? (`<table>` = `/Game/DB/Attacks/attacks.attacks`) |
| `rowprops <table>` | Lists every property of the table's rows: name, kind, offset |
| `rowget <table> <row> <property>` | Shows one value |
| `rowset <table> <row> <property> <value>` | Sets one value right now (to try a value; same validation as rows.json) |
| `rowprobe <table> <row> <property>` | Asks the **game's own** lookup for that row on a live character and shows the value it sees (proves the edit reached the game) |
| `rowfull <table> <first> <last>` | Writes every property of the numeric rows `first`..`last` to `rowfull.txt` |
| `rowdump <table> <first> <last>` | Writes those rows as a ready-made `rows.json` (`rowdump.json`): each row as a copy of the most similar other row plus the differences |
| `rowapply <table>` | Applies all loaded mods' edits to that table again |
| `objdump <object>` | Writes every property of a data asset as text to `objdump_<Name>.txt` |
| `objrt <object> <property>` | Checks that a property can be exported and imported back unchanged |
| `url` | Shows the join URL the LAN code would use (without joining) |

Example `lmtest.txt`:

```
tbl /Game/DB/Attacks/attacks.attacks
rowget /Game/DB/Attacks/attacks.attacks 004 m_iFrameBuildUp
rowprobe /Game/DB/Attacks/attacks.attacks 004 m_iFrameBuildUp
objdump /Game/DB/Guard/GuardDB.GuardDB
```

---

## 4. Pak mods (new files)

Use a pak for anything that is a file: animations, meshes, textures, icons, maps, blueprints.

* The assets must be **cooked for Unreal Engine 4.18.3** (the game's version). Cook them with the matching engine, or export them with the community tools.
* Pack with `UnrealPak.exe` (UE 4.18): write a response file with one line per file, `"<path on disk>" "../../../Absolver/Content/<path inside the game>"`, then
  `UnrealPak.exe MyMod.pak -create=resp.txt`. The mount point is `../../../`, so a file ends up at `Absolver/Content/...`.
* Put the `.pak` in `Mods\<YourMod>\`. Name it anything.
* **Do not ship files that replace a vanilla file when you can edit it instead** (section 2, "Which kind of mod do I need?"). Two mods that ship the same file conflict; the later one wins and the other one's change is lost.
* When you edit a cooked `.uasset` (for example with UAssetAPI) its header size must stay identical, or the game stops with "Serialization Error: Corrupt data found".

### Conflicts between paks

The loader reads every pak's file list at start and writes `LanNative\mods.txt`:

* **Conflicts**: the same file in two mods. The later mod wins. This is what you ordered, so it is only information.
* **Split assets** (dangerous): an Unreal asset is several files (`X.uasset` or `X.umap` plus `X.uexp`). The game serves each file from the highest mod that has it. If two mods each ship only *part* of the same asset, the game gets a mix of two versions and usually crashes when it loads that asset (`Serial size mismatch`). The loader lists these as `SPLIT ASSET` in the log and in `mods.txt`. Fix: switch one mod off, or reorder so one mod supplies the whole asset.

Tools in the repository: `tools\pakls.py` (list a pak's files), `tools\lannative\test\modscan.exe` (offline conflict report for a whole `Mods` folder; section 13).

---

## 5. `rows.json`: editing table rows

### Format

```json
{
  "format": 1,
  "edits": [ ... ]
}
```

A top-level array of edits is accepted too. `//` comments and trailing commas are allowed. The file may be up to 8 MB.

Two kinds of table edit:

**Change a row**

```json
{ "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 8, "m_bCanBeMirrored": false } }
```

**Add a row**

```json
{ "table": "attacks", "add": "MyKick", "copy_of": "004", "set": { "m_iFrameBuildUp": 12 } }
```

* `table` is the table's short name (`attacks`).
* `row` / `add` is the row name. Row names are text: ids like `004` or any name you like (`MyKick`).
* `add` makes a new row as a **copy of `copy_of`** at that moment, then applies `set`. So the new row inherits earlier edits to the source row. If a row with that name already exists (another mod added it) it is kept and your `set` is applied to it.
* `set` maps property name to value. Values may be JSON strings, numbers or `true`/`false`.
* An attack row you add should also set `m_Name` to its own row name (the game identifies the move by it: learned state, new-move marks). The loader does not check that the two match.
* `"learn": "always"` on an `add` of an `attacks` row: the move counts as learned for every character, so it shows in the deck editor without editing the save. Nothing is written to the save. Any other value is an error.
* Use **namespaced row names** (`<ModId>_<move>`, e.g. `MyMod_DoubleStrike`): numeric ids collide across mods (two mods that each add `258` overwrite each other).

### Values

Values use the game's own text format:

| Property kind | Write | Example |
|---|---|---|
| whole number (Int, Byte) | digits | `8`, `-5` |
| number (Float) | decimal | `12.5`, `0.25` |
| Bool | `true` / `false` (or `"True"` / `"False"`) | `false` |
| Enum | the enum's name | `"BackRight"` (valid names: F11 `rowprops` / `rowdump`, section 3) |
| Name | the name | `"FL_Strike"` |
| Text (display names) | `INVTEXT("...")` or plain text | `"INVTEXT(\"My Kick\")"` |
| Soft object (animations, icons) | the asset path | `"/Game/Animations/Attacks/MyMod/MyKick.MyKick"` |

Soft object paths are written exactly as `/Game/<folder>/<Asset>.<Asset>` (the pak must contain that asset). `None` clears one.

**Hard object references** (a property of kind `Object` / `Class`, for example `DT_LightSettings`' `SkySphereMaterial_...` = a material instance) are written the same way (`/Game/.../MI_X.MI_X`). Setting one makes the game load that asset, which must not happen while the table itself is loading. So the loader handles them like this:
* the object is already in memory: applied at once, with the other edits;
* the object is not loaded yet but its package is in a pak: the edit is **deferred** and applied at the start of the next game frame (normally the title screen, before any map uses the table), where the engine loads the asset normally. The log says `rows: <row>.<prop> deferred to the game thread` and later `rows: deferred object references applied on the game thread: N ok, M failed`;
* the package is in no pak (a typo, or an object a mod creates at runtime): skipped with a log line and an entry in `rows.txt`; it would hang the game.

A table that the game reads before its first frame sees the old value until then; a deferred edit that a later mod overrides with a direct value is dropped, so the later mod still wins.

The loader only checks that the object exists; whether the game copes with it is up to the data. For example, a sky material instance that the open world does not use may load and apply fine but crash the open world when its Blueprints read parameters the instance does not have. Use sky materials that the open world already uses, or edit their parameters instead.

**Names** (row, property, `copy_of`) may be up to 191 characters. Properties of Blueprint structs carry a GUID suffix (`HeightFogSunInscatteringColor_45_D32E825B4E726E2E58693DB7D327648E`, 65 characters): write the full name as `rowprops` prints it.

### Checking and failing

Each edit is checked before it reaches the game: a number must be a number, a Bool must be True/False, an enum must look like a name, and the game then rejects unknown enum names and unknown properties. A bad edit is **skipped on its own**, with the reason in `LanNative\rows.txt`; everything else still applies. A single bad field in a `set` (a property name that is too long or has a blank / quote / slash in it, or a value that is a list, an object or `null`) is skipped when the file is read: the log and `mods.txt` list it (`[WARN]`), the F2 Mods page shows a `WARNING:` line, and the rest of the file loads. A file with broken JSON or a structural mistake (no `table`, both `row` and `add`, a bad row name, ...) is ignored as a whole and the loader tells you the line number.

To check a whole `Mods` folder offline before starting the game, run `reccheck.exe` (section 13).

### When edits happen

The loader applies a table's edits right after the game loads it, for every mod in load order, so later mods win per field. If the table is ever loaded again, the edits are applied again.

---

## 6. `rows.json`: editing data assets

Some data is not a table but a single object with big nested properties, for example the guard animations (`GuardDB`). Edit these with `object` edits:

```json
{ "object": "/Game/DB/Guard/GuardDB.GuardDB", "ops": [
    { "append": "m_GuardAnimations.m_Array[0].m_Array[0].m_Array",
      "value": "AnimSequence'\"/Game/Animations/MyMod/MyGuard.MyGuard\"'" },
    { "insert": "m_GuardAnimations.m_Array[0].m_Array[0].m_Array[0]", "value": "AnimSequence'\"/Game/Animations/MyMod/MyOtherGuard.MyOtherGuard\"'" },
    { "set": "m_SomeNumber", "value": 3.5 }
] }
```

### The three operations

| Op | Meaning |
|---|---|
| `append` | add one element at the end of the list at the path |
| `insert` | add one element **before** position `[i]` (the path must end with `[i]`; `[count]` appends) |
| `set` | replace the value at the path |

`value` is the game's text for **one** value: for a list element an animation reference is `AnimSequence'"/Game/path/Name.Name"'`; a struct is `(member=value,member=value)`.

### Paths

A path walks the property's text. Property names and members are separated by `.`; `[i]` picks an element by position. The first segment is always a property of the object.

How the game writes things (you see this in an `objdump`):

* a struct: `(name=value,name=value)`
* a list: `(value,value,value)`
* a fixed-size array member: `name[0]=value,name[1]=value` (the `[0]` is part of the member's name)

So in `(m_Array[0]=(m_Array=(A,B,C)),m_Array[1]=(m_Array=(D)))`:

| Path | Selects |
|---|---|
| `m_Array[0]` | the first fixed array member |
| `m_Array[0].m_Array` | the list `(A,B,C)` |
| `m_Array[0].m_Array[1]` | the element `B` |
| `m_Array[1].m_Array` | the list `(D)` |

To add `E` to the first list: `{ "append": "...m_Array[0].m_Array", "value": "E" }`.

### Merging

Edits are small, so **two mods that append to the same list both end up in it**; nothing is replaced as a whole. Only a `set` of the same path by two mods is a conflict (the later wins, listed in `mods.txt`). Inserts shift later positions, so prefer `append` when order does not matter.

### Finding the path

1. `objdump /Game/DB/Guard/GuardDB.GuardDB` writes the object's properties as text.
2. Open `objdump_GuardDB.txt`, find the property and walk the nesting.
3. Or let a tool do it: with your mod's change in the game, dump the object once with the mod off (`vanilla`) and once on (`modded`), then
   `python tools\objdiff.py vanilla\objdump_X.txt modded\objdump_X.txt > ops.json` writes the exact `append`/`set` ops that turn one into the other.
4. To check ops offline, `ueapply.exe` applies them to a vanilla dump and writes the result (section 13).

### When edits happen

A data asset is edited when it finishes loading. If the loading happens on a background thread the edit is queued and applied at the start of the next frame, on the game thread (adding animation references loads those animations, which Unreal only allows on the game thread).

A Blueprint's default object (`/Game/Blueprints/BP_X.Default__BP_X_C`) can be dumped with `objdump`; applying ops to it is not supported.

---

## 7. Worked example: a mod with a pak and a `rows.json`

A full-size mod that adds a new style with its own moves looks like this:

```
MyStyleMod\
  MyStyleMod.pak     animations, icons, the style asset, blueprints (new files only)
  rows.json          its new attack rows + the entries for the animation databases it extends
  mod.json
```

Such a mod only *adds*: new files in its pak, new rows and database entries through `rows.json`, so it coexists with other mods that touch the same tables. A mod whose pak replaces the whole attacks table or a database cannot: only one copy of a file is read. Converting a replacing mod to an adding one keeps every value the same (check it with the dumps below).

How such a conversion is done (any mod that replaces tables or databases):

1. Start the game with the old mod **off**; dump what it changes with `rowfull`/`objdump` (these are the "vanilla" dumps).
2. Start it with the old mod **on**; run the same dumps ("modded").
3. `rowdump <table> <first> <last>` on the modded state gives the added rows as `rows.json`; `tools\objdiff.py vanilla modded` gives the database ops.
4. Rebuild the pak without the replaced files (they are in the pak as `X.uasset` + `X.uexp`).
5. Start with the new mod and dump again: the files must be identical.

---

## 8. Testing and debugging

Where to look (all in `Absolver\Binaries\Win64\LanNative\`):

| File | Contents |
|---|---|
| `lannative.log` | everything the loader did. Lines starting `mods:` are about mods and conflicts, `rows:` about edits |
| `mods.txt` | rewritten every start: which mods mounted, pak conflicts, split assets, row-edit conflicts, the mod fingerprint |
| `rows.txt` | per mod: edits applied and failed; the first failures with the reason |
| `launcher.log` | the launcher: start arguments and exit code |

Start the game once with `-NoMods` (Steam launch options) to run without any mods, for example if a mod stops the game from starting.

Common messages and what to do:

| Message | Meaning / fix |
|---|---|
| `rows.json IGNORED: expected ',' or '}' (line 7)` | JSON mistake near that line |
| `row 'X' does not exist` | wrong row name, or the row is added by a mod that is later in the load order |
| `no property 'X'` | misspelled property; see `rowprops` or the reference |
| `'banana' is not a number (Float property)` | wrong value type |
| `the game did not accept 'X' for this Enum property` | not a valid enum name; see the reference |
| `the row to copy ('X') does not exist` | `copy_of` names a row that is missing |
| `no member 'x' here` / `element [n] does not exist` | the object-edit path does not match the property's text; compare with an `objdump` |
| `SPLIT ASSET ...` | two paks each provide part of one asset (section 4) |
| the game closes after a few seconds | start with `-NoMods` to confirm it is a mod; then bisect: switch mods off one at a time in `loadorder.txt`. A crash report is in `%LOCALAPPDATA%\Absolver\Saved\Crashes` |

### 8b. The Mods page (F2) and hot reload

**F2** opens the F1 menu straight on the **Mods** page (F2 again or F1 closes it, Left goes back to the main menu). It shows, live:

The menus are drawn with Dear ImGui by default: a dark, translucent window with the same pages and key bindings (F1, F2, Up / Down, Enter, Left), and mouse clicks work too. While a menu is open it takes the mouse and the keyboard (F1 - F12 still reach the game, the gamepad is never taken). The table below lists the lines of the text form; the ImGui page shows the same data as sections (Mods table, Conflicts, Content registry, Plugins). Set `menu_style = text` in `LanNative\config.txt` for the plain text menu. When the overlay cannot start, the text menu is used and `LanNative\lannative.log` says why (`overlay: ...`).

| Line | Meaning |
|---|---|
| `Safe mode` | off, or ON with the reason (Shift held, `-SESafe`, `safe.txt`, crash-loop guard) |
| `Mods` / `Load order` | how many mods loaded, are switched off or were skipped, and the order they load in |
| `[on] Name vX: ...` | a loaded mod: pak count (mounted/found), row edits, content files, plugin (allowed or BLOCKED), `[cosmetic]` |
| `[off]` / `[SKIPPED] Name: reason` / `[empty]` | switched off in `loadorder.txt`; skipped (missing requirement, API too new, duplicate id ...); nothing to load |
| `Conflicts` | files shared by several mods (how many differ), row edit conflicts, data asset conflicts, split assets (likely crash) |
| `Content` | content registry: files, entries in use, invalid entries, and one line per `items` / `styles` / plugin-kind id that two mods define |
| `Plugin X` | running with no faults, running with faults so far (n of 3), or SWITCHED OFF after 3 faults |
| `Last reload` | result of the last hot reload |

The first lines (`>>`) are actions: Enter on them runs them. The first one is the hot reload; plugins add their own with `add_menu_item`.

**Hot reload** re-reads `Mods\*\content\**\*.json` and `rows.json` of the mods that loaded and applies what is new or changed without restarting the game. Two ways to trigger it: choose the first line of the Mods page ("Reload Mods\*\content and rows.json now"), or create the file `LanNative\reload.txt` (the loader reads and deletes it within a second; the result is in `LanNative\reload_result.txt` and on screen). Plugins can call `reload_content`. What it can and cannot do:

| What changed | Result |
|---|---|
| `rows.json`: a value of an existing or new edit | applied to the table if it is in memory (attack rows are read every time, so the effect is immediate) |
| `rows.json`: an edit was removed | stays in effect until a restart (the old value is not kept) |
| `rows.json`: data asset edits (`object` / `ops`) | not reloaded: they apply once when the asset loads |
| `rows.json`: a syntax error | the whole file set is rejected, the old edits stay, the error is shown |
| item: `name`, `description`, `icon`, `icon_file`, `props`, `price`, `gear_row.set`, `grant` | re-applied to the live object (a changed `icon_file` image becomes a new texture object) |
| item: `base`, `shop`, `shop_copy_of`, `gear_row.table` / `copy_of`, `moves` | refused and logged (`needs a restart`): the object, shop row or move binding exists already |
| a new item | created like at start-up (a `grant` is given a second later) |
| style: `name`, `icon` | written into the style DB, visible the next time the Meditation menu opens |
| style: `db`, `style_id`, `index`; a new style | `db` / `style_id` / `index` need a restart; a new style appears when the Meditation menu is built again |
| plugin content kinds | `apply` is called again for every changed or new entry (write it idempotent) |
| an entry deleted from disk | stays in the game until a restart; reported |
| a mod added after the game started | not noticed (restart); a mod that had no `content` folder when the game started has no content registry yet |

---

## 9. `mod.json` and the LAN check

```json
{
  "name": "FastJab",
  "version": "1.0",
  "author": "you",
  "description": "A faster Jab and a new move.",
  "affects_gameplay": true
}
```

The loader reads these keys (all optional; a mod without `mod.json` uses its folder name as id):

| Key | Meaning |
|---|---|
| `id` | stable name: letters, digits, `_`, `-`, `.`. Used by `requires` / `after`, in `/Game/Mods/<id>/...` object paths and in logs. Default: the folder name. Two mods with the same id: the later one is skipped |
| `version` | dotted number (`0.1.0`), compared by `requires.min` |
| `api` | the lowest mod API version the mod needs (currently 2); a loader with an older API skips the mod and says so |
| `requires` | `[ { "id": "Lib", "min": "1.0" } ]` (or just `"Lib"`): if that mod is missing, off, skipped or too old, this mod (pak, rows, content and plugin) is skipped and the reason is logged and shown on screen. Mods that require each other in a cycle are all skipped and listed as such. |
| `after` | `["Other"]`: load after Other if Other is present (no error if it is not) |
| `supersedes` | `["zzzOldMoves.pak"]`: pak names of OTHER mods that this mod replaces (it must ship its own pak); they are not mounted. Section 14 |
| `affects_gameplay` | set to `false` for a mod that only changes how things look (texture and skin packs) |
| `name`, `author`, `description` | for humans |

The load order is: `requires` and `after` first (a mod always loads after what it needs), then `loadorder.txt` (last line wins), which new folders join alphabetically at the end. It is printed once at boot in `lannative.log` (`mods: load order ...`); skipped mods appear as `mods: SKIPPED <mod>: <reason>`, in `LanNative\mods.txt` and as a red line on screen for 90 seconds. Plugins and content use the same order.

When you host or join a LAN game, both PCs compare their **gameplay mods** (every mod except those marked `affects_gameplay: false`): the same mods, in the same order, with the same file contents (a fingerprint over pak contents and `rows.json` contents). If they differ, both players see a red warning listing who has what. Fights between different versions of a move are not meaningful. The setting `modmismatch = warn | block | off` in `LanNative\config.txt` chooses between only warning (default), refusing the join, or no check.

A mod that adds items or changes moves must **not** be marked cosmetic.

---

## 10. Making your mod play well with others

* **Edit, don't replace.** `rows.json` edits merge; a pak that replaces a table or database does not.
* **Give new things your own names.** Name new rows with a prefix (`MyMod_Kick`) and put assets in your own folder (`/Game/Animations/MyMod/...`) so nobody else's files or rows collide with yours.
* **Append rather than insert** into shared lists, and `set` as few shared fields as you can: only a `set` of the same field is a conflict.
* **Don't ship half an asset.** If your pak has `X.uexp` it must also have the matching `X.uasset`/`X.umap` (split assets, section 4).
* **Order-dependent on purpose?** Put `"after": ["Other"]` or `requires` in `mod.json` (section 9), and say so in your README.
* Test next to a mod that changes the same table and read `mods.txt`.

---

## 10b. Content mods: items and fighting styles without code

Drop JSON files in `Mods\<mod>\content\<kind>\`. No `plugin.dll`, no `plugins.txt` entry: the loader's own content registry reads them (offline / private play, like every mod). Examples: `examples\ContentMods\ExampleMask` and `examples\ContentMods\WideWave`. A file holds one entry object, an array of entries, or `{ "items": [ ... ] }`; every entry needs an `id` (letters, digits, `_`).

```json
{ "items": [ { "id": "MyMask", "base": "/Game/DB/Equipment/Masks/SillyFace_Mask_DataAsset.SillyFace_Mask_DataAsset",
               "name": "My Mask", "grant": true, "equip": true } ] }
```
* **`items`**: `base` (stock asset to copy), `name`, `description`, `icon`, `props` (properties as engine text), `shop` / `shop_copy_of` / `price` (a shop row), `grant` (given to the loaded character as soon as its inventory is in, retried every 1.5 s from every world until it is; an item already owned is not added twice; a grant that is still waiting shows as a WARNING on the F2 Mods page), `equip` (section 14). The item exists from engine ready on, so the game keeps it in the save and loads it again next session. Remove the mod and the game boots normally: the item is hidden and its slot empty. New art (mesh, icon) needs a pak in the same mod and a soft path in `icon`.
  * `gear_row` (equipment and weapons): `{ "table": "WeaponDataTable", "copy_of": "w006", "set": { "m_eGearRarity": "Rare" } }` gives the item its own row `<ModId>_<ItemId>` in that `/Game/DB/Gear/` table (stats, rarity, weapon collision capsule); `name` / `description` are written into the row too. Without it the copy shares the source item's row.
  * `icon_file`: `"icon.png"` (a PNG / JPEG / BMP next to `mod.json`) becomes the item icon at runtime, no cooking. A cooked texture in your pak (`icon` = its path) works the same and is the better choice for a released mod.
  * Weapons in the shop: `"shop": "WeaponShop", "shop_copy_of": "NewRow", "price": 5000` (weapon rows are found and filled automatically).
  * `moves` (weapons): `["321", ...]` attack rows that are allowed in the weapon family's deck only while this item is equipped (the rows still need `m_bWith<Family>` True in data, e.g. via `rows.json`).
* **`styles`**: `db` (a FightingStyleDB asset, from your pak), `style_id` (new styles start at 6), `index` (button slot, 5 and up), optional `name` / `icon`. Adds a button to Meditation > Combat Style and lets it be unlocked and equipped. The style's moves and data come from your pak and `rows.json`.
* Every entry is checked; a bad entry is skipped and logged as `content: ... INVALID: <reason>`, unknown fields are logged and ignored, unknown kinds are skipped. If two mods define the same `id` of a kind, the one later in the load order wins and the log says `content: CONFLICT ...`.
* Plugins can add more kinds (`register_content_kind`, `docs/PLUGIN-API.md`).

---

## 10c. Powers without code (`content\powers`)

A new power = a clone of the stock power whose look is closest + data. `examples\ContentMods\WideWave\content\powers\widewave.json`:
```json
{ "powers": [ { "id": "WideWave", "base": "/Game/DB/Items/Shockwave.Shockwave", "name": "Wide Wave", "icon_file": "wave.png",
                "cost": 3, "cooldown": 4.0, "anims": { "all": "/Game/Animations/ItemUse/Exhaust.Exhaust" },
                "attack": { "copy_of": "10001", "set": { "m_fKnockBackDistance": 500 } }, "radius": 450, "grant": true, "equip_slot": 0 } ] }
```
It appears in the inventory under its own name and icon, is granted and put into power slot 0 (key 1 / D-pad left; `equip_slot` 1 = key 3), costs 3 shards, has a 4 s cooldown and hits in a 450 unit sphere (stock Shockwave: 280) with its own ItemAttack row; the stock Shockwave is untouched. Field list: `docs/PLUGIN-API.md` "powers entry".
* The effect actor, its visuals (particles, lights, timelines) and the kind of effect come from `base`; they cannot be changed without a cooked Blueprint. New projectiles, teleports or summons are not possible this way.
* A new animation needs the power animation's notifies (`ItemUseNotify` = when the effect, the cost and the cooldown happen; parry / absorb / avoid windows; early blend-out). A stock animation as stand-in works (WideWave uses Exhaust).
* Testing: keys 1 / 3 only switch decks in practice mode, so test in the open world, or call `power_use` from a plugin.

## 10d. NPCs without code (`content\npcs`)

A new NPC = a copy of a stock archetype (stats, deck, style, AI tuning) plus data. For example `MyNpc\content\npcs\brawler.json`:
```json
{ "id": "brawler", "base": "/Game/DB/AI/NPCs/Trickster/Trickster.Trickster", "health": 800, "style": 3,
  "deck": { "FrontLeft": ["078"], "FrontRight": ["138"], "BackRight": ["030"], "BackLeft": ["129"] },
  "ai": { "attack_cooldown": 1.5, "xp_bonus": 0 }, "aggro": true,
  "spawn": { "map": "MainMap_Main", "loc": [-33378, -7647, -3545], "yaw": 0, "near": 1500, "delay": 4 },
  "on_area": { "radius": 450, "do": ["message The Brawler is on you!"] },
  "on_health_below": { "fraction": 0.5, "do": ["message The Brawler catches his breath", "heal"] },
  "on_death": { "do": ["log the brawler is down"] } }
```
Drop the folder into `Mods\` (no `plugin.dll`, no pak). When the player walks to within 1500 units of the spot the Brawler appears there 4 s later (once per world), goes for the player, fights with its own four bare-hands rows only, shows a message at 450 units and heals itself once when it falls to half health. The Trickster archetype and its deck are untouched: the loader works on copies named `/Game/Mods/<mod>/<id>` and `<id>_Deck`.

* **What you can set**: `health`, `style` (0 Forsaken, 1 Windfall, 2 Lost Prospect, 3 Kahlt, 4 Stagger, 5 Faejin, 6+ a mod style), the bare-hands deck (`FrontLeft`, `FrontRight`, `BackRight`, `BackLeft`, `Alt`; the names are the asset's own fields and use the same left/right mirror as your save's decks; a key you leave out keeps the copied deck's rows; `copy_of` copies another `AttacksComboDB` instead), `ai` (`attack_cooldown`, `can_attack`, `can_defend`, `xp_bonus`), `props` (any archetype property as engine text), `faction` (0-5, AI default 2) and `aggro` (section 14).
* **A deck must fit the AI**: the AI plays *input actions* (Attack1, Attack2...) from start quadrants; keep one row per quadrant, and pick rows whose start stance matches the quadrant you put them in (the Brawler uses the Trickster's own start/end stances). A row missing for a quadrant makes it try an empty slot.
* **Where it can appear**: the spawn spot needs loaded ground. The open world (`MainMap_Main` and the sublevel around you) has navmesh; duel maps do not (except City_Cross and the 3v3 maps), so an NPC there stands and swings but does not chase. There is no ground trace: use a spot an NPC of the game stands on (the forest clearing in the example). A spot with no ground lets the NPC fall to the kill plane and die.
* **Reactions** (`on_health_below`, `on_death`, `on_area`; actions `log`, `message`, `heal`, `ai on|off`, `target player`, `faction <n>`, `remove`) run for every character that runs the archetype, however it was spawned, also by a plugin's `npc_spawn`.
* **From a plugin** `npc_spawn(self, "brawler", loc, &yaw, NULL)` spawns the entry on demand (a full archetype path works too), `npc_set_target` sends it after someone, `npc_remove` removes it, `on_area` watches a spot. Never `test_teleport` an AI NPC after it spawned: it stops moving for good; give `npc_spawn` the spot instead.
* **Not supported**: a weapon or gear for the NPC (`weapon`, `gear` are accepted and ignored) and a difficulty level. The NPC's display name is not changed (the `name` field is a label for the logs).
* NPCs give XP and loot like their base archetype: set `"ai": { "xp_bonus": 0 }` for test NPCs. Remove NPCs before saving in the open world.

## 10e. Map scripts without code (`content\maps`)

Lights that change colour, a gate that slides, a zone that opens it: one JSON file, no `plugin.dll`. `examples\ContentMods\MapScriptDemo\content\maps\demo.json` (placed relative to where the player stands in the open world):
```json
{ "id": "demo", "map": "MainMap_Main", "origin": "player", "delay": 2,
  "curves": { "rainbow": { "type": "color", "loop": 6, "keys": [[0,[1,0,0]], [2,[0,1,0]], [4,[0,0,1]], [6,[1,0,0]]] },
              "pulse":   { "type": "float", "loop": 2, "keys": [[0,0.3], [1,1], [2,0.3]] } },
  "lights": [ { "id": "rainbow", "pos": [350,0,180], "radius": 900, "intensity": 60000, "color_curve": "rainbow", "intensity_curve": "pulse" },
              { "id": "party", "pos": [350,-350,180], "color": [0,1,0.8], "on": "phase:party" } ],
  "meshes": [ { "id": "gate", "mesh": "/Engine/BasicShapes/Cube.Cube", "pos": [600,0,35], "scale": [0.3,3,2.5], "initial": "closed", "move_time": 0.8,
                "states": { "closed": { "offset": [0,0,0], "collision": true }, "open": { "offset": [0,0,320], "collision": false } } } ],
  "triggers": [ { "id": "zone", "box": { "center": [350,450,0], "half": [150,150,250] },
                  "on_enter": [ { "mesh": "gate", "state": "open" }, { "phase": "opened" }, { "message": "GATE OPEN", "seconds": 3 } ],
                  "on_exit":  [ { "mesh": "gate", "state": "closed" }, { "phase": "opened", "on": false } ] } ] }
```
* **Where it runs**: `map` is a case-insensitive part of the world path (`MainMap_Main`, `City_Coliseum`, `*` = any). `origin`: `"world"` (default; positions are map coordinates, `offset` is added to all of them) or `"player"` (positions are relative to the player when the scene is built: x forward, y right, z up, boxes turn with the player's yaw; good for demos and for maps whose coordinates you do not know). The scene is built `delay` seconds (0..60, default 2) after the player exists and is gone with the world.
* **curves**: `type` `color` (keys `[time, [r,g,b]]`, linear 0..1) or `float`; `loop` seconds (0 = no loop, the ends are held). `clock`: `"local"` (default) or `"server"` (game state time, same phase on every machine).
* **lights**: `type` `point` (default) or `spot` (`rot` [pitch, yaw, roll] describes the beam, `cone` [inner, outer] degrees), `pos`, `color` [r,g,b], `intensity` (0..2000000; in daylight 60000 is needed to be seen, indoors ~5000), `radius` (1..10000), `shadows` (default false), `color_curve`, `intensity_curve` (a float curve MULTIPLIES `intensity`), `phase_offset` (seconds), `on`: `always` (default), `never`, `phase:<name>`, `!phase:<name>`. Up to 64 lights per scene, 48 live in total (the strongest are kept).
* **meshes**: `mesh` any loaded or pak-packed StaticMesh (`/Engine/BasicShapes/Cube|Sphere|Cylinder|Cone|Plane` need no pak; the default is the cube, a 100 uu cube scaled by `scale`; alone it renders with the engine's grey checker, so give it a `material`), `pos`, `yaw`, `scale`, `states` (name -> `offset` from `pos` and `collision` on/off), `initial`, `move_time` seconds (smooth slide; collision is switched off at the start of an opening move and back on at the end of a closing one), `on` conditions like lights. Look: `material` (a stock material or instance) and `color` / `color_curve` / `brightness` / `param` (a dynamic material instance, e.g. the stock emissive `M_AltarBlocEmissive` with its `Color` parameter).
* **grids**: animated floor panels: `count` cells with a per-cell time offset (`checker`, `wave`, `rows`, `columns`, `ring`).
* **triggers**: `box` {`center`, `half`}, `who` `player` (only value), `once`, `on_enter` / `on_exit`: up to 4 actions each: `{"mesh": id, "state": name}`, `{"phase": name, "on": true|false}`, `{"light": id, "on": true|false}` (overrides the light's `on`), `{"message": text, "seconds": n}` (a banner, same as the modes API).
* **events**: `start`, `every n seconds`, `score >= n`, `match state`, each with `do` / `undo` and an optional `if` phase; the `cycle` action is a light-show sequencer.
* **Hot reload** (the F2 page): a changed entry replaces the running scene.
* **From a plugin**: `mapscript_phase(api, "party", 1)` and `mapscript_mesh(api, "gate", "open")` drive the same flags and states; the modes API (`on_round_end`, `match_info`) is the natural source for them in a game mode.
* **Not supported**: other characters as trigger sources (the box test is a per-tick check of the player), moving lights, particles, sounds.
* In a 1v1 arena use `map` of that arena and `origin: world`; lights need to be strong there too (a map's baked ambient washes weak lights out).

## 10f. Bots without code (`content\bots`, `content\ranks`)

A bot = an NPC that fights with a real player deck and a chosen AI profile. For example `MyBots\content\bots\duelist.json`:
```json
{ "id": "duelist", "profile": "vanilla", "ai_level": 60, "style": "Faejin", "health": 1000,
  "deck": { "weapon_slot": 0, "rows": { "FR": ["197", "169", "212"], "FL": ["210", "014"], "BL": ["191", "206"], "BR": ["198", "196"] },
            "alternates": { "FR": "170", "FL": "015", "BR": "117", "BL": "042" } },
  "aggro": true, "spawn": { "map": "MainMap_Main", "loc": [-33378, -7647, -3545], "near": 1500 } }
```
* `deck` uses the deck editor's row names FR / FL / BR / BL, inline as above or `{ "file": "decks/mydeck.json" }` next to `mod.json`. Bare hands only.
* `profile`: `vanilla` (typical stock NPC tuning) or `classic_hard` (the game's unused dev bot as it is); per-property overrides go in `props`. `ai_level` 0..100 = the AI's starting difficulty.
* Everything else (`spawn`, `aggro`, `on_death`, `on_area`, ...) works like `npcs` (section 10d). A plugin spawns a bot with `npc_spawn(self, "duelist", ...)`.
* `content\ranks\*.json` holds reward tables for plugins that pay a combat-trial reward (`trial_reward`, `docs/PLUGIN-API.md` "Bots and ranks").

## 11. Sharing your mod

A shareable mod is the mod folder in a zip:

```
FastJab.zip
  FastJab\
    rows.json
    mod.json
    README.txt
```

Checklist:

* `README.txt`: what it does, version, author, any load-order note, what it needs (for example "needs LuaHost").
* `mod.json` filled in (and `affects_gameplay: false` for look-only mods).
* **No game files.** Do not include extracted or re-saved vanilla assets you do not own. A `rows.json` contains only your changes; a pak should contain only your own new assets.
* No personal data in the files (user names, folder paths, IP addresses).
* Say it is for offline/LAN play.
* Test on a clean install with only your mod, then with another mod that touches the same table.

---

## 12. Limits (what this cannot do yet)

* Edits cover **DataTables** and **data assets** (objects with properties). They cannot change Blueprint graphs, C++ behaviour or anything stored in code. A mod that needs new game logic needs a plugin DLL (`docs/PLUGIN-API.md`) or a Lua script (`docs/sdk/LUA.md`).
* The attack table's row struct has no arrays or nested structs; other tables may. Struct values are written in the game's text form (`(X=1,Y=2)`); arrays inside rows are replaced as a whole (table rows have no per-element syntax; object edits do).
* Removing an element from a list is not supported (only `set`, `append`, `insert`).
* Data asset edits happen when the asset loads; an asset that never loads is never edited.
* Hard references added to a data asset load the referenced asset: the animation must exist in a mounted pak.
* Hard object references in **table rows** to assets that are not loaded yet are applied one frame after the table loads (section 5, "Hard object references"), not during the load.
* A table's edits are matched by the table's short object name (`attacks`); two different tables with the same short name would both match.
* Per object up to 24 different properties are edited per load, 64 different objects per game session, 256 mods.

---

## 13. Reference

* F11 `rowprops` / `rowdump` (section 3) - every property of a row, with its current value.
* Examples (`examples\`): `FastJab`, `HeavyJab` (table edits), `GuardTweak` (data asset edits: one `append`, one `insert`), `PlusCompat` (a rows.json mod that sits on top of a pak mod), `ContentMods\` (content kinds: `ExampleMask`, `WideWave`, `MapScriptDemo` and more; see `examples\ContentMods\README.md`). Plugin examples: `HelloPlugin`, `PluginTemplate` (`docs/PLUGIN-API.md`).
* Python tools (`tools\`): `objdiff.py` (object-edit ops from two `objdump` files), `pakls.py` (list a pak's files).
* Offline checkers in `tools\lannative\test\`, built by `build_tools.bat` (read-only, they never touch the game; they use the same code as the loader):
  * `reccheck.exe <Mods folder> [--dump] [--alone]` - validates every mod's `rows.json` and content in load order and reports errors and conflicts. Property names and value types are checked only in the game (`rows.txt`).
  * `modscan.exe <Mods folder>` - pak conflict report (shared files, split assets) in load order.
  * `ueapply.exe <vanilla objdump.txt> <rows.json> <object path> <out dir>` - applies a mod's object ops to a vanilla `objdump` and writes the result, to compare with a dump of the modded game.

---

## 14. Mod hygiene: pak shadowing, `supersedes`, one equip per slot, NPC aggro

**Pak read order (deterministic).** Mods mount in the resolved load order (`requires` / `after` first, then `loadorder.txt`; section 9). Each pak gets read order `10000 + 1000 * position + pak index` (paks of one mod sorted by name). For a file that exists in several paks the engine reads the pak with the HIGHEST read order, so a mod later in the load order wins, and inside one mod a later pak name wins. Put `"after": ["Other"]` in `mod.json` to make sure you load after `Other`.

**Shadowing warning.** When two mounted paks hold the same asset path with different content, the loader logs `mods: WARNING <winner pak> (read order N) shadows <loser pak> (M): k file(s) differ, e.g. <path> - only the first one is read`, writes it under "Pak shadowing" in `LanNative\mods.txt` and shows a `WARNING:` line right under "Load order" on the F2 Mods page. Files with identical content are harmless and only counted. Whole-table paks (attacks table, style tables) never merge: keep ONE per install.

**`supersedes`** (mod.json): `"supersedes": ["zzzOldMoves.pak"]` (pak file names, `.pak` optional, no folders). While the mod is loaded AND ships at least one `.pak`, a pak of that name in any OTHER mod is not mounted (`mods: WARNING ... is superseded by <mod> ... NOT mounted`, `[SUPERSEDED]` in mods.txt). If both mods list the same name, the later one in the load order keeps its pak. Declare it only if your pak really is a superset. Example: a weapon mod whose pak is a superset of a style mod's moves pak lists that pak in `supersedes`, so both mods can be installed together.

**Equip rule (`equip: true` in `items`).** The item is equipped only in the pass that grants it for the first time (an item already owned is left as the save has it), only if its slot (weapons: one slot; otherwise the folder of `base`, e.g. `Equipment/Masks`) was not already filled by another mod item in this session (first in load order wins), and for weapons only if the player does not already hold another weapon (hand-equipped or from the save). Every refusal is logged (`content: equip X skipped (slot S): ...`) and, for conflicts, shown as a `WARNING:` on the F2 page naming the items. Use `equip: true` for at most one item per slot.

**NPC aggro.** `"aggro": true` runs the engage sequence (`BPF_ForceEnemy(player)` plus the AI blackboard key `Enemy` set to the player) 1.5, 4, 8, 15 and 30 s after the spawn and again when the player walks into the NPC's own `on_area` radius; every call logs `npcs: <id> engage (...): ForceEnemy 1, blackboard Enemy 1, distance D uu, focus ..., alerted ...`. `npc_set_target` stays for manual use.

## 15. Bot trials without code (`content\bot_trials`)

A combat trial against your bots, offline and solo: the loader takes you to a duel map, spawns the bot(s), counts the rounds, shows the score and pays the trial reward. For example `MyTrial\content\bot_trials\coliseum_duelist.json`:
```json
{ "id": "coliseum_duelist", "name": "Duelist in the Coliseum", "map": "/Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum",
  "bot": "duelist", "rounds_to_win": 3, "ai_level": 60, "rewards": "bot_trials", "menu": true }
```
* Start it from the F2 Mods page ("Content: Bot trial: Duelist in the Coliseum") or from a plugin with `bot_trial_start(self, "coliseum_duelist")`.
* `bot` is a `bots` entry (section 10f); `"bots": ["a", "b", "c"]` makes a gauntlet: beat each one in turn; `"ai_level_step": 10` makes each next bot 10 levels harder.
* Put `"ai_level_fixed": true` in the bot entry so the AI keeps its level for the whole fight (otherwise the stock AI gets easier every time it or you succeed and soon drops close to 0).
* `rewards` names a `ranks` table (a `bot_trials` table with the stock 1v1 numbers: 500 XP for finishing, +4000 for a win, +1500 for 2+ rounds won, 1 game-mode point each). The reward is paid after you are back in the open world.
* Your points fill the stock score diamonds and count as real scored points for map scripts (a scene's `score` event fires on them).
* Duel maps have no navigation mesh, so a bot often stays where it spawned; the loader then walks it towards you (`approach_assist`, default `auto`), it fights on its own once it is close. It does not circle you the way it does in the open world.
* A trial mod needs the mod that defines its bot (`requires` in `mod.json`). Reference: `docs/PLUGIN-API.md` "Bot trials" (since API 2.16).

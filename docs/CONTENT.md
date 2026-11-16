# Data reference: rows.json and content kinds

## rows.json
```json
{ "format": 1, "edits": [
  { "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 8 } },
  { "table": "attacks", "add": "MyMod_Kick", "copy_of": "004", "learn": "always", "set": { "m_Name": "MyMod_Kick" } },
  { "object": "/Game/DB/Guard/GuardDB.GuardDB", "ops": [ { "append": "m_GuardAnimations.m_Array[0].m_Array[0].m_Array", "value": "..." } ] }
] }
```
| Edit | Meaning |
|---|---|
| `table` + `row` + `set` | change properties of a row (`table` = the table's short name) |
| `table` + `add` + `copy_of` + `set` | new row as a copy of `copy_of`; an existing row of that name is kept and `set` applied. Use names with your mod id as prefix |
| `"learn": "always"` (attack rows) | the move counts as learned for every character; nothing is written to the save |
| `object` + `ops` | data assets: `append` (add at the end of the list at the path), `insert` (before `[i]`), `set` (replace the value at the path). Paths walk the property's text: `name.member[i].member` |

* Values use the engine's text format: numbers, `true` / `false`, enum names, `INVTEXT("...")`, asset paths `/Game/Path/Asset.Asset`, structs `(X=1,Y=2)`.
* Each edit is checked; a bad edit is skipped alone and logged in `LanNative\rows.txt`.
* Several mods may edit the same table: later mods win per field; two mods appending to one list both land.
* A hard object reference to an asset that is loaded applies at once, one in a mounted pak one frame later, one in no pak is skipped.
* Limits: arrays inside table rows are replaced whole; no removing of list elements; an asset that never loads is never edited.
* `"format"`, `//` comments and trailing commas are accepted; a top-level array of edits works too.

## Content kinds
Files in `Mods\<mod>\content\<kind>\*.json` hold one entry, an array, or `{ "<kind>": [ ... ] }`. Every entry needs `id` (letters, digits,
`_`, max 63). Bad entries are skipped and logged (`content: ... INVALID: <reason>`); the same kind and id in two mods: the later mod wins.
Objects are created under `/Game/Mods/<mod id>/<id>`. Plugins add kinds with `register_content_kind`.

| Kind | Creates | Main fields |
|---|---|---|
| `items` | a copy of a stock item (gear, weapon, mask) | `base` (stock asset path), `name`, `description`, `icon` / `icon_file` (PNG / JPEG / BMP next to `mod.json`), `props` (properties as engine text), `shop` + `shop_copy_of` + `price`, `gear_row` `{table, copy_of, set}`, `moves` (weapons), `weapon` (a `weapons` kind), `grant`, `equip` |
| `weapons` | a weapon kind shared by items | moves, damage / grip values, `deck`, `anim_set`, `anim_style`, `deflect_style`, `moves_exclusive` |
| `styles` | a button in Meditation > Combat Style | `db` (a FightingStyleDB asset from your pak), `style_id` (6 and up), `index` (button slot, 5 and up), `name`, `icon`, `gleam_level` (unlock level) |
| `anim_sets` | an animation set at its own index of the style-indexed animation tables | `index` (`auto` or a number), `fallback_style`, `slots` (`table`, `arr`, `at`, `anim`) |
| `powers` | a copy of a stock power | `base`, `name`, `icon_file`, `cost` (shards), `cooldown` (s), `attack` `{copy_of, set}` (own ItemAttack row), `radius`, `anims`, `grant`, `equip_slot` (0 / 1) |
| `npcs` | an NPC archetype + deck copy | `base`, `health`, `style`, `deck` (`FrontLeft` `FrontRight` `BackRight` `BackLeft` `Alt`), `ai` (`attack_cooldown`, `can_attack`, `can_defend`, `xp_bonus`), `props`, `faction`, `aggro`, `spawn` `{map, loc, yaw, near, delay}`, reactions `on_health_below` / `on_death` / `on_area` with actions `log message heal ai target faction remove` |
| `bots` | an `npcs` entry with a player deck and an AI profile | `template`, `deck` (rows FR / FL / BR / BL, `alternates`, or `{file}`), `deck_sword`, `profile` (`vanilla` / `classic_hard`), `ai_level` (0-100), `ai_level_fixed`, `resilience`, `style`, `stats`, `outfit`, `intro`, plus these `npcs` fields: `health`, `aggro`, `faction`, `spawn`, `props` and the `on_*` reactions |
| `ranks` | a reward table for `trial_reward` | `rewards` (`finish`, `victory`, `performance`, `mentoring`, `xp_scale`), `gm_points` |
| `bot_trials` | a duel against one bot or a gauntlet | `map` (a 1v1 map), `bot` / `bots`, `rounds_to_win`, `ai_level`, `ai_level_step`, `rewards`, `menu`, `player_like` |
| `maps` | a scene in a map: lights, moving meshes, grids, triggers, events | `map`, `origin`, `delay`, `curves`, `lights`, `meshes` (with `states`), `grids`, `triggers`, `events` |
| `clones` | a second local player driven by a bot brain | `name`, `brain`, `profile` (a file with the character data), `style`, `menu` |
| `poise` | a poise gauge for an archetype | `archetype`, `poise`, `heavy_power` |

Content and `rows.json` reload from the first line of the Mods page (F2); `base` and `copy_of` fields need a restart.

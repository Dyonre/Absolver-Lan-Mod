# Lua scripting (LuaHost)

Write gameplay logic in Lua without writing C. The script itself needs no compiler, but the LuaHost plugin that runs it is **not** in the release zip: you build it once (MSVC Build Tools 2022, `tools\luahost\build.bat`) or use a build you trust. If you are new, start with `rows.json` or `content` JSON instead. Lua 5.4.9 (MIT, `third_party/lua`), hosted by the plugin `tools/luahost` (build: `tools\luahost\build.bat` -> `out\plugin.dll`).
Offline / private play, `-NoEAC` only, like the whole extender.

## Turn it on
1. Folder `Mods\LuaHost\` with `plugin.dll`, `plugin.cfg`, `mod.json` (from `tools\luahost`).
2. Add the line `LuaHost` to `Mods\plugins.txt` (plugins are native code: only listed ones load). Without it no script runs.
3. Your mod: `Mods\<YourMod>\scripts\*.lua` (+ `mod.json` as usual). Scripts of every ENABLED mod run in the loader's load order, files by name.

`Mods\LuaHost\plugin.cfg`:

| key | default | meaning |
|---|---|---|
| `budget` | 2000000 | Lua instructions one handler / timer / REPL line may run (a script's top level gets 5x) |
| `memory_mb` | 64 | memory cap of all scripts together |
| `reload` | on | poll script files every second; a change reloads every script |
| `repl` | on | run Lua written into `repl_in.txt` |

## A whole mod
```lua
-- Mods/BigJab/scripts/main.lua : the player's Jab (row 004) does 3x damage
abs.on("hit_pre", function(h)
  if h.attack == "004" and h.attacker == abs.player() then h.damage_scale = h.damage_scale * 3 end
end)
abs.on("hit", function(h) abs.log("hit", h.attack, h.outcome, h.damage) end)
```
Example in the repo: `examples/ContentMods/LuaDemo`.

## Rules of the sandbox
* Available: base Lua (no `load`, `loadfile`, `dofile`), `string` (no `string.dump`), `table`, `math`, `utf8`, `coroutine`. Not available: `io`, `os`, `require`/`package`, `debug`. Only text chunks load.
* Each mod has its own globals (your `x = 1` is not another mod's `x`); the libraries and `abs` are shared, do not modify them.
* Budget: every handler / timer / REPL line may execute up to `budget` instructions (plugin.cfg, default 2,000,000; a script's top level 5x). Over it the call stops with "instruction budget exceeded"; your own `pcall` cannot keep it running.
* Memory: all scripts together `memory_mb` (default 64). Over it the allocation fails inside the script ("not enough memory").
* A handler that errors 3 times is removed (logged). Errors are logged with mod, event and a traceback; the game keeps running.
* Characters and objects are opaque handles (light userdata). Use them inside the event you got them in; do not keep them across worlds.

## Events: `abs.on(name, function(...) end)`
| name | arguments |
|---|---|
| `ready` | after all scripts loaded (also after every reload) |
| `character` | the local player's character appeared (its save section is readable); again after a reload |
| `tick` | `dt` seconds, every frame (keep it tiny) |
| `world` | map path of a new world |
| `hit_pre` | table `{victim, attacker, attack, attack_id, perfect_link, damage_scale, stun_scale, knockback_scale, special_ratio_scale, special_ratio}`; change the scales (0..100; `special_ratio` 0..1 or -1) |
| `hit` | table `{victim, attacker, attack, attack_id, damage, outcome, perfect_link}`; outcome `hit guarded guard_broken absorbed parried dodged no_effect` |
| `attack` | `character, attack_id, attack` (once per real attack) |
| `death` | `victim, killer, attack_id, attack` |
| `defense` | table `{type, defender, attacker, attack, frame_in_attack, startup}`; type `guard guard_break absorb parry deflect dodge avoid` |
| `weapon` | `character, old_item, new_item` (local player) |
| `style` | `character, old_style, new_style` (local player) |
| `menu` | `menu_class, state` |
| any other name | your own: `abs.emit(name, ...)` calls every script's handlers (an event bus between mods) |

## Functions
| call | what |
|---|---|
| `abs.log(...)` / `print(...)` | a line in `LanNative\lannative.log`, tagged `lua <mod>:` |
| `abs.timer(seconds, repeat, fn)` -> id, `abs.cancel(id)` | game-thread timer (>= 0.05 s) |
| `abs.player()` | the local player's character or nil |
| `abs.char_info(c)` | `{name, health, max_health, guard, stance, style, is_player, is_dead}` (nil = player) |
| `abs.find(path)` | a loaded object (`/Game/DB/...`) or nil |
| `abs.get(obj, prop)` / `abs.set(obj, prop, text)` | property as engine text |
| `abs.row_set(table_path, row, prop, text)` | one DataTable field (validated like rows.json) |
| `abs.row_float(table_path, row, byte_offset)` | read a float field of a row |
| `abs.damage_mod(c, key, value)` | per-character multiplier (`damage_taken`, `damage_dealt`, `stun_taken`, ...) |
| `abs.console(cmd)`, `abs.message(text, s)`, `abs.banner(text, s)` | console command, toast, HUD banner |
| `abs.kv_get(key)` / `abs.kv_set(key, value)` | small persistent store per mod (`LanNative\data\LuaHost.txt`, keys prefixed with your mod) |
| `abs.bind(action, key, fn[, label])` | a key for your action (section Keys) |
| `abs.save_section(version[, migrate])`, `abs.save_commit()` | per-character data (section Save data) |
| `abs.clock()`, `abs.lua_version`, `abs.generation`, `abs.mod` | seconds since boot, "Lua 5.4.9", reload counter, your mod id |

Raw reflection is limited to `abs.get` / `abs.set` / `abs.find`: calling UFunctions by name and typed property access are not available from Lua (use a C plugin for those).

## Keys (keybind registry)
`local key = abs.bind("toggle", "K", function() ... end, "Toggle my thing")` registers the action `<YourMod>.toggle` on K and returns the key it really got.
* Players move keys in `Mods\LuaHost\keybinds.txt`: `LuaDemo.toggle = J` or `= off` (read at load and whenever the file changes).
* `Mods\LuaHost\keybinds_report.txt` lists every binding and every conflict (two actions on one key, or an action on a loader key F1-F11); conflicts are also logged. Both actions still fire on a shared key.
* Keys: F1..F24, A..Z, 0..9, NUMPAD0..NUMPAD9, 0x01..0xFE. Only while the game window has the focus.
* Limit: the loader has 32 key slots for all plugins together; each distinct key LuaHost uses takes one.

## Save data per character (save sections)
```lua
abs.on("character", function(c)                         -- the local player's character is there (also after a reload)
  local s = abs.save_section(2, function(old_version, data)   -- migrate older data once
    if old_version == 1 then data.count = data.visits; data.visits = nil end
    return data
  end)
  s.count = (s.count or 0) + 1
  abs.save_commit()                                     -- also done automatically when the game saves and before a reload
end)
```
* Stored in the character's sidecar (`LanNative\state\<GUID>.txt`), never in the game's `.sav`: the save loads unchanged without mods, and removing a mod leaves only an unused line.
* Values: tables of strings, numbers, booleans (nested up to 16, no functions, no self references), at most 60,000 bytes per mod.
* Data written by a NEWER version of your script is kept unchanged and read-only (`save_commit` returns false).

## Hot reload, REPL, menu
* Save a script: within a second every script is reloaded (the whole Lua state restarts: timers and handlers of the old state are gone, `ready` fires again). `reload = off` in plugin.cfg disables it.
* REPL: write Lua into `Mods\LuaHost\repl_in.txt` (an expression or statements); it runs within a second, the result is logged (`luahost: repl>`) and appended to `repl_out.txt`. `repl = off` in plugin.cfg disables it.
* F1 > Mods page: `LuaHost: reload scripts`, `LuaHost: status`.

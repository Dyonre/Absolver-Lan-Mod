# Settings

`<game>\Absolver\Binaries\Win64\LanNative\config.txt`, one `key = value` per line, a line starting with `#` is a comment. Read at start; a missing
line = the default. Without a `config.txt` the loader reads the older one-line files instead (`LanNative\parryfix.txt`, `pvpfix.txt`, ...);
once `config.txt` exists those files are ignored.

## LAN (details: `docs/HOSTING.md`)
| Key | Default | Meaning |
|---|---|---|
| `lan` | `off` | `on`: LAN networking hooks (joiner fixes, host tracking, cleanup of a departed player) |
| `hostip` | `auto` | address a host binds (`auto` = every adapter); repeatable, the first is the default, F1 cycles them |
| `allow` | none | `a.b.c.d[/bits]`, repeatable (max 64; `#` starts a comment): only these addresses may join and see the game in the LAN list. An invalid line refuses every join until it is fixed |
| `lanfinder` | `on` | `off`: no LAN list (this PC neither answers searches while hosting nor searches at an altar; UDP 7778) |
| `maxplayers` | `6` | players in the host's world, the host included: `2`-`16` or `off` |
| `moddownload` | `prompt` | `prompt`: a host offers its shareable pak mods and a joiner is asked before anything is downloaded; `off`: neither |
| `moddownload_port` | `7779` | TCP port of the host mod download |
| `modmismatch` | `warn` | `warn` / `block` / `off`: what happens when host and joiner have different gameplay mods |
| `autoleave` | `on` | in a duel map, return to your own world when the other player's connection is gone |
| `pvpfix` | `on` | `on`: LAN duels (send-back guard + duel flow); `guard`: guard only; `off` |
| `perteam` | `1` | players per team in 3v3 maps: `1`, `2`, `3` / `off` (the game's own 6) |
| `soloduel` | `off` | `on`: a hosted 1v1 / 3v3 starts with only the local player |
| `npc` | `on` | F1 > Spawn NPC (host only) |
| `npcrepl` | `off` | also replicate the NPC spawner actor to joiners |
| `matchmaking` | `off` | a private address or `auto`: the game's two backend service names resolve to that LAN address (for a LAN backend program); private / loopback addresses only |
| `servermode` | `off` | `on` (or `-LNServer`): unattended host that loads `servermode_save`, travels to `servermode_map` after `servermode_delay` seconds (3-300, default 12) and hosts |

## Mods and patches
| Key | Default | Meaning |
|---|---|---|
| `content_paks` | `stock` | `stock`: in Modded starts the loader mounts only the game's own paks from `Content\Paks` (`pakchunk0`-`2`) and the `Mods` folder; every other pak in `Content\Paks` (and its subfolders) is skipped (counted on the Mods page, each one listed in `LanNative\mods.txt` and the log). `all`: those paks load as well. Vanilla starts always load them |
| `parryfix` | `on` | Forsaken parry fix (one byte patched in memory at start) |
| `records` | `off` | `on`: named constants (`flats`) and templates in `rows.json` |
| `patches` | `off` | `on`: apply `LanNative\patches.txt`, one row per patch: `name | RVA or Symbol+offset | expected bytes | new bytes | on/off`; a row is refused when the bytes in memory differ. Memory only, the exe file is never changed |

## Menus, logs, performance
| Key | Default | Meaning |
|---|---|---|
| `menu` | `on` | the F1 / F2 menus |
| `menu_style` | `imgui` | `imgui` / `text` |
| `pad_menu` | `on` | gamepad in the F1 / F2 menus: Back + Start opens F1, d-pad / A / B navigate; `off` |
| `keep_keys` | `F11` | keys the game never gets in a Modded start (comma-separated key names, or `none`); F keys the loader or a mod uses are kept from the game as well. Works through the ImGui overlay (`menu_style = imgui`, the default) |
| `telemetry` | `basic` | `off` / `basic` / `verbose` [seconds]: CSV snapshots in `LanNative\` |
| `perf_stats` | `on` | frame-time recorder, 30 s windows written to the log |
| `perf_uncap` | `off` | `on`: send `r.VSync 0` and `t.MaxFPS 0` once a player exists (measurements; the game's settings file is unchanged) |

Keys of individual features (`wp_*`, `anim_sets*`, `bot_ai_*`, `clone_spawn`, `deck_editor_kind_tab`, `input_rec_dir`,
`patches_selftest`) default to the shipped behaviour; most features log their value at start.

## Command line
| Switch | Meaning |
|---|---|
| `-NoEAC` | required: without it the loader does nothing. The launcher installed in place of `Absolver.exe` adds it in Modded starts |
| `-Modded` / `-Vanilla` | launcher only (Steam launch options): start that way without the Modded / Vanilla window. Vanilla renames `version.dll` to `version.dll.off` and runs the original `Absolver.exe.eac` with EasyAntiCheat; the next Modded start renames it back. The remembered choice is `LanNative\launch.txt` (`modded` / `vanilla`); Shift held at start shows the window again (release it once the window is up: Shift still held when the game starts also selects safe mode) |
| `-NoMods` | start without mods |
| `-SESafe` | safe mode: no mods, no plugins, no extender hooks (also: Shift held at start, or a file `LanNative\safe.txt`) |
| `-LNServer`, `-LNSave=<character>` | server mode |
| `-InstanceID<n>` | a second game instance on the same PC gets its own player id (no `=`) |

## Keys
| Key | Action |
|---|---|
| F1 | loader menu: host, join, LAN games, kick, maps (categories; type to search), NPCs, options |
| F2 | Mods page: load order, switch mods on / off, profiles, settings, conflicts, plugin faults, reload, skipped `Content\Paks` paks, host mod download, Net Inspector, key binding, log window (`docs/MODS.md`) |
| Back + Start (gamepad) | opens the F1 menu (`pad_menu`) |
| F9 / F10 / F11 | developer keys, only while `LanNative\devkeys.txt` exists: free camera, teleport, developer commands (`docs/ADVANCED.md`) |

A key that two owners use (the loader, a plugin's `on_key` or `declare_key`, the game, Steam) is a clash: it is logged once and listed on the
Mods page. While the F1 / F2 menu or a plugin window has the keyboard, other plugins' `on_key` callbacks get only F keys.

## Per-mod settings
A mod's `plugin.cfg` (`key = value`), read by its plugin with `config_get`. A `settings` list in `mod.json` shows it as a form on the Mods page.

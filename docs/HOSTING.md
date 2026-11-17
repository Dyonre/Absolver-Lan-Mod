# Hosting and joining Absolver games (LAN / VPN)

Offline / private LAN play only (`-NoEAC`, like every part of the loader). Players on different networks need a shared LAN or VPN (for example Radmin VPN, Hamachi or ZeroTier). Settings go in `Absolver\Binaries\Win64\LanNative\config.txt`, one `key = value` per line (the installer does not create it: make it with any text editor when you need a setting).

**There is no join key, joiner limit or lock-out (since 5.2.0).** Anybody who can reach your PC on the network can find and join your game. On a shared or public VPN network (a public Radmin or Hamachi room) set `allow` lines (section 2) so that only your friends' addresses can join.

## 1. Host, find, join

**Host:** F1 menu > `Host`. The main world opens as a listen server (UDP port 7777) and the screen says that LAN players can find it at an altar.

**Join from an altar (no address to type):** walk to any altar and open it. While the altar menu is open (and you are not joined to a host) the loader searches the network; a host that answers shows up as `LAN game found: <character> on <map> - press F1 to join`. Press **F1**: the menu opens on the **LAN games** list (`<character> - <map> (co-op / duel, N players) - <address> - mods match / MODS DIFFER`). Select a game and press **Enter** to join. The same list is on the F1 menu as `LAN games...` anywhere in the world.

**Join by address:** F1 > `Join address (Enter to type it)` writes the host's address (`192.168.1.20` or `26.1.2.3:7777`) to `LanNative\ip.txt`, then F1 > `Join`. Use this when the list stays empty (section 3).

| F1 item | What it does |
|---|---|
| `Host` | opens `/Game/Maps/MainMap/MainMap_Main?listen` as a listen server |
| `Host address` | which network address a host binds: `auto` (every adapter, default) or one of the `hostip = <address>` lines in `config.txt` (repeatable; first line = default). With a fixed address, joiners must use exactly that address (the LAN list does this for them) |
| `LAN games...` | hosts found on the LAN / VPN; Enter joins, Enter on the "Searching..." line searches again |
| `Join address (Enter to type it)` | type or paste the host's address; it goes to `ip.txt` line 1 |
| `Join` | connects to the address in `ip.txt` |
| `Bring joiners to me` | moves the joined players to the host |
| `Kick player...` | host only: the connected players (name and address); Enter arms a player, Enter again within 5 seconds removes them |

Joining from the LAN list writes `ip.txt` too (line 1 = the host's address; line 2 = `none` for a host on a duel map, so the join uses a bare URL), so `Join` reconnects to the same host later. While connected as a joiner, `Host`, `Join` and map travel are ignored once (logged); select the item again within 5 seconds to leave and do it anyway. `ip.txt` line 2 may also hold extra URL options for the join. The developer command `url` (`docs/ADVANCED.md`) shows the join URL without joining.

Every host travel the loader makes (F1 > `Host`, the map list, server mode) adds `?bIsLanMatch` to the URL, so a host whose Steam is online still listens on the network addresses (without it, joins by address time out).

Both sides compare their gameplay mods when a joiner arrives (`modmismatch = warn | block | off`, `docs/MODS.md`); the LAN list already shows whether they match. A joiner can get the host's shareable pak mods from the host (section 4).

**Kick a player (host):** F1 > `Kick player...`, select the player, press Enter twice. The game's own kick runs: the player's character and connection are removed, their game goes back to the title screen and shows `KICKED: The host removed you from the session.` (the loader's notice; a joiner with `modmismatch = off` does not show it). A kicked player can join again; to keep someone out, use `allow` lines.

**Player limit:** `maxplayers = 6` (default; `2`-`16`, or `off`) counts the players in the host's world, the host included. A joiner past the limit is refused with "the host's world is full".

## 2. Who may join

| Setting (`LanNative\config.txt`) | Default | Meaning |
|---|---|---|
| `allow = a.b.c.d[/bits]` (repeatable, up to 64; `#` starts a comment) | none = anybody who can reach your PC | Only these addresses may join **and** see your game in the LAN list, e.g. `allow = 26.0.0.0/8` for a Radmin network, or one line per friend's address |
| `lanfinder = on / off` | `on` | `off`: this PC neither answers LAN searches while hosting nor searches at an altar (joining by address still works) |
| `maxplayers = N / off` | `6` | players in the host's world, the host included (`2`-`16`) |

**An `allow` line that is not valid closes the host:** a bad address, `/bits` outside 0-32, anything after the address that is not a `#`
comment, an empty `allow =` line or more than 64 lines refuses every join and hides the game from every LAN list until the line is fixed
and the game restarted. The log says `join check: FAIL CLOSED - allow line N ...`, the host's screen shows `ALLOW LIST ERROR: line N ...`
at start, when hosting and at each refused join, and the joiner is told that the host's allow list has an error.

The settings `joinkey`, `joinprotect` and `maxpeers` and the file `joinkey.txt` of earlier versions are no longer used: the log says so once at start, and the lines can be deleted. Peer addresses are masked in the log (`26.12.0.x`). With `allow` lines set, a login whose check faults is refused.

## 3. How the LAN list works, and when it stays empty

* A player at an altar (or on the `LAN games` page) sends a search every 2 seconds to UDP **7778** on every network adapter's broadcast address (the real LAN and each VPN adapter), to `255.255.255.255`, and directly to the address in `ip.txt`. Nothing is sent while nobody looks.
* A hosting PC answers each search with one short reply: character name, map, player count, loader version, the host's bound address (when `hostip` is set) and the gameplay-mod fingerprint and names. A host that stops answering drops off the list after 7 seconds.
* A search is padded to 512 bytes and a reply is always smaller, and a host sends at most 20 replies per second, so the finder cannot be used to flood a third party. A host with `allow` lines answers only the addresses they allow.

**The list stays empty when:**

| Cause | Fix |
|---|---|
| Windows Firewall blocks UDP 7778 on the host | Allow `Absolver-Win64-Shipping.exe` (or UDP 7777-7778) on **private** networks |
| The VPN does not pass broadcasts (Tailscale and other point-to-point VPNs; some Hamachi network types) | Put the host's VPN address in `ip.txt` (F1 > `Join address`): the finder also asks that address directly, so it shows in the list; or use `Join` |
| The host set `allow` lines that do not include you, or `lanfinder = off` | Ask the host |
| The host is not hosting yet | The host presses F1 > `Host` first |

## 4. Mods from the host

A host offers the `.pak` files of its loaded pak-only gameplay mods whose `mod.json` says `"shareable": true` (`docs/MODS.md`); `rows.json`, `content\`, plugins and Lua are never offered, and other gameplay mods are only named. When a joiner's gameplay mods differ from the host's, the joiner sees the offer (a message and rows on the F2 Mods page). Nothing is downloaded before the player clicks Install. Each file is checked (size, SHA-256, that it is a game pak, which file types it holds, and that no packed path leaves the game folders: no `..`, nothing mounted above the game folders, no path over 511 characters) and stored in `Mods\_FromHost\<host>\<mod>\`; those mods load at the next start, after the player's own mods. A pak that fails one of the pak checks refuses the whole download: the files already written are deleted and nothing is activated. Then restart and join again.

The download uses its own TCP connection to the host (port `moddownload_port`, default 7779). A host answers only players that joined its game, and its `allow` lines apply. `moddownload = off` turns it off on that PC (it neither offers nor asks).

## Playing over the internet: VPN safety and responsibility

The official online mode is not supported and almost certainly will not work with this loader, which needs EasyAntiCheat off. Never use mods in the official online mode. To play with friends who are not on your network, use a private LAN connection or a VPN / virtual LAN that makes everyone look like they are on one network.

You are responsible for how you do this. Some basics:

* **Only play with people you trust.** A host accepts connections from every address that can reach it, and everyone on the same virtual network sees your game in their LAN list. Use a private VPN network (with a password) rather than a public one; on a shared network add `allow = <range>` lines.
* **Never port-forward the game (7777), the finder (7778) or the mod download (7779) to the open internet** and never bind the host to a public address.
* **A VPN provider sees your traffic metadata and your friends' addresses.** Read the terms and privacy policy of whatever VPN or virtual-LAN software you choose, and only install software you trust.
* **Everyone should run the same gameplay mods.** The LAN list and the join check warn when they differ.
* **A plugin from a friend is a program.** Do not enable a `plugin.dll` you have not checked, even if it comes from someone you play with.
* **No promises.** LAN and VPN play are best effort. This is a hobby project and I take no responsibility for what happens.

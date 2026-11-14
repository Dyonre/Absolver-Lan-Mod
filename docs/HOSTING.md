# Hosting Absolver games (LAN / VPN) and join protection

Offline / private LAN play only (`-NoEAC`, like every part of the loader). Players on different networks need a shared LAN or VPN (for example Radmin VPN). Settings go in `Absolver\Binaries\Win64\LanNative\config.txt`, one `key = value` per line (the installer does not create it: make it with any text editor when you need a setting).

## 1. Host and join

Everything is on the **F1** menu:

| Item | What it does |
|---|---|
| `Host` | opens the main world (`/Game/Maps/MainMap/MainMap_Main?listen`) as a listen server (UDP port 7777); the join key is shown on screen |
| `Host address` | which network address a host binds: `auto` (every adapter, default) or one of the `hostip = <address>` lines in `config.txt` (repeatable; first line = default). With a fixed address, joiners must use exactly that address |
| `Join address (Enter to type it)` | type the host's address (`192.168.1.20` or `26.1.2.3:7777`); it is written to `LanNative\ip.txt` (line 1) |
| `Join` | connects to the address in `ip.txt` |
| `Bring joiners to me` | moves the joined players to the host |

While connected as a joiner, `Host`, `Join` and map travel are ignored once (logged); select the item again within 5 seconds to leave and do it anyway. `ip.txt` line 2 may hold extra URL options for the join, or `none` for a bare URL. F11 `url` (MODDING-GUIDE section 3) shows the join URL without joining.

Both sides compare their gameplay mods when a joiner arrives (`modmismatch = warn | block | off`, MODDING-GUIDE section 9).

## 2. Join protection

A hosting PC checks every login before the engine creates a player:

| Setting (`LanNative\config.txt`) | Default | Meaning |
|---|---|---|
| `joinprotect = key / open` | `key` | `key`: the joiner must send `?LNKey=<key>` (added to its join URL automatically when it has the same key). `open`: no key; the other checks still apply |
| `joinkey = <4-32 chars A-Z a-z 0-9 - _>` | random, kept in `LanNative\joinkey.txt` | The shared key. Shown on the host's screen when it starts hosting; never written to the log. Joiners put the same `joinkey = ...` in their config. Change the random key by editing `joinkey.txt` |
| `allow = a.b.c.d[/bits]` (repeatable, up to 16) | none = any address | Only these addresses may join, e.g. `allow = 26.0.0.0/8` for a Radmin network |
| `maxpeers = N / off` | 4 | Joiners at once (0-64; `off` or 0 = unlimited) |

5 failed tries from one address within a minute lock that address out for 5 minutes (also with the right key). Peer addresses are masked in the log (`26.12.0.x`). If the check itself faults, the login is refused.

Limits: the key travels in the clear. It keeps strangers out; it is no protection against someone who can read the traffic, so never reuse a password as the key. The mod check is a courtesy check, not security.

## Playing over the internet: VPN safety and responsibility

The official online mode is not supported and almost certainly will not work with this loader, which needs EasyAntiCheat off. Never use mods in the official online mode. To play with friends who are not on your network, use a private LAN connection or a VPN / virtual LAN that makes everyone look like they are on one network.

You are responsible for how you do this. Some basics:

* **Only play with people you trust.** A host accepts connections from every address that can reach it. Anyone on the same virtual network can try to join.
* **Keep the join key private.** The loader requires one by default. Do not post it publicly, and do not turn the key check off (`joinprotect = open`) on a network you do not fully control.
* **Never port-forward the game to the open internet** and never bind it to a public address. Use `allow = <range>` and `maxpeers` in `config.txt` to limit who can connect.
* **A VPN provider sees your traffic metadata and your friends' addresses.** Read the terms and privacy policy of whatever VPN or virtual-LAN software you choose, and only install software you trust.
* **Everyone should run the same gameplay mods.** The loader warns when the mods differ.
* **A plugin from a friend is a program.** Do not enable a `plugin.dll` you have not checked, even if it comes from someone you play with.
* **No promises.** LAN and VPN play are best effort. This is a hobby project and I take no responsibility for what happens.

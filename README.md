# Absolver LAN Mod (LanNative)

LAN / VPN multiplayer for **Absolver**, for offline mode. One player hosts, others join by IP address. Co-op in the open
world, 1v1 and 3v3 duels, host-side NPC spawning, and a fix for the Forsaken "cursed parry" bug.

LanNative is a small `version.dll` proxy loader that the game picks up from its own folder. It hooks a few of the game's own
functions by address and does its work on the game thread. There is no UE4SS and no Lua.

> Offline / LAN play only. It needs the Steam launch option `-NoEAC` and does nothing without it. It never disables or
> bypasses EasyAntiCheat and never contacts official services. Not affiliated with Sloclap. No game files are included.

Game build: Absolver 1.31 (b1.25_575). On any other build LanNative refuses to patch anything (it checks the original bytes).

## Features
- Host with F6, join with F7 (`ip.txt`), in-game menu on F1
- Joiner fixes: walking, leftover characters removed (also after leave and rejoin), respawn, leaving players cleaned up
- 1v1 / 3v3 duels offline: match flow, end screen, rematch, auto-leave when the other player drops
- NPC spawning (host): 280 NPC types in groups (open world, minibosses, bosses, PvE), works in duels too; NPC kills don't score
- Forsaken parry fix (one byte patched in memory), optional logging/telemetry for netcode research
- Single settings file `config.txt`

## Install (players)
Take a release zip (or build one, below), close the game and run `Install.bat` on each PC. See `package/README.txt` for keys,
settings and troubleshooting, and for how to play over a VPN (Hamachi, Radmin VPN, ZeroTier).

## Playing over a VPN (Hamachi, Radmin VPN, ZeroTier)
A VPN program gives every player a virtual LAN address, so the game sees them as one local network and the only thing that
changes is which address the joiners type. The LAN steps are tested; the VPN steps below have not been tried in a real
session yet.

1. Everyone installs the same VPN program. One person creates a private network (name + password), the others join it.
   Hamachi's free plan allows 5 people per network. Start the VPN before the game.
2. Check that every player shows as connected in the VPN window. A direct connection is better than a relayed one (less lag).
3. Everyone installs LanNative (same version) and adds the Steam launch option `-NoEAC`.
4. **Firewall (the usual problem):** Windows often treats a VPN network as "Public". On the host, allow
   `Absolver-Win64-Shipping.exe` on that network profile, or add an inbound UDP rule for port 7777.
5. **Host:** copy your VPN address (Hamachi: right-click your name, Copy IPv4, looks like `25.x.x.x`; Radmin VPN shows
   `26.x.x.x`), send it to the others, load a character into the open world and press **F6**.
6. **Joiners:** put the host's VPN address on line 1 of `LanNative\ip.txt`, load a character into the world and press **F7**.
   For duels the host picks a 1v1 / 3v3 map in the F1 menu; if a joiner can't get in, put `none` on line 2 of `ip.txt`.

If it doesn't connect:
- Ping the host's VPN address from the joiner's PC. If the ping fails, it is a VPN or firewall problem, not LanNative.
- If the host has several network adapters, set `hostip = <host VPN address>` in `config.txt` on the host and restart the game
  (or pick it in F1 > Host address). A set address means *only* that address can join; set it back to `auto` for normal LAN play.
- Compare the first line of `lannative.log` on every PC to make sure the versions match.

Expect extra lag over a VPN, especially when relayed; the game limits each player to about 20 KB/s. LanNative has no login
check, so the VPN network password is your only access control: use a strong one and only invite people you trust.

## Build
Needs Windows, MSVC Build Tools 2022, Python with `pefile` (only to regenerate headers).

```
cd lannative
build.bat                 # builds version.dll
cd test
build_test.bat            # offline tests (fake game memory), prints "N passed, 0 failed"
```

`gen_headers.py` regenerates `lannative_targets.h` and `version_exports.h` from your own copy of the game exe;
`gen_npcs.py` regenerates `package/npcs.txt` from an extraction of the game's data (you must provide that yourself).

To make an install package, copy `version.dll` into a folder together with the files in `package/`.

## Layout
- `lannative/` - source: `lannative.c` (loader, hooks, menu, config), `lan_logic.c` (game logic over raw memory through a small
  API table, so it is testable), `lan_telemetry.c`, small header-only modules, `test/`
- `package/` - installer, user README, NPC / map / altar lists
- `docs/` - `how-it-works.md` (design notes, start here) and research notes on the parry bug, the multiplayer architecture and NPC spawning

## Releases and the DLL
The compiled `version.dll` is not stored in the repo. It is built from the source here and shipped inside the release zip
(Releases page). The release notes list the SHA-256 of the zip and of the DLL inside it, so you can check what you downloaded.
Builds are not bit-for-bit reproducible (the compiler embeds paths and timestamps), so a DLL you build yourself will have a
different hash; it is the same code.

| Release | zip SHA-256 | version.dll SHA-256 |
|---|---|---|
| v4.3.1 | `42CF8301864066E4772E31CC35AB65449A8051D820A8403FDF1D1EA8D3DBA1E7` | `A54ABC434E908FA658B379695A35FEB0399B2E89C985E15D32567737F42B0CEE` |

The source in `lannative/` is the original source, not decompiled code.

## Safety
There is no login check: anyone who can reach the host's port can join. Only play with people you trust, preferably over a
private VPN, and don't forward the port on your router. Some antivirus programs flag the DLL because of how it loads
(false positive, see `docs/av-false-positive-report.md`).

## License
MIT, see `LICENSE`.

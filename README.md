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
A VPN gives every player a virtual LAN address, so the game treats you as one local network. (The LAN steps are tested; the VPN
steps have not been tried in a real session yet.)

**Everyone**
1. Install the same VPN program. One person creates a private network (name + password) and the others join it. Keep it running
   while you play. Hamachi's free plan allows 5 people.
2. Run `Install.bat` from the LanNative zip and add the Steam launch option `-NoEAC`.

**Host**
3. Allow the game through the firewall: Windows Defender Firewall > Allow an app > `Absolver-Win64-Shipping.exe`, tick the
   network type your VPN uses (usually Public). Or add an inbound UDP rule for port 7777.
4. Copy your VPN address (Hamachi: right-click your name > Copy IPv4, like `25.x.x.x`; Radmin VPN: like `26.x.x.x`) and send it
   to the others.
5. Load a character into the open world and press **F6**.

**Joiners**
6. Open `LanNative\ip.txt` and replace line 1 with the host's VPN address.
7. Load a character into the open world and press **F7**.

For a duel, the host picks a 1v1 / 3v3 map in the F1 menu and the joiners press F7 again. If a joiner can't get into a duel, put
the word `none` on line 2 of `ip.txt`.

**Can't connect?**
- Ping the host's VPN address from the joiner's PC. If the ping fails, it is a VPN or firewall problem.
- Host with several network adapters: set `hostip = <host VPN address>` in `config.txt` and restart the game. Set it back to `auto`
  for normal LAN play.
- Check that everyone has the same LanNative version (first line of `lannative.log`).

Expect some extra lag over a VPN. LanNative has no login check, so the VPN password is your only protection: use a strong one
and only invite people you trust.

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
| v4.3.1 | `2C9D3AA1442C10994343A3628F7C98C4F212EC3541D6DA8EBF14A201970E9DF9` | `A54ABC434E908FA658B379695A35FEB0399B2E89C985E15D32567737F42B0CEE` |

The source in `lannative/` is the original source, not decompiled code.

## Safety
There is no login check: anyone who can reach the host's port can join. Only play with people you trust, preferably over a
private VPN, and don't forward the port on your router. Some antivirus programs flag the DLL because of how it loads
(false positive, see `docs/av-false-positive-report.md`).

## License
MIT, see `LICENSE`.

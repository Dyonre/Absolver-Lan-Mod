LanNative 4.3.1 - LAN / VPN multiplayer for Absolver
====================================================

Lets two or more PCs play Absolver together over a LAN or a VPN (Radmin, Hamachi, ...). One player hosts, the others
join by IP address. Co-op in the open world and 1v1 / 3v3 duels work; the host can also spawn NPCs.
Offline / LAN play only. It does not use Sloclap's servers and does not touch EasyAntiCheat.
Game version: Absolver 1.31 (b1.25_575). On any other build LanNative refuses to patch anything.


REQUIREMENTS
------------
- Steam launch option  -NoEAC  (Steam > Absolver > Properties > Launch Options). Without it LanNative does nothing at all.
- Every player runs the same LanNative version.
- The old UE4SS Lua mod "LanDirect" must be off (set  LanDirect : 0  in Mods\mods.txt), otherwise both mods handle F6/F7.


INSTALL
-------
1. Close the game. Unzip LanNative, run Install.bat on EACH PC. It asks for the host's IP address.
2. It copies version.dll next to the game exe and creates a LanNative folder with the settings and logs.
   It refuses to overwrite a version.dll that belongs to another mod.
3. Uninstall.bat removes version.dll again (the LanNative folder stays).


QUICK START
-----------
1. Everyone loads a character into the open world.
2. Host: press F6.
3. Joiners: put the host's IP on line 1 of  LanNative\ip.txt  (the installer can do this), then press F7.
4. To leave as a joiner, press F6 twice within 5 seconds (one press only shows a warning, so a stray key can't drop you).

Over the internet with friends: see PLAYING OVER A VPN below.

PLAYING OVER A VPN (Hamachi, Radmin VPN, ZeroTier)
--------------------------------------------------
A VPN gives every player a virtual LAN address, so the game treats you as one local network. (The LAN steps are tested; the
VPN steps have not been tried in a real session yet.)

EVERYONE
 1. Install the same VPN program. One person creates a private network (name + password) and the others join it.
    Keep it running while you play. Hamachi's free plan allows 5 people.
 2. Run Install.bat from the LanNative zip and add the Steam launch option  -NoEAC .

HOST
 3. Allow the game through the firewall: Windows Defender Firewall > Allow an app > Absolver-Win64-Shipping.exe, and tick the
    network type your VPN uses (usually Public). Or add an inbound UDP rule for port 7777.
 4. Copy your VPN address (Hamachi: right-click your name > Copy IPv4, like 25.x.x.x; Radmin VPN: like 26.x.x.x)
    and send it to the others.
 5. Load a character into the open world and press F6.

JOINERS
 6. Open  LanNative\ip.txt  and replace line 1 with the host's VPN address.
 7. Load a character into the open world and press F7.

For a duel, the host picks a 1v1 / 3v3 map in the F1 menu and the joiners press F7 again. If a joiner can't get into a duel,
put the word  none  on line 2 of ip.txt.

CAN'T CONNECT?
 - Ping the host's VPN address from the joiner's PC. If the ping fails, it is a VPN or firewall problem.
 - Host with several network adapters: set  hostip = <host VPN address>  in config.txt and restart the game. Set it back to
   auto  for normal LAN play.
 - Check that everyone has the same LanNative version (first line of lannative.log).

Expect some extra lag over a VPN. LanNative has no login check, so the VPN password is your only protection: use a strong one
and only invite people you trust.

KEYS (game window focused)
--------------------------
F1   open / close the LanNative menu          F3   host: bring all joiners to you
F6   host a game                              F5   write a state dump to the log
F7   join the host in ip.txt                  F8   write a numbered marker line to the log

Menu (F1): Up/Down move, Enter or Right select, Left or Backspace go back.
  - Host, Join, Bring joiners, State dump, Log marker
  - Auto-leave on/off, 3v3 players per team, Host address
  - Map list: travel to a map (open world, 1v1, 3v3, PvE rooms)         [host]
  - Altar teleport: move your own character to one of the 42 altars     [host / single player]
  - Spawn NPC...: pick a group, then a type; Remove all NPCs            [host / single player]


DUELS (1v1 / 3v3)
-----------------
Host: F1 > Map list > pick a 1v1 or 3v3 map. Joiners follow with F7. LanNative makes the offline game run the whole
match: loading, intro, fight, end screen, rematch.
- If a joiner can't get into a duel, put the word  none  on line 2 of ip.txt and press F7 again.
- 3v3 normally needs 6 players. LanNative lowers it to 1 player per team (2 total) by default; change it in the menu
  or with  perteam  in config.txt.
- When the other player leaves, you are taken back to your own game after about 13 seconds (auto-leave).
- Kills of NPCs do not count for the duel score.


NPCs (host only)
----------------
F1 > Spawn NPC... shows groups: Open world, MiniBoss, Boss: Cargal & Kilnor, Boss: Kuretz, Boss: Risryn,
PvE Theme 1-3, PvE Bosses, PvE waves & support, Tutorial / test. Pick a group, then Enter on a type to spawn one next
to you. The menu stays open so you can spawn several. "Remove all NPCs" takes them away.
- Works in every map where you are the server, duels included. No limit on how many.
- Everyone can fight them, they give XP, and a killed NPC stays dead.
- Bosses are split into phases (Lv1-Lv4 are difficulty levels). Boss and miniboss entries use their own character
  Blueprint; if one misbehaves, delete the last column of its line in npcs.txt to use the generic fighter.
- In the open world, use "Remove all NPCs" before you save and quit.
- In duel maps without navigation data an NPC may stand still.
- Add your own entries to npcs.txt: group, label, archetype path [, character class path], separated by TABs.


SETTINGS
--------
All settings are in  LanNative\config.txt  (one  key = value  per line, # starts a comment). They apply when the
game starts. Install.bat creates the file and never overwrites an existing one.

  parryfix   on / off     Fixes the Forsaken "cursed parry" bug (patches one byte in memory at start).      default on
  pvpfix     on / guard / off   on = duels work (send-back guard + match flow). guard = guard only.        default on
  perteam    1 / 2 / 3 / off    Players per team in 3v3 (3 or off = the game's own 6).                     default 1
  autoleave  on / off     Leave a duel when the other player's connection is gone.                         default on
  menu       on / off     The F1 menu.                                                                     default on
  npc        on / off     NPC spawning in the menu.                                                        default on
  npcrepl    on / off     Also replicate the NPC helper actor to joiners (try on if joiners see NPCs wrong). default off
  telemetry  off / basic / verbose [seconds]   Logging of network and player data (read-only).            default basic
  hostip     auto / IPv4  Address the host listens on. auto = every adapter. Repeat the line for several
                          choices; the menu cycles them. A set address means ONLY that address can join.  default auto

Other files in the LanNative folder:
  ip.txt        line 1 = host IP (or IP:port). Line 2 (optional) = extra join options, or  none  for duels.
  npcs.txt      the NPC list for the menu.
  maps.txt      the map list for the menu.        altars.tsv   the altar list for the menu.
  commands.txt  console commands the game understands. Open the console with the Tilde key; useful ones are
                "Stat FPS" and "Open <ip>".


WHAT IT FIXES
-------------
- Joiners can walk (their character arrives with the wrong network role) and leftover copies of characters are removed,
  including after leaving and rejoining.
- Respawn works for joiners; a leaving player's body is removed from the host.
- A crash when a player dies or leaves is avoided (the game's co-op voice chat flag is cleared).
- Duels (see above) and the Forsaken parry fix.


LOGS AND TROUBLESHOOTING
------------------------
Everything is logged to  <game folder>\Binaries\Win64\LanNative\lannative.log . The first line shows the version;
check it on every PC, since an old copy on one machine causes confusing results.
- Nothing happens at all: the -NoEAC launch option is missing (the log says so).
- F6 / F7 seem ignored: while you are a joiner they need a second press within 5 seconds (the log says so).
- Can't connect: check the IP, the firewall (UDP 7777) and that both PCs run the same version.
- Hosting fails right after setting hostip: the address isn't on this PC. Set it back to  auto .
- If part of LanNative faults, that part switches itself off for the run and is logged; the game keeps running.
With telemetry on, three CSV files (telemetry_frames / telemetry_conns / telemetry_players) record frame times, lag, packet
rates and player positions during a session. When reporting a problem, send them with lannative.log from BOTH PCs.


SAFETY
------
LanNative adds no login check: anyone who can reach the host's port can join. Only play with people you trust, ideally over
a private VPN network, and don't forward the game's port on your router.
It is a small DLL that hooks a few of the game's own functions, so some antivirus programs flag it (false positive;
the source is in the project).

Absolver Mod Launcher
=====================

Asks how to start Absolver each time you press Play:

  Modded   the loader (version.dll) and the mods in the Mods folder. EasyAntiCheat is off. Offline and private LAN play only;
           never use it for the official online mode.
  Vanilla  the unmodified game: the original EasyAntiCheat launcher, EasyAntiCheat on, no loader, no Mods folder. Online play works.

Mod paks placed directly in Absolver\Content\Paks load only in Vanilla starts (the engine reads that folder itself). In Modded starts
the loader mounts the game's own paks and the Mods folder only; content_paks = all in LanNative\config.txt mounts them as well.

Remember the choice: tick "Remember my choice" in the question window. It is saved in
Absolver\Binaries\Win64\LanNative\launch.txt ("modded" or "vanilla"). To see the question again, hold Shift while the game starts
(after pressing Play), or delete launch.txt.

How it works
  Modded: Steam's Play button normally runs Absolver.exe, the EasyAntiCheat launcher, and EasyAntiCheat refuses a modified game.
  The game itself has an offline switch (-NoEAC); the launcher starts Absolver-Win64-Shipping.exe with it and waits for the game to exit.
  Vanilla: the launcher renames Binaries\Win64\version.dll to version.dll.off, so the game loads no loader, then runs the original
  launcher (Absolver.exe.eac) with your options and waits for the game to exit. The next Modded start renames version.dll back.

Install: the installer (tools\installer\Install.bat) keeps the original Absolver.exe as Absolver.exe.eac and puts the launcher in its
place, so Steam's Play button shows the question. Uninstall.bat puts the original back.

Options (add them to the game's Steam launch options)
  -Modded   start modded without asking.
  -Vanilla  start vanilla without asking.
  -NoMods   start the loader without mods, if a mod stops the game from starting.
  Anything else is passed on to the game (in both modes).
  AbsolverModLauncher.exe --migrate   moves the extra paks in Content\Paks into their own folders under Mods (they then load only in
                                      Modded starts, in Mods\loadorder.txt order).

Build: build.bat (MSVC Build Tools 2022, x64).

Problems: see Absolver\Binaries\Win64\LanNative\launcher.log (every start, the chosen mode, every argument and exit code is logged).

Absolver Mod Launcher
=====================

Starts Absolver so that the loader (version.dll) and its mods work. Offline and private LAN play only. Never use it for the official
online mode.

Why it is needed: Steam's Play button runs Absolver.exe, which is the EasyAntiCheat launcher, and EasyAntiCheat refuses a modified game.
The game itself has an offline switch (-NoEAC); the launcher starts Absolver-Win64-Shipping.exe with it and waits for the game to exit.

Install: the installer (tools\installer\Install.bat) keeps the original Absolver.exe as Absolver.exe.eac and puts the launcher in its
place, so Steam's Play button starts the modded game. Uninstall.bat puts the original back.

Options (add them to the game's Steam launch options)
  -NoMods   start the loader without mods, if a mod stops the game from starting.
  Anything else is passed on to the game.

Build: build.bat (MSVC Build Tools 2022, x64).

Problems: see Absolver\Binaries\Win64\LanNative\launcher.log (every start, argument and exit code is logged).

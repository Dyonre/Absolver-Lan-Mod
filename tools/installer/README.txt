Gleam Loader
============

Hobby project, provided as is. I take no responsibility for anything that happens from using it.
Offline and private LAN play only. Never use mods in the official online mode.

Install
  1. Close the game. Unzip this folder anywhere.
  2. Double-click Install.bat. It finds the game through Steam.

  It copies version.dll (the loader) and AbsolverModLauncher.exe into the game folder, keeps the original Absolver.exe as
  Absolver.exe.eac and puts the launcher in its place, and creates a Mods folder. It never touches EasyAntiCheat files, your paks or your saves.
  Every step is written to install.log next to the installer.
  It also asks GitHub whether a newer Gleam Loader release exists and tells you (it downloads nothing; Install.bat -NoUpdateCheck skips it).
  The loader itself never goes online. Its version is the first line of lannative.log and in version.dll's file properties.

Use
  Press Play in Steam. To add a mod, put its folder in <game folder>\Mods\.
  If a mod stops the game from starting, add -NoMods to the game's Steam launch options.

Uninstall
  Double-click Uninstall.bat. Your Mods folder stays. Run it before playing the official online mode.
  Steam's "Verify integrity of game files" also restores the original Absolver.exe; run Install.bat again afterwards.

Problems
  Read Absolver\Binaries\Win64\LanNative\launcher.log and lannative.log (lines starting "mods:").
  Guides for making mods are in the repository (docs\).

PlusCompat - example mod for use with AbsolverPlus
===================================================

AbsolverPlus is not included here. Get it from its own page and put its files into the Mods folder like this:

  <game folder>\Mods\AbsolverPlus\   <- the Plus .pak and .sig files go in this folder
  <game folder>\Mods\PlusCompat\     <- this example

Then start the game. Absolver\Binaries\Win64\LanNative\mods.txt should list AbsolverPlus first and PlusCompat after it, and rows.txt
should say "0 failed".

How it aims to stay compatible
  Two paks that contain the same file do not merge: one wins. Plus ships its own attacks table, so a pak with another copy of that
  table would hide either Plus's rows or yours. This mod does not ship a table. Its rows.json adds one move (PlusCompat_QuickJab) as a
  copy of the Jab while the game loads the table, so it works on top of Plus's table, or on the normal one if Plus is not installed.
  It loads after Plus ("after" in mod.json) and uses a prefixed row name so it cannot collide with Plus or other mods.

To make your own, copy this folder, rename it and the row, and change the values. See docs/MODDING-GUIDE.md.

Disclaimer
  AbsolverPlus belongs to its own authors; this example is not affiliated with them and includes none of their files. If the AbsolverPlus
  authors do not want this example in the repository, it will be removed.

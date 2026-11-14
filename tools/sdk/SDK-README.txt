Gleam Loader SDK folder
=======================

AbsolverSE.h   The plugin API header (Gleam plugin API, version 2). Include it in your plugin; every
               function is documented in its comments.

Build a plugin with MSVC Build Tools 2022 (x64), from your plugin folder:

  cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I "<this folder>" /LD plugin.c /Fe:plugin.dll /link user32.lib kernel32.lib

Install it: put plugin.dll, mod.json and an optional plugin.cfg in <game folder>\Mods\<YourPlugin>\.
The first time the game starts, the loader adds the plugin's name to Mods\plugins.txt with a # in front;
remove the # to enable it. Plugins are programs: the loader never loads one that is not enabled.

The repository (https://github.com/Dyonre/GleamLoader) has the guide (docs/sdk/README.md), the full
reference (docs/PLUGIN-API.md) and a scaffold script that writes a ready-to-build plugin:

  powershell -ExecutionPolicy Bypass -File tools\sdk\New-Plugin.ps1 -Name MyPlugin -Sdk <this folder> -Build

Offline and private LAN play only. Provided as is.

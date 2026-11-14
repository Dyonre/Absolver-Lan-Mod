# Security Policy

## Supported Versions

This is a hobby project with no support commitment. Only the latest release is considered current.

| Version | Supported |
| ------- | --------- |
| latest release | :white_check_mark: |
| older | :x: |

## Reporting a Vulnerability

Please use GitHub's private vulnerability reporting ("Security" tab > "Report a vulnerability") instead of opening a public issue. Reports are handled on a best-effort basis, with no guaranteed response time.

## Notes for users

* Mods can be programs. A mod containing `plugin.dll` runs native code with your user rights. The loader does not load a plugin until you enable it in `Mods\plugins.txt`. Only install mods from sources you trust.
* The loader is a proxy DLL (`version.dll`) that patches the game in memory, so antivirus products may flag it. The source is in this repository. It refuses to run with EasyAntiCheat, and it is meant for offline and private LAN play only.

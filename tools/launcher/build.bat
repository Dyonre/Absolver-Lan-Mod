@echo off
rem Builds AbsolverModLauncher.exe (GUI subsystem, static CRT: no runtime to install) and runs the argument checks first.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS test.c /Fe:launcher_test.exe || exit /b 1
.\launcher_test.exe || exit /b 1
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS launcher.c /Fe:AbsolverModLauncher.exe /link /NOLOGO /SUBSYSTEM:WINDOWS user32.lib shell32.lib advapi32.lib || exit /b 1
rem Give the launcher the game's own icon, copied from your installed game exe (a game asset: never committed).
rem Without it Windows shows its default app icon for Absolver.exe. Set ABSOLVER_GAME to the game folder (the one with Absolver.exe) first.
if not defined ABSOLVER_GAME echo NOTE: ABSOLVER_GAME is not set - the launcher keeps the default icon
if not defined ABSOLVER_GAME goto done
set "GAMEEXE=%ABSOLVER_GAME%\Absolver\Binaries\Win64\Absolver-Win64-Shipping.exe"
cl /nologo /O2 /MT /W3 seticon.c /Fe:seticon.exe /link /NOLOGO || exit /b 1
if not exist "%GAMEEXE%" echo WARNING: game exe not found - the launcher keeps the default icon (set ABSOLVER_GAME)
if not exist "%GAMEEXE%" goto done
.\seticon.exe "%GAMEEXE%" AbsolverModLauncher.exe || exit /b 1
:done
echo built: %CD%\AbsolverModLauncher.exe

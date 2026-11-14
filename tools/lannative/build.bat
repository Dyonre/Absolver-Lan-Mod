@echo off
rem Builds the loader with MSVC Build Tools 2022 (x64). Usage (run from this folder):
rem   build.bat        -> version.dll      (the normal build)
rem   build.bat dev    -> version.dev.dll  (adds a developer shot runner; do not ship it)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem The script extender needs symbols.bin, made from your own copy of the game (README.md in this folder): stop instead of building without it.
if not exist "%~dp0symbols.bin" (
  echo ERROR: %~dp0symbols.bin is missing. Generate it first: see README.md in this folder.
  exit /b 1
)
rc /nologo /fo lannative.res lannative.rc || exit /b 1
call :imgui_objs || exit /b 1
if /I "%~1"=="dev" (
  cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DLN_OVERLAY %IMCL% /DLANNATIVE_SHOTRUNNER /LD lannative.c lan_logic.c lan_telemetry.c lannative.res overlay.cpp %IMOBJ% /Fe:version.dev.dll /link /NOLOGO /RELEASE %IMLINK% user32.lib kernel32.lib shlwapi.lib || exit /b 1
  echo built: %CD%\version.dev.dll
) else (
  cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DLN_OVERLAY %IMCL% /LD lannative.c lan_logic.c lan_telemetry.c lannative.res overlay.cpp %IMOBJ% /Fe:version.dll /link /NOLOGO /RELEASE %IMLINK% user32.lib kernel32.lib shlwapi.lib || exit /b 1
  echo built: %CD%\version.dll
)
exit /b 0

rem Dear ImGui (third_party\imgui, unmodified): no exceptions / RTTI, no gamepad / IME / shell helpers. The d3dcompiler and dwmapi imports are delay-loaded,
rem so no graphics DLL is loaded with the proxy DLL. Sets IMCL (compiler flags for overlay.cpp), IMOBJ (objects) and IMLINK (linker flags).
:imgui_objs
set "IMG=..\..\third_party\imgui"
if not exist obj mkdir obj
set "IMCL=/GR- /EHs-c- /FIoverlay_imconfig.h /I. /I%IMG% /I%IMG%\backends"
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS %IMCL% /c /Foobj\ %IMG%\imgui.cpp %IMG%\imgui_draw.cpp %IMG%\imgui_tables.cpp %IMG%\imgui_widgets.cpp %IMG%\backends\imgui_impl_dx11.cpp %IMG%\backends\imgui_impl_win32.cpp || exit /b 1
set "IMOBJ=obj\imgui.obj obj\imgui_draw.obj obj\imgui_tables.obj obj\imgui_widgets.obj obj\imgui_impl_dx11.obj obj\imgui_impl_win32.obj"
set "IMLINK=/DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dwmapi.dll delayimp.lib gdi32.lib"
exit /b 0

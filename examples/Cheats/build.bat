@echo off
rem Builds plugin.dll with MSVC Build Tools 2022 (x64): plugin.c (C) + cheatsui.cpp (C++) with its own copy of Dear ImGui 1.91.5 from third_party\imgui (the same version as the loader's)
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
set "L=..\..\tools\lannative"
set "IMG=..\..\third_party\imgui"
if not exist obj mkdir obj
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /c plugin.c /Foobj\plugin.obj || exit /b 1
cl /nologo /O2 /MT /W3 /GR- /EHs-c- /D_CRT_SECURE_NO_WARNINGS /FI%L%\overlay_imconfig.h /I%L% /I%IMG% /c cheatsui.cpp %IMG%\imgui.cpp %IMG%\imgui_draw.cpp %IMG%\imgui_tables.cpp %IMG%\imgui_widgets.cpp /Foobj\ || exit /b 1
link /nologo /DLL /OUT:plugin.dll obj\plugin.obj obj\cheatsui.obj obj\imgui.obj obj\imgui_draw.obj obj\imgui_tables.obj obj\imgui_widgets.obj user32.lib kernel32.lib || exit /b 1
echo built: %CD%\plugin.dll

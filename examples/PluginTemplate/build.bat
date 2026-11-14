@echo off
rem Builds plugin.dll with MSVC Build Tools 2022 (x64)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /LD plugin.c /Fe:plugin.dll /link /NOLOGO user32.lib kernel32.lib


@echo off
rem Builds version.dll (the LanNative proxy) with MSVC BuildTools. Usage: build.bat   (run from this folder)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rc /nologo /fo lannative.res lannative.rc || exit /b 1
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /LD lannative.c lan_logic.c lan_telemetry.c lannative.res /Fe:version.dll /link /NOLOGO /RELEASE user32.lib kernel32.lib || exit /b 1
echo built: %CD%\version.dll

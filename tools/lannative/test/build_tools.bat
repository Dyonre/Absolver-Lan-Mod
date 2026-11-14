@echo off
rem Builds the mod-checking tools reccheck.exe, modscan.exe and ueapply.exe (MSVC Build Tools 2022, x64).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0" || exit /b 1
for %%f in (reccheck modscan ueapply) do cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS %%f.c /Fe:%%f.exe || exit /b 1
echo built: reccheck.exe modscan.exe ueapply.exe

@echo off
rem Builds LuaHost\out\plugin.dll with MSVC Build Tools: plugin.c + the Lua 5.4 sources (third_party\lua, one unit lua_all.c).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0" || exit /b 1
if not exist out mkdir out
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I..\..\third_party\lua\src /LD plugin.c lua_all.c /Fo:out\ /Fe:out\plugin.dll /link /NOLOGO user32.lib kernel32.lib || exit /b 1
copy /y plugin.cfg out\plugin.cfg >nul
echo LUAHOST OK

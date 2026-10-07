@echo off
rem Builds and runs the offline tests (run from this folder): build_test.bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /W4 /D_CRT_SECURE_NO_WARNINGS /I.. logic_test.c ..\lan_logic.c ..\lan_telemetry.c /Fe:logic_test.exe || exit /b 1
logic_test.exe

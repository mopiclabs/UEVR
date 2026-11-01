@echo off
rem Builds burstcap.exe next to this file (see burstcap.cpp). Needs Visual Studio 2022 with the C++ workload.
setlocal EnableDelayedExpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo no Visual Studio found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /W4 burstcap.cpp /Fe:burstcap.exe /link user32.lib || exit /b 1
del /q burstcap.obj 2>nul

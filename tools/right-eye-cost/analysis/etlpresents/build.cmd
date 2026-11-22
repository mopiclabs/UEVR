@echo off
rem Builds etlpresents.exe next to this file. Needs Visual Studio 2022 with the C++ workload.
setlocal EnableDelayedExpansion
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo no Visual Studio found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /W3 etlpresents.cpp /Fe:etlpresents.exe /link advapi32.lib || exit /b 1
del /q etlpresents.obj 2>nul

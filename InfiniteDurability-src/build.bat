@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."
set "OUT_DIR=%ROOT_DIR%\Solarpunk\Binaries\Win64"
set "SRC=%SCRIPT_DIR%InfiniteDurability.cpp"
set "OUT=%OUT_DIR%\InfiniteDurability.dll"

call "E:\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist "%OUT_DIR%" exit /b 1

cl.exe /nologo /std:c++20 /EHsc /O2 /LD "%SRC%" /link /NOLOGO /OUT:"%OUT%"
exit /b %errorlevel%

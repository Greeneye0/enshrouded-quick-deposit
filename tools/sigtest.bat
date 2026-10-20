@echo off
rem usage: tools\sigtest.bat <path\to\enshrouded.exe>
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build\test mkdir build\test
cl /nologo /std:c++20 /EHsc /Fobuild\test\ /Fe:build\test\sigtest.exe tools\sigtest.cpp src\scan.cpp >nul || exit /b 1
build\test\sigtest.exe "%~1"

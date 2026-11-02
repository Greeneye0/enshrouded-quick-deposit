@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist test\out mkdir test\out
cl /nologo /std:c++20 /O2 /W4 /EHsc /Fo:test\out\altar_upgrade.obj /Fe:test\out\altar_upgrade.exe test\altar_upgrade.cpp || exit /b 1
test\out\altar_upgrade.exe
exit /b %errorlevel%

@echo off
rem Builds bin\powrprof.dll with the VS 2022 Build Tools.
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
if not exist bin mkdir bin
ml64 /nologo /c /Fo build\proxy_stubs.obj src\proxy_stubs.asm || exit /b 1
cl /nologo /std:c++20 /O2 /MT /W4 /EHsc /DUNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /c src\*.cpp || exit /b 1
link /nologo /DLL /OUT:bin\powrprof.dll /DEF:src\powrprof.def build\*.obj user32.lib kernel32.lib || exit /b 1
echo Built bin\powrprof.dll

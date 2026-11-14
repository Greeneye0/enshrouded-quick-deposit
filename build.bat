@echo off
rem Builds Quick Deposit with the VS 2022 Build Tools: bin\loader\ for the Greeneye Loader (the
rem release) and the same DLL as the standalone bin\powrprof.dll.
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
if not exist bin mkdir bin
ml64 /nologo /c /Fo build\proxy_stubs.obj src\proxy_stubs.asm || exit /b 1
cl /nologo /std:c++20 /O2 /MT /W4 /utf-8 /EHsc /DUNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /c src\*.cpp || exit /b 1
link /nologo /DLL /OUT:bin\powrprof.dll /DEF:src\powrprof.def build\*.obj user32.lib kernel32.lib advapi32.lib winmm.lib || exit /b 1
rem The same DLL for the Greeneye Loader: greeneye\quick-deposit\quick-deposit.dll + mod.json.
if not exist bin\loader mkdir bin\loader
copy /y bin\powrprof.dll bin\loader\quick-deposit.dll >nul || exit /b 1
copy /y mod.json bin\loader\mod.json >nul || exit /b 1
echo Built bin\powrprof.dll (standalone) and bin\loader\quick-deposit.dll (Greeneye Loader)

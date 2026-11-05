@echo off
rem Builds FCStruct2.dll with the installed VS Build Tools (MSVC x64). Output: build\FCStruct2.dll
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /LD /DNDEBUG /DWIN32_LEAN_AND_MEAN FCStruct2.cpp ^
   /Fo:build\ /Fe:build\FCStruct2.dll /link /NOLOGO shell32.lib user32.lib || exit /b 1
echo built build\FCStruct2.dll

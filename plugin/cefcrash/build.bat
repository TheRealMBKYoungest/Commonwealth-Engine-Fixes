@echo off
rem Builds CEFCrashLog.dll with the installed VS Build Tools (MSVC x64). Output: build\CEFCrashLog.dll
rem Test builds: build.bat /DCEF_TEST_CRASH=1 (main-thread crash at game data ready) or =2 (worker-thread crash after 45 s)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /LD /DNDEBUG /DWIN32_LEAN_AND_MEAN %* CEFCrashLog.cpp ^
   /Fo:build\ /Fe:build\CEFCrashLog.dll /link /NOLOGO shell32.lib || exit /b 1
echo built build\CEFCrashLog.dll

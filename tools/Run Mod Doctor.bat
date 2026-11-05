@echo off
rem CEF Mod Doctor: checks your plugins, masters, F4SE plugins and game settings on this PC. Sends nothing anywhere.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 -I check_mods.py %* & goto done)
where python >nul 2>nul && (python -I check_mods.py %* & goto done)
echo Python 3 is needed to run the CEF Mod Doctor.
echo Get it free from https://www.python.org/downloads/ (tick "Add python.exe to PATH"), then run this again.
:done
echo.
pause
@echo off
rem CEF log analyzer: reads your Fallout 4 logs on this PC and explains them. Sends nothing anywhere.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 -I analyze_logs.py %* & goto done)
where python >nul 2>nul && (python -I analyze_logs.py %* & goto done)
echo Python 3 is needed to run the CEF log analyzer.
echo Get it free from https://www.python.org/downloads/ (tick "Add python.exe to PATH"), then run this again.
:done
echo.
pause
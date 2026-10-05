@echo off
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0set-version.ps1" %*
pause

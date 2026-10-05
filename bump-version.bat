@echo off
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0bump-version.ps1" %*
pause

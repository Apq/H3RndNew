@echo off
chcp 936 >nul
rem 成功后是否停住窗口：0=直接关闭（默认，保持原行为），1=pause 等按键。
rem 双击前可在本文件改默认值；命令行可覆盖：set PAUSE_ON_SUCCESS=1 && deploy.bat
if not defined PAUSE_ON_SUCCESS set PAUSE_ON_SUCCESS=0
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1"
if errorlevel 1 (
    pwsh -c "Write-Host '部署失败' -ForegroundColor Red"
    pause
    exit /b 1
)
pwsh -c "Write-Host '部署完成' -ForegroundColor Green"
if "%PAUSE_ON_SUCCESS%"=="1" pause

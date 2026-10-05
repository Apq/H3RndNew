@echo off
chcp 936 >nul
rem 成功后是否暂停窗口：0=直接关闭（默认），1=pause 按任意键再关
rem 双击运行当前批处理时取默认值，命令行里可覆盖：set PAUSE_ON_SUCCESS=1 && pack.bat
if not defined PAUSE_ON_SUCCESS set PAUSE_ON_SUCCESS=0
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack.ps1" %*
if errorlevel 1 (
    pwsh -c "Write-Host '打包失败' -ForegroundColor Red"
    pause
    exit /b 1
)
pwsh -c "Write-Host '打包完成' -ForegroundColor Green"
if "%PAUSE_ON_SUCCESS%"=="1" pause

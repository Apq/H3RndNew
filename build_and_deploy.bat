@echo off
chcp 936 >nul
setlocal
rem 成功后是否停住窗口：0=直接关闭（默认，保持原行为），1=pause 等按键。
rem 双击前可在本文件改默认值；命令行可覆盖：set PAUSE_ON_SUCCESS=1 && build_and_deploy.bat
if not defined PAUSE_ON_SUCCESS set PAUSE_ON_SUCCESS=0
rem 清代理变量：HTTP_PROXY 与 http_proxy 同时存在时 MSBuild 报 MSB6001。
set HTTP_PROXY=& set http_proxy=& set HTTPS_PROXY=& set https_proxy=& set ALL_PROXY=& set all_proxy=& set NO_PROXY=& set no_proxy=

pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if errorlevel 1 (
    pwsh -c "Write-Host '编译失败' -ForegroundColor Red"
    goto :error
)

echo 正在部署...
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1"
if errorlevel 1 (
    pwsh -c "Write-Host '部署失败' -ForegroundColor Red"
    goto :error
)

pwsh -c "Write-Host '编译并部署完成' -ForegroundColor Green"
if "%PAUSE_ON_SUCCESS%"=="1" pause
exit /b 0

:error
echo.
pause
exit /b 1

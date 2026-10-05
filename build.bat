@echo off
chcp 936 >nul
rem 成功后是否停住窗口：0=直接关闭（默认，保持原行为），1=pause 等按键。
rem 双击前可在本文件改默认值；命令行可覆盖：set PAUSE_ON_SUCCESS=1 && build.bat
if not defined PAUSE_ON_SUCCESS set PAUSE_ON_SUCCESS=0
rem 清代理变量：HTTP_PROXY 与 http_proxy 同时存在时，MSBuild 的 .NET 环境字典
rem 键冲突（大小写不敏感）会报 MSB6001，编译直接失败。
set HTTP_PROXY=& set http_proxy=& set HTTPS_PROXY=& set https_proxy=& set ALL_PROXY=& set all_proxy=& set NO_PROXY=& set no_proxy=
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe" H3RndNew.vcxproj /p:Configuration=Release /p:Platform=Win32 /m /t:Rebuild
if errorlevel 1 (
    pwsh -c "Write-Host '编译失败' -ForegroundColor Red"
    pause
    exit /b 1
)
pwsh -c "Write-Host '编译完成' -ForegroundColor Green"
if "%PAUSE_ON_SUCCESS%"=="1" pause

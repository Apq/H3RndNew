$ErrorActionPreference = 'Stop'
$gameDir = 'D:\Heroes3\Heroes3_2026.05.01'
$packsDst = "$gameDir\_HD3_Data\Packs\真随机开局"
$src = "$PSScriptRoot\Release"

try {
    if (-not (Test-Path $packsDst)) {
        New-Item -ItemType Directory -Path $packsDst -Force | Out-Null
    }
    $dll = Join-Path $src 'H3RndNew.dll'
    if (-not (Test-Path $dll)) {
        throw "未找到编译输出：$dll"
    }
    Copy-Item $dll $packsDst -Force
    Copy-Item "$PSScriptRoot\H3RndNew.default.ini" $packsDst -Force
    Copy-Item "$PSScriptRoot\使用说明.txt" $packsDst -Force

    Write-Host "已部署到 $packsDst"
} catch {
    Write-Host "部署错误: $_"
    exit 1
}

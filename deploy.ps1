$ErrorActionPreference = 'Stop'
# 共享部署目标：$env:H3_GAME_DIR > 仓库根 H3Env.ps1 > 内置默认值（换版本改 H3Env.ps1）。
$h3root = Split-Path -Parent $PSScriptRoot
if (Test-Path -LiteralPath "$h3root\H3Env.ps1") { . "$h3root\H3Env.ps1" }
if (-not (Get-Command Get-H3GameDir -ErrorAction SilentlyContinue)) {
    function Get-H3GameDir {
        if ($env:H3_GAME_DIR -and (Test-Path -LiteralPath $env:H3_GAME_DIR)) { $env:H3_GAME_DIR }
        else { 'D:\Heroes3\Heroes3_2026.10.09' }
    }
}
$gameDir = Get-H3GameDir
$packsDst = "$gameDir\_HD3_Data\Packs\热血插件"
$src = "$PSScriptRoot\Release"

try {
    # 在创建目标目录、复制DLL或任何素材之前，校验全部源文件和精确48项清单。
    $dll = Join-Path $src 'H3RndNew.dll'
    $sourceFiles = @($dll, "$PSScriptRoot\H3RndNew.default.ini", "$PSScriptRoot\使用说明.txt")
    foreach ($sourceFile in $sourceFiles) {
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
            throw "部署文件缺失：$sourceFile"
        }
    }
    $uiAssets = Join-Path $PSScriptRoot 'whui'
    $expectedAssets = @(
        foreach ($button in @('open', 'close', 'take', 'put')) {
            foreach ($state in 0..3) { "btn_$button$state.pcx" }
        }
        foreach ($tab in 0..11) {
            foreach ($state in @('n', 's')) { 'tab{0:00}{1}.pcx' -f $tab, $state }
        }
        'arrow_prev0.pcx'; 'arrow_prev1.pcx'; 'arrow_next0.pcx'; 'arrow_next1.pcx'
        'pager_n.pcx'; 'pager_s.pcx'
        'tidy_n.pcx'; 'tidy_s.pcx'
    )
    $assets = @(Get-ChildItem -LiteralPath $uiAssets -Filter '*.pcx' -File)
    $assetNames = @($assets | ForEach-Object { $_.Name })
    if ($assets.Count -ne 48 -or @(Compare-Object $expectedAssets $assetNames).Count -ne 0) {
        throw "仓库UI素材清单不匹配：期望48个指定PCX，实际$($assets.Count)"
    }

    New-Item -ItemType Directory -Path $packsDst -Force | Out-Null
    foreach ($sourceFile in $sourceFiles) { Copy-Item -LiteralPath $sourceFile -Destination $packsDst -Force }
    $uiDst = Join-Path $packsDst 'whui'
    New-Item -ItemType Directory -Path $uiDst -Force | Out-Null
    foreach ($staleName in @('title.pcx', 'tab12n.pcx', 'tab12s.pcx', 'tab13n.pcx', 'tab13s.pcx')) {
        $staleAsset = Join-Path $uiDst $staleName
        if (Test-Path -LiteralPath $staleAsset) {
            $resolvedAsset = (Resolve-Path -LiteralPath $staleAsset).Path
            $resolvedUiDst = (Resolve-Path -LiteralPath $uiDst).Path
            if ([System.IO.Path]::GetDirectoryName($resolvedAsset) -ne $resolvedUiDst -or
                [System.IO.Path]::GetFileName($resolvedAsset) -ne $staleName -or
                (Get-Item -LiteralPath $resolvedAsset).PSIsContainer) {
                throw "拒绝删除非目标旧素材：$resolvedAsset"
            }
            Remove-Item -LiteralPath $resolvedAsset -Force
        }
    }
    $assets | Copy-Item -Destination $uiDst -Force

    Write-Host "已部署到 $packsDst"
} catch {
    Write-Host "部署错误: $_"
    exit 1
}

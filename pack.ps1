param(
    [string]$Source,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Text.Encoding.CodePages -ErrorAction SilentlyContinue
[System.Text.Encoding]::RegisterProvider([System.Text.CodePagesEncodingProvider]::Instance)

# 2026-10-10 用户裁定：打包不再读游戏部署目录；DLL 取项目 Release 构建产物，
# 配置/说明取仓库源文件。包内容保持既有清单，不扩为整个 Release 目录。
# $Source 参数保留只为兼容旧调用，已不参与取材。
if (-not $OutputDir) {
    $OutputDir = Join-Path $PSScriptRoot 'Release'
}

function Get-PackVersion {
    $rcPath = Join-Path $PSScriptRoot 'H3RndNew.rc'
    if (-not (Test-Path -LiteralPath $rcPath)) { return 'unknown' }
    $text = [System.Text.Encoding]::GetEncoding(936).GetString(
        [System.IO.File]::ReadAllBytes($rcPath))
    $match = [regex]::Match($text,
        '(?m)^\s*VALUE\s+"FileVersion"\s*,\s*"([^"]+)"')
    if (-not $match.Success) { return 'unknown' }
    $parts = $match.Groups[1].Value.Split('.')
    return 'v' + ($parts[0..1] -join '.')
}

# 显式清单只取本插件需要的文件；包内顶层目录沿用原压缩包目录名（热血插件/）。
$files = @(
    @{ Local = Join-Path $PSScriptRoot 'Release\H3RndNew.dll'; Entry = '热血插件/H3RndNew.dll' },
    @{ Local = Join-Path $PSScriptRoot 'H3RndNew.default.ini'; Entry = '热血插件/H3RndNew.default.ini' },
    @{ Local = Join-Path $PSScriptRoot '使用说明.txt';         Entry = '热血插件/使用说明.txt' }
)

$expectedAssets = @(
    foreach ($button in @('open', 'close', 'take', 'put')) {
        foreach ($state in 0..3) { "btn_$button$state.pcx" }
    }
    foreach ($state in 0..3) { "tidy$state.pcx" }
    foreach ($tab in 0..11) {
        foreach ($state in @('n', 's')) { 'tab{0:00}{1}.pcx' -f $tab, $state }
    }
    'arrow_prev0.pcx'; 'arrow_prev1.pcx'; 'arrow_next0.pcx'; 'arrow_next1.pcx'
    'pager_n.pcx'; 'pager_s.pcx'
)
$uiAssets = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'whui') -Filter '*.pcx' -File)
$assetNames = @($uiAssets | ForEach-Object { $_.Name })
if ($uiAssets.Count -ne 50 -or @(Compare-Object $expectedAssets $assetNames).Count -ne 0) {
    throw "仓库UI素材清单不匹配：期望50个指定PCX，实际$($uiAssets.Count)"
}
foreach ($asset in $uiAssets) {
    $files += @{ Local = $asset.FullName; Entry = '热血插件/whui/' + $asset.Name }
}

# 输出ZIP创建/覆盖之前完成源文件校验，避免缺素材时留下半包。
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file.Local -PathType Leaf)) {
        throw "打包文件缺失: $($file.Local)"
    }
}

$version = Get-PackVersion
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$zipPath = Join-Path $OutputDir "真随机开局_$version.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

$included = 0
$zip = [System.IO.Compression.ZipFile]::Open(
    $zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $files) {
        if (-not (Test-Path -LiteralPath $file.Local)) {
            throw "打包文件缺失: $($file.Local)"
        }
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $file.Local, $file.Entry,
            [System.IO.Compression.CompressionLevel]::Optimal)
        $included++
        Write-Host "加入 $($file.Entry)"
    }
} finally {
    $zip.Dispose()
}

if ($included -eq 0) {
    Remove-Item -LiteralPath $zipPath -Force
    throw '没有可打包的文件。'
}

Write-Host "已打包 $included 个文件: $zipPath"

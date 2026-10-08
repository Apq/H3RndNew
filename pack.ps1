param(
    [string]$Source,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Text.Encoding.CodePages -ErrorAction SilentlyContinue
[System.Text.Encoding]::RegisterProvider([System.Text.CodePagesEncodingProvider]::Instance)

if (-not $Source) {
    $Source = 'D:\Heroes3\Heroes3_2026.10.07\_HD3_Data\Packs\热血插件'
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $PSScriptRoot 'Release'
}

$sourcePath = (Resolve-Path -LiteralPath $Source).Path
if (-not (Test-Path -LiteralPath $sourcePath -PathType Container)) {
    throw "打包源目录不存在: $sourcePath"
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

# 部署目录（热血插件）与 H3Auto 共用，混有其他插件的文件；
# 不能整目录排除法打包，改为显式清单只取本插件需要的文件：
# DLL 取已部署目录（包内 DLL 与 Release、部署现场三方一致），
# 配置与说明取仓库源目录（部署目录中的说明文件可能被其他插件覆盖）。
$files = @(
    @{ Local = Join-Path $sourcePath 'H3RndNew.dll';           Entry = '热血插件/H3RndNew.dll' },
    @{ Local = Join-Path $PSScriptRoot 'H3RndNew.default.ini'; Entry = '热血插件/H3RndNew.default.ini' },
    @{ Local = Join-Path $PSScriptRoot '使用说明.txt';         Entry = '热血插件/使用说明.txt' }
)

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

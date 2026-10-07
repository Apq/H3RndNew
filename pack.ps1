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
    $Source = 'D:\Heroes3\Heroes3_2026.10.07\_HD3_Data\Packs\真随机开局'
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

function Test-PackExcluded {
    param([string]$RelativePath)
    $name = [System.IO.Path]::GetFileName($RelativePath)
    if ($name -eq 'H3RndNew.user.ini') { return $true }
    if ($name -like '*.log') { return $true }
    return $false
}

$version = Get-PackVersion
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$zipPath = Join-Path $OutputDir "真随机开局_$version.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

$included = 0
$excluded = 0
$zip = [System.IO.Compression.ZipFile]::Open(
    $zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $prefix = $sourcePath.TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $sourcePath -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($prefix.Length)
        if (Test-PackExcluded $relative) {
            $script:excluded++
            Write-Host "排除 $relative"
            return
        }
        $entryName = ('真随机开局\' + $relative) -replace '\\', '/'
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $_.FullName, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal)
        $script:included++
        Write-Host "加入 $relative"
    }
} finally {
    $zip.Dispose()
}

if ($included -eq 0) {
    Remove-Item -LiteralPath $zipPath -Force
    throw '没有可打包的文件。'
}

Write-Host "已打包 $included 个文件，排除 $excluded 个: $zipPath"

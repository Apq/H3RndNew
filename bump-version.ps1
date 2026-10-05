param(
    [string]$RcPath,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Text.Encoding.CodePages -ErrorAction SilentlyContinue
[System.Text.Encoding]::RegisterProvider([System.Text.CodePagesEncodingProvider]::Instance)
$RcEncoding = [System.Text.Encoding]::GetEncoding(936)

function Resolve-RcPath {
    param([string]$Path)
    if ($Path) {
        $resolved = Join-Path $PSScriptRoot $Path
        if (Test-Path $resolved) { return (Resolve-Path $resolved).Path }
        if (Test-Path $Path) { return (Resolve-Path $Path).Path }
        throw "RC file not found: $Path"
    }

    $files = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.rc' -File
    if ($files.Count -ne 1) {
        throw "Expected exactly one .rc file in $PSScriptRoot, found $($files.Count). Use -RcPath."
    }
    return $files[0].FullName
}

function Get-VersionParts {
    param([string]$Text)
    $m = [regex]::Match($Text, '(?m)^\s*FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$')
    if (-not $m.Success) { throw 'FILEVERSION not found.' }
    [pscustomobject]@{
        Major = [int]$m.Groups[1].Value
        Minor = [int]$m.Groups[2].Value
        Year  = [int]$m.Groups[3].Value
        Date  = [int]$m.Groups[4].Value
    }
}

function Set-VersionText {
    param(
        [string]$Text,
        [int]$Major,
        [int]$Minor
    )

    $today = Get-Date
    $year = $today.Year
    $datePart = $today.Month * 100 + $today.Day
    $csv = "$Major,$Minor,$year,$datePart"
    $dot = "$Major.$Minor.$year.$datePart"

    $result = $Text
    $result = [regex]::Replace($result, '(?m)^(\s*FILEVERSION\s+)\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+(\s*)$', { param($m) $m.Groups[1].Value + $csv + $m.Groups[2].Value })
    $result = [regex]::Replace($result, '(?m)^(\s*PRODUCTVERSION\s+)\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+(\s*)$', { param($m) $m.Groups[1].Value + $csv + $m.Groups[2].Value })
    $result = [regex]::Replace($result, '(?m)^(\s*VALUE\s+"FileVersion"\s*,\s*")[^"]*("\s*)$', { param($m) $m.Groups[1].Value + $dot + $m.Groups[2].Value })
    $result = [regex]::Replace($result, '(?m)^(\s*VALUE\s+"ProductVersion"\s*,\s*")[^"]*("\s*)$', { param($m) $m.Groups[1].Value + $dot + $m.Groups[2].Value })

    [pscustomobject]@{
        Text = $result
        Csv  = $csv
        Dot  = $dot
    }
}

$rcPathResolved = Resolve-RcPath $RcPath
$text = $RcEncoding.GetString([System.IO.File]::ReadAllBytes($rcPathResolved))

$m = [regex]::Match($text, '(?m)^\s*FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$')
if (-not $m.Success) { throw 'FILEVERSION not found.' }
$currentMajor = [int]$m.Groups[1].Value
$currentMinor = [int]$m.Groups[2].Value
$newMinor = $currentMinor + 1

$updated = Set-VersionText -Text $text -Major $currentMajor -Minor $newMinor

Write-Host "RC file: $rcPathResolved"
Write-Host "Current version: $currentMajor.$currentMinor.$($m.Groups[3].Value).$($m.Groups[4].Value)"
Write-Host "New version: $($updated.Dot)"

if ($DryRun) {
    Write-Host 'Dry run: no files changed.'
    exit 0
}

[System.IO.File]::WriteAllBytes($rcPathResolved, $RcEncoding.GetBytes($updated.Text))
Write-Host 'Version bumped.'

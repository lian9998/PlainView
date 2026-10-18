<#
.SYNOPSIS
    Repeated cold-open benchmark of PlainView's decode and first-frame path.

.DESCRIPTION
    Stages the executable and the input images on the local Windows disk, then runs
    `PlainView.exe --bench` a number of times per file and reports min / median / max.

    Staging matters: images read through the WSL 9P mount (\\wsl.localhost\...) are
    several times slower to decode than the same bytes on NTFS, which dominates the
    measurement.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\bench.ps1 -Runs 5 `
        -ImageList "C:\Users\me\source\repos\big-image.jpg;tests\out\demo.jpg"

    Separate several images with semicolons; -File cannot pass a PowerShell array.
#>
[CmdletBinding()]
param(
    [int]$Runs = 5,
    [string]$Exe = "build\PlainView.exe",
    [string]$ImageList = "",
    [string]$Scratch
)

$ErrorActionPreference = "Stop"
$Images = @($ImageList -split "\s*;\s*" | Where-Object { $_ })
if ($Images.Count -eq 0) {
    throw "Pass at least one image: -ImageList path1.jpg;path2.png"
}
if (-not $Scratch) {
    $Scratch = Join-Path $env:TEMP "PlainView-bench"
}
New-Item -ItemType Directory -Force $Scratch | Out-Null

function Get-Metric([string]$text, [string]$pattern, [double]$scale = 1.0) {
    $match = [regex]::Match($text, $pattern)
    if ($match.Success) { return [double]$match.Groups[1].Value * $scale }
    return $null
}

function Get-Stats([double[]]$values) {
    $sorted = @($values | Sort-Object)
    $count = $sorted.Count
    if ($count % 2 -eq 1) {
        $median = $sorted[[int][Math]::Floor($count / 2)]
    } else {
        $median = ($sorted[$count / 2 - 1] + $sorted[$count / 2]) / 2.0
    }
    [pscustomobject]@{ Min = $sorted[0]; Median = $median; Max = $sorted[-1] }
}

function Format-Row([string]$name, [double[]]$values) {
    if ($values.Count -eq 0) { return }
    $stats = Get-Stats $values
    "{0,-16} min {1,8:F1}   median {2,8:F1}   max {3,8:F1} ms" -f $name, $stats.Min, $stats.Median, $stats.Max
}

$localExe = Join-Path $Scratch "PlainView.exe"
Copy-Item (Resolve-Path $Exe) $localExe -Force

foreach ($image in $Images) {
    $localImage = Join-Path $Scratch ([System.IO.Path]::GetFileName($image))
    Copy-Item (Resolve-Path $image) $localImage -Force

    $report = Join-Path $Scratch "report.txt"
    $decode = @()
    $first = @()
    $upload = @()
    $peak = @()
    $size = ""

    for ($run = 1; $run -le $Runs; $run++) {
        Remove-Item $report -ErrorAction SilentlyContinue
        & $localExe --bench $report $localImage | Out-Null
        if (-not (Test-Path $report)) {
            throw "no report written for $localImage (run $run)"
        }
        $text = Get-Content -Raw $report
        if (-not $size) {
            $sizeMatch = [regex]::Match($text, "image size\s*:\s*(\d+ x \d+)")
            if ($sizeMatch.Success) { $size = $sizeMatch.Groups[1].Value }
        }
        $value = Get-Metric $text "pixel decode ([\d.]+) ms"
        if ($null -ne $value) { $decode += $value }
        $value = Get-Metric $text "first frame\s*:\s*([\d.]+) ms"
        if ($null -ne $value) { $first += $value }
        $value = Get-Metric $text "texture upload\s*:\s*([\d.]+) ms"
        if ($null -ne $value) { $upload += $value }
        $value = Get-Metric $text "peak working set\s*:\s*([\d.]+) MB"
        if ($null -ne $value) { $peak += $value }
        Start-Sleep -Milliseconds 250
    }

    Write-Host ""
    Write-Host ("{0}  ({1}, {2} runs)" -f [System.IO.Path]::GetFileName($image), $size, $Runs)
    Format-Row "first frame" $first
    Format-Row "pixel decode" $decode
    Format-Row "texture upload" $upload
    if ($peak.Count -gt 0) {
        $peakStats = Get-Stats $peak
        "peak working set min {0,6:F0}   median {1,6:F0}   max {2,6:F0} MB" -f $peakStats.Min, $peakStats.Median, $peakStats.Max
    }
}

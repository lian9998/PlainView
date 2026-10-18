<#
.SYNOPSIS
    Build a portable PlainView release archive.

.DESCRIPTION
    Compiles src\version.h's version into the VERSIONINFO resource (via build.bat),
    stages PlainView.exe with the license and README, and writes:

        dist\PlainView-<version>-win64.zip
        dist\PlainView-<version>-win64.zip.sha256

    The executable is linked with the static C runtime (/MT), so the archive needs no
    Visual Studio redistributable and no installation.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release.ps1
    powershell -ExecutionPolicy Bypass -File tools\release.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot is empty when this script is fed through -EncodedCommand, so fall
# back to the current directory (tools\msvc.sh -C maps the repository root there).
$repoRoot = if ($PSScriptRoot) { Split-Path -Parent $PSScriptRoot } else { (Get-Location).Path }
Push-Location $repoRoot
try {
    if (-not (Test-Path "src\version.h")) {
        throw "Run this from the repository root (missing src\version.h in $repoRoot)."
    }
    $versionHeader = Get-Content -Raw "src\version.h"
    if ($versionHeader -notmatch 'PLAINVIEW_VERSION_STRING\s+"([^"]+)"') {
        throw "Could not read PLAINVIEW_VERSION_STRING from src\version.h."
    }
    $version = $Matches[1]

    if (-not $SkipBuild) {
        Write-Host "Building PlainView $version ..."
        & cmd.exe /c "build.bat"
        if ($LASTEXITCODE -ne 0) {
            throw "build.bat failed with exit code $LASTEXITCODE."
        }
    }

    $exe = Join-Path $repoRoot "build\PlainView.exe"
    if (-not (Test-Path $exe)) {
        throw "Missing $exe. Build first, or drop -SkipBuild."
    }

    # The archive name and the file properties must agree with the source of truth.
    $fileVersion = (Get-Item $exe).VersionInfo.FileVersion
    if ($fileVersion -ne $version) {
        throw "build\PlainView.exe is version '$fileVersion' but src\version.h says '$version'; rebuild."
    }

    $name = "PlainView-$version-win64"
    $stage = Join-Path $repoRoot "dist\$name"
    $zip = Join-Path $repoRoot "dist\$name.zip"

    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item $exe $stage
    foreach ($doc in @("LICENSE", "README.md")) {
        if (Test-Path $doc) { Copy-Item $doc $stage }
    }

    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name.zip" | Set-Content -Path "$zip.sha256" -Encoding Ascii

    Write-Host ""
    Write-Host "Release archive: dist\$name.zip"
    Write-Host "SHA-256:         $hash"
} finally {
    Pop-Location
}

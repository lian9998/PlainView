<#
.SYNOPSIS
    Sign PlainView.exe with a throwaway self-signed code-signing certificate.

.DESCRIPTION
    Creates a self-signed code-signing certificate in the current user's personal
    store, signs the executable (SHA-256), checks the result, and removes the
    certificate again. It is never added to Trusted Root Certification Authorities
    or Trusted Publishers, so Windows still reports an unknown publisher and
    SmartScreen still warns. This is for testing the signed-but-untrusted path,
    not for distribution.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sign.ps1
    powershell -ExecutionPolicy Bypass -File tools\sign.ps1 -Path dist\PlainView-1.1.0-win64\PlainView.exe
#>
[CmdletBinding()]
param(
    [string]$Path = "build\PlainView.exe",
    [string]$Subject = "CN=PlainView Self-Signed, O=PlainView, C=CN"
)

$ErrorActionPreference = "Stop"

$target = (Resolve-Path $Path).Path
if (-not (Test-Path $target)) { throw "No such file: $Path" }

$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { throw "signtool.exe not found in the Windows SDK" }

Write-Host "Creating a self-signed code-signing certificate (not trusted) ..."
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation Cert:\CurrentUser\My `
    -NotAfter (Get-Date).AddYears(5)

try {
    Write-Host "Signing $target ..."
    & $signtool sign /fd SHA256 /sha1 $certificate.Thumbprint /v $target
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed with exit code $LASTEXITCODE" }

    $signature = Get-AuthenticodeSignature $target
    Write-Host ""
    Write-Host "Status : $($signature.Status)"
    Write-Host "Signer : $($signature.SignerCertificate.Subject)"
    Write-Host "SHA1   : $($signature.SignerCertificate.Thumbprint)"

    Write-Host ""
    Write-Host "Verifying the chain (failure is expected: the root is not trusted) ..."
    & $signtool verify /pa /v $target
    $verifyExit = $LASTEXITCODE
    Write-Host "signtool verify exit code: $verifyExit (expect non-zero)"
}
finally {
    # Leave nothing behind: the certificate is removed from the personal store.
    Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -Force
    Write-Host "Removed the signing certificate from Cert:\CurrentUser\My"
}

# Creates tests/data/quad.jpg: four colored quadrants on a non-square canvas.
# TL red, TR green, BL blue, BR yellow.  Used by the EXIF orientation checks.
# Run from the repository root: paths are relative to the current directory.
Add-Type -AssemblyName System.Drawing

$outDir = 'tests\data'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$width = 256
$height = 192
$bmp = New-Object System.Drawing.Bitmap $width, $height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Red)
$g.FillRectangle([System.Drawing.Brushes]::Lime, [int]($width / 2), 0, [int]($width / 2), [int]($height / 2))
$g.FillRectangle([System.Drawing.Brushes]::Blue, 0, [int]($height / 2), [int]($width / 2), [int]($height / 2))
$g.FillRectangle([System.Drawing.Brushes]::Yellow, [int]($width / 2), [int]($height / 2), [int]($width / 2), [int]($height / 2))

# A small white marker in the top-left quadrant makes flips visible to the eye too.
$g.FillEllipse([System.Drawing.Brushes]::White, 16, 12, 28, 28)
$g.Dispose()

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$encoderParams = New-Object System.Drawing.Imaging.EncoderParameters 1
$encoderParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]95)
$path = Join-Path $outDir 'quad.jpg'
$bmp.Save($path, $codec, $encoderParams)
$bmp.Dispose()
Write-Output ("wrote " + $path)

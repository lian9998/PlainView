# Prints the color class at the four corners of every PNG in tests/out:
#   <name>: TL=red TR=green BL=blue BR=yellow
# Classes are matched loosely so JPEG ringing and scaling do not matter.
Add-Type -AssemblyName System.Drawing

function Classify([System.Drawing.Color]$c) {
    $r = [int]$c.R; $g = [int]$c.G; $b = [int]$c.B
    if ($r -gt 140 -and $g -gt 140 -and $b -lt 110) { return 'yellow' }
    if ($r -gt 140 -and $g -lt 110 -and $b -lt 110) { return 'red' }
    if ($r -lt 110 -and $g -gt 120 -and $b -lt 110) { return 'green' }
    if ($r -lt 110 -and $g -lt 120 -and $b -gt 140) { return 'blue' }
    return ('other({0},{1},{2})' -f $r, $g, $b)
}

$files = Get-ChildItem -Path 'tests\out' -Filter '*.png' | Sort-Object Name
foreach ($file in $files) {
    $image = [System.Drawing.Image]::FromFile($file.FullName)
    $bitmap = New-Object System.Drawing.Bitmap $image
    $w = $bitmap.Width
    $h = $bitmap.Height
    $inset = 120
    $tl = Classify $bitmap.GetPixel($inset, $inset)
    $tr = Classify $bitmap.GetPixel(($w - $inset), $inset)
    $bl = Classify $bitmap.GetPixel($inset, ($h - $inset))
    $br = Classify $bitmap.GetPixel(($w - $inset), ($h - $inset))
    $bitmap.Dispose()
    $image.Dispose()
    '{0}: TL={1} TR={2} BL={3} BR={4}' -f $file.Name, $tl, $tr, $bl, $br
}

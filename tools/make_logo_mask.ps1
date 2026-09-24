# Generates res\logo_mask.bin from res\logo.png.
#
# The tray icon needs a monochrome glyph that can be tinted to match the battery
# ring, so the logo is reduced to an 8 bit alpha mask. When the source image has
# no transparency the mask is derived from colour (the glyph is darker/more
# saturated than the background).
#
# File format: "RMK1" magic, uint16 width, uint16 height, width*height alpha bytes.

param(
    [string]$Source = (Join-Path $PSScriptRoot '..\res\logo.png'),
    [string]$Target = (Join-Path $PSScriptRoot '..\res\logo_mask.bin')
)

Add-Type -AssemblyName System.Drawing

$bitmap = New-Object System.Drawing.Bitmap((Resolve-Path -LiteralPath $Source).Path)
$width = $bitmap.Width
$height = $bitmap.Height

$mask = New-Object 'byte[]' ($width * $height)
$hasAlpha = $false

for ($y = 0; $y -lt $height; $y++) {
    for ($x = 0; $x -lt $width; $x++) {
        $pixel = $bitmap.GetPixel($x, $y)
        if (-not $hasAlpha -and $pixel.A -lt 250) { $hasAlpha = $true }
        $index = $y * $width + $x
        if ($pixel.A -lt 250) {
            $mask[$index] = $pixel.A
        } else {
            $minChannel = [Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
            $mask[$index] = [byte](255 - $minChannel)
        }
    }
}
$bitmap.Dispose()

if (-not $hasAlpha) {
    Write-Host 'source has no alpha channel, using colour derived mask'
}

# Trim to the glyph and keep a square, slightly padded canvas so the tray icon
# keeps the original proportions.
$minX = $width; $minY = $height; $maxX = -1; $maxY = -1
for ($y = 0; $y -lt $height; $y++) {
    for ($x = 0; $x -lt $width; $x++) {
        if ($mask[$y * $width + $x] -ge 8) {
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
if ($maxX -lt 0) { throw 'logo appears to be empty' }

$glyphWidth = $maxX - $minX + 1
$glyphHeight = $maxY - $minY + 1
$side = [Math]::Max($glyphWidth, $glyphHeight)
$padding = [Math]::Max(1, [int]($side * 0.03))
$side += $padding * 2

$centerX = [int](($minX + $maxX) / 2)
$centerY = [int](($minY + $maxY) / 2)
$cropX = $centerX - [int]($side / 2)
$cropY = $centerY - [int]($side / 2)

$result = New-Object 'byte[]' ($side * $side)
for ($y = 0; $y -lt $side; $y++) {
    for ($x = 0; $x -lt $side; $x++) {
        $sx = $cropX + $x
        $sy = $cropY + $y
        if ($sx -ge 0 -and $sx -lt $width -and $sy -ge 0 -and $sy -lt $height) {
            $result[$y * $side + $x] = $mask[$sy * $width + $sx]
        }
    }
}

# The source artwork is a line drawing. At 16 px taskbar size those strokes
# collapse into a faint speck, so the enclosed areas are filled in and the mark
# becomes the solid three headed snake silhouette.
$ink = New-Object 'bool[]' ($side * $side)
for ($i = 0; $i -lt $result.Length; $i++) { $ink[$i] = $result[$i] -ge 8 }

$outside = New-Object 'bool[]' ($side * $side)
$queue = New-Object 'System.Collections.Generic.Queue[int]'
function Add-BackgroundPixel([int]$index) {
    if (-not $ink[$index] -and -not $outside[$index]) {
        $outside[$index] = $true
        $queue.Enqueue($index)
    }
}
for ($x = 0; $x -lt $side; $x++) {
    Add-BackgroundPixel $x
    Add-BackgroundPixel (($side - 1) * $side + $x)
}
for ($y = 0; $y -lt $side; $y++) {
    Add-BackgroundPixel ($y * $side)
    Add-BackgroundPixel ($y * $side + $side - 1)
}
while ($queue.Count -gt 0) {
    $index = $queue.Dequeue()
    $x = $index % $side
    $y = [int](($index - $x) / $side)
    if ($x -gt 0) { Add-BackgroundPixel ($index - 1) }
    if ($x -lt $side - 1) { Add-BackgroundPixel ($index + 1) }
    if ($y -gt 0) { Add-BackgroundPixel ($index - $side) }
    if ($y -lt $side - 1) { Add-BackgroundPixel ($index + $side) }
}
for ($i = 0; $i -lt $result.Length; $i++) {
    if (-not $ink[$i] -and -not $outside[$i]) { $result[$i] = 255 }
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([byte[]][char[]]'RMK1')
$writer.Write([uint16]$side)
$writer.Write([uint16]$side)
$writer.Write($result)
$writer.Flush()
[IO.File]::WriteAllBytes($Target, $stream.ToArray())
$writer.Close()

Write-Host "mask written: $Target (${side}x${side}, $($stream.Length) bytes)"

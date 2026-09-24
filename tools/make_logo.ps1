# Draws the project's own logo artwork from scratch and writes every asset the
# build needs. The mark is an original minimalist gaming mouse (body, scroll
# wheel slot, one side button) drawn with plain vector primitives - it does not
# reproduce any vendor logo.
#
# Outputs:
#   res\logo.png       256x256 mark, used by the README header
#   res\logo_mask.bin  8 bit alpha mask embedded as RCDATA for the tray icon
#   res\app.ico        multi size icon (16..256) for the exe / installer
#
# Mask file format: "RMK1" magic, uint16 width, uint16 height, width*height alpha bytes.

param(
    [string]$LogoPath = (Join-Path $PSScriptRoot '..\res\logo.png'),
    [string]$MaskPath = (Join-Path $PSScriptRoot '..\res\logo_mask.bin'),
    [string]$IconPath = (Join-Path $PSScriptRoot '..\res\app.ico')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$DesignSize = 256.0
$Accent     = [System.Drawing.Color]::FromArgb(255, 68, 214, 44)   # #44D62C neon green

# Geometry in design units (256 x 256). The body is a rounded capsule, the
# wheel is punched out of the upper middle and one side button is punched out
# of the left edge, which is what makes it read as a gaming mouse.
$BodyGeometry       = @{ X = 69.0;  Y = 22.0;  W = 118.0; H = 212.0; R = 56.0 }
$WheelGeometry      = @{ X = 117.0; Y = 52.0;  W = 22.0;  H = 52.0;  R = 11.0 }
$ButtonSplitGeometry = @{ X = 69.0; Y = 106.0; W = 118.0; H = 7.0;   R = 3.5 }
$SideButtonGeometry = @{ X = 56.0;  Y = 116.0; W = 26.0;  H = 46.0;  R = 13.0 }

function New-RoundedPath {
    param([hashtable]$Shape)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $diameter = $Shape.R * 2.0
    $x = $Shape.X; $y = $Shape.Y; $w = $Shape.W; $h = $Shape.H
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc($x + $w - $diameter, $y, $diameter, $diameter, 270, 90)
    $path.AddArc($x + $w - $diameter, $y + $h - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($x, $y + $h - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-LogoBitmap {
    param([int]$PixelSize, [double]$Scale = 1.0)

    $bitmap = New-Object System.Drawing.Bitmap($PixelSize, $PixelSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    $factor = ($PixelSize / $DesignSize) * $Scale
    $offset = ($PixelSize - $DesignSize * $factor) / 2.0
    $graphics.TranslateTransform([single]$offset, [single]$offset)
    $graphics.ScaleTransform([single]$factor, [single]$factor)

    $bodyPath = New-RoundedPath $BodyGeometry
    $brush = New-Object System.Drawing.SolidBrush($Accent)
    $graphics.FillPath($brush, $bodyPath)

    # Punch the cut-outs: SourceCopy writes the fully transparent brush verbatim.
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $clearBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
    $wheelPath = New-RoundedPath $WheelGeometry
    $splitPath = New-RoundedPath $ButtonSplitGeometry
    $buttonPath = New-RoundedPath $SideButtonGeometry
    $graphics.FillPath($clearBrush, $wheelPath)
    $graphics.FillPath($clearBrush, $splitPath)
    $graphics.FillPath($clearBrush, $buttonPath)

    $clearBrush.Dispose(); $brush.Dispose(); $bodyPath.Dispose()
    $wheelPath.Dispose(); $splitPath.Dispose(); $buttonPath.Dispose(); $graphics.Dispose()
    return $bitmap
}

function Get-BitmapPixels {
    param([System.Drawing.Bitmap]$Bitmap)

    $rect = New-Object System.Drawing.Rectangle(0, 0, $Bitmap.Width, $Bitmap.Height)
    $data = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $length = [Math]::Abs($data.Stride) * $Bitmap.Height
        $buffer = New-Object 'byte[]' $length
        [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buffer, 0, $length)
    } finally {
        $Bitmap.UnlockBits($data)
    }
    return @{ Pixels = $buffer; Stride = $data.Stride }
}

# ---------------------------------------------------------------- logo.png
$logo = New-LogoBitmap 256 0.96
$logo.Save($LogoPath, [System.Drawing.Imaging.ImageFormat]::Png)
$logo.Dispose()
Write-Host "logo written: $LogoPath"

# ------------------------------------------------------------ logo_mask.bin
$mask = New-LogoBitmap 256 1.0
$capture = Get-BitmapPixels $mask
$pixels = $capture.Pixels
$stride = $capture.Stride

$alpha = New-Object 'byte[]' (256 * 256)
$minX = 256; $minY = 256; $maxX = -1; $maxY = -1
for ($y = 0; $y -lt 256; $y++) {
    for ($x = 0; $x -lt 256; $x++) {
        $value = $pixels[$y * $stride + $x * 4 + 3]
        $alpha[$y * 256 + $x] = $value
        if ($value -ge 8) {
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
$mask.Dispose()
if ($maxX -lt 0) { throw 'the mark rendered empty' }

# Trim to the glyph and pad slightly so the tray icon keeps its proportions.
$padding = 2
$cropX = [Math]::Max(0, $minX - $padding)
$cropY = [Math]::Max(0, $minY - $padding)
$cropW = [Math]::Min(256 - $cropX, ($maxX - $minX + 1) + $padding * 2)
$cropH = [Math]::Min(256 - $cropY, ($maxY - $minY + 1) + $padding * 2)

# The tray drawing code maps the mask onto a square box, so pad the trimmed
# glyph to a square here - that preserves its aspect ratio on screen.
$side = [Math]::Max($cropW, $cropH)
$offsetX = [int](($side - $cropW) / 2)
$offsetY = [int](($side - $cropH) / 2)
$trimmed = New-Object 'byte[]' ($side * $side)
for ($y = 0; $y -lt $cropH; $y++) {
    for ($x = 0; $x -lt $cropW; $x++) {
        $trimmed[($y + $offsetY) * $side + ($x + $offsetX)] = $alpha[($cropY + $y) * 256 + ($cropX + $x)]
    }
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([byte[]][char[]]'RMK1')
$writer.Write([uint16]$side)
$writer.Write([uint16]$side)
$writer.Write($trimmed)
$writer.Flush()
[IO.File]::WriteAllBytes($MaskPath, $stream.ToArray())
$writer.Close()
Write-Host "mask written: $MaskPath (${side}x${side}, glyph ${cropW}x${cropH})"

# ---------------------------------------------------------------- app.ico
function ConvertTo-IcoFrame {
    param([System.Drawing.Bitmap]$Bitmap)

    $size = $Bitmap.Width
    $data = $Bitmap.LockBits((New-Object System.Drawing.Rectangle(0, 0, $size, $size)),
                             [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                             [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $stride = $data.Stride
        $buffer = New-Object 'byte[]' ($stride * $size)
        [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buffer, 0, $buffer.Length)
    } finally {
        $Bitmap.UnlockBits($data)
    }

    $xorSize = $size * $size * 4
    $maskPitch = [int](([Math]::Floor($size / 32.0) + 1) * 4)
    $maskSize = $maskPitch * $size
    $frame = New-Object 'byte[]' (40 + $xorSize + $maskSize)
    $stream = [System.IO.MemoryStream]::new([byte[]]$frame, $true)
    $writer = [System.IO.BinaryWriter]::new($stream)

    $writer.Write([uint32]40)                     # biSize
    $writer.Write([int32]$size)                   # biWidth
    $writer.Write([int32]($size * 2))             # biHeight (XOR + AND)
    $writer.Write([uint16]1)                      # biPlanes
    $writer.Write([uint16]32)                     # biBitCount
    $writer.Write([uint32]0)                      # biCompression
    $writer.Write([uint32]($xorSize + $maskSize)) # biSizeImage
    $writer.Write([int32]0); $writer.Write([int32]0)
    $writer.Write([uint32]0); $writer.Write([uint32]0)

    # XOR bitmap: rows are stored bottom-up, pixels stay BGRA
    $xor = New-Object 'byte[]' $xorSize
    for ($y = 0; $y -lt $size; $y++) {
        $sourceRow = ($size - 1 - $y) * $stride
        [Array]::Copy($buffer, $sourceRow, $xor, $y * $size * 4, $size * 4)
    }

    # AND mask: 1 bit set means fully transparent
    $and = New-Object 'byte[]' $maskSize
    for ($y = 0; $y -lt $size; $y++) {
        $sourceRow = ($size - 1 - $y) * $stride
        $targetRow = $y * $maskPitch
        for ($x = 0; $x -lt $size; $x++) {
            if ($buffer[$sourceRow + $x * 4 + 3] -lt 32) {
                $byteIndex = $targetRow + [int]($x / 8)
                $and[$byteIndex] = $and[$byteIndex] -bor (1 -shl (7 - ($x % 8)))
            }
        }
    }

    $writer.Write($xor, 0, $xor.Length)
    $writer.Write($and, 0, $and.Length)

    $writer.Flush()
    $writer.Dispose()
    $stream.Dispose()
    return ,$frame
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @()
foreach ($size in $sizes) {
    $bitmap = New-LogoBitmap $size 0.92
    $frames += ,(ConvertTo-IcoFrame $bitmap)
    $bitmap.Dispose()
}

$iconStream = [System.IO.MemoryStream]::new()
$iconWriter = [System.IO.BinaryWriter]::new($iconStream)
$iconWriter.Write([uint16]0)                  # reserved
$iconWriter.Write([uint16]1)                  # type: icon
$iconWriter.Write([uint16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $size = $sizes[$i]
    $dimension = if ($size -ge 256) { 0 } else { $size }
    $iconWriter.Write([byte]$dimension)       # width
    $iconWriter.Write([byte]$dimension)       # height
    $iconWriter.Write([byte]0)                # palette
    $iconWriter.Write([byte]0)                # reserved
    $iconWriter.Write([uint16]1)              # planes
    $iconWriter.Write([uint16]32)             # bit count
    $iconWriter.Write([uint32]$frames[$i].Length)
    $iconWriter.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($frame in $frames) { $iconWriter.Write($frame) }
$iconWriter.Flush()
[IO.File]::WriteAllBytes($IconPath, $iconStream.ToArray())
$iconWriter.Close()
Write-Host "icon written: $IconPath ($($sizes.Count) sizes: $($sizes -join ', '))"

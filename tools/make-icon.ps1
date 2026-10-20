# The mascot and the app icon from one picture: tools\make-icon.ps1 <picture.png>
# Crops the picture to its figure (a square around what is not transparent, with a little air), then writes
# assets\mascot.png (256 px, drawn over the chat's input) and assets\aicontrols.ico (16-256 px, PNG frames).
param([Parameter(Mandatory)][string]$Source)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot '..\assets'

$picture = New-Object Drawing.Bitmap $Source
try {
    $minX = $picture.Width; $minY = $picture.Height; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $picture.Height; $y += 2) {
        for ($x = 0; $x -lt $picture.Width; $x += 2) {
            if ($picture.GetPixel($x, $y).A -gt 24) {
                if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) { throw 'The picture is all transparent' }
    $side = [int]([Math]::Max($maxX - $minX, $maxY - $minY) * 1.04) + 4
    $cx = ($minX + $maxX) / 2; $cy = ($minY + $maxY) / 2
    $crop = New-Object Drawing.RectangleF ([float]($cx - $side / 2)), ([float]($cy - $side / 2)), $side, $side

    function Render([int]$size) {
        $bitmap = New-Object Drawing.Bitmap $size, $size, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [Drawing.Graphics]::FromImage($bitmap)
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.DrawImage($picture, (New-Object Drawing.RectangleF 0, 0, $size, $size), $crop, [Drawing.GraphicsUnit]::Pixel)
        $g.Dispose()
        $stream = New-Object IO.MemoryStream
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
        return , $stream.ToArray()
    }

    [IO.File]::WriteAllBytes((Join-Path $assets 'mascot.png'), (Render 256))

    # An .ico of PNG frames (Windows Vista and later read them at every size).
    $sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
    $frames = foreach ($size in $sizes) { , (Render $size) }
    $out = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter $out
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256))  # 256 is written as 0
        $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]$frames[$i].Length); $w.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $w.Write($frame) }
    $w.Flush()
    [IO.File]::WriteAllBytes((Join-Path $assets 'aicontrols.ico'), $out.ToArray())
    "crop: $([int]$crop.X),$([int]$crop.Y) side $side"
} finally {
    $picture.Dispose()
}

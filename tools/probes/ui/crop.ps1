param([string]$Path, [int]$X, [int]$Y, [int]$W, [int]$H, [int]$Scale = 3, [string]$Out)
# A piece of a picture, made bigger (OCR misses small grey words), and how many of its pixels are not the background.
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile($Path)
$dst = New-Object System.Drawing.Bitmap ($W * $Scale), ($H * $Scale)
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, ($W * $Scale), ($H * $Scale)), (New-Object System.Drawing.Rectangle $X, $Y, $W, $H), [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()
$bg = $src.GetPixel($X, $Y)
$ink = 0
for ($i = $X; $i -lt $X + $W; $i++) { for ($j = $Y; $j -lt $Y + $H; $j++) {
    $p = $src.GetPixel($i, $j)
    if ([Math]::Abs($p.R - $bg.R) + [Math]::Abs($p.G - $bg.G) + [Math]::Abs($p.B - $bg.B) -gt 60) { $ink++ }
} }
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$src.Dispose(); $dst.Dispose()
"background {0},{1},{2}; ink pixels {3}" -f $bg.R, $bg.G, $bg.B, $ink

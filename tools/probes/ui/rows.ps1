param([string]$Path, [int]$X, [int]$Y, [int]$W, [int]$H)
# Which pixel rows of a region of a screenshot carry ink (anything unlike the region's top-left pixel): runs "from-to"
# in the region's own rows, to see where lines of text stand and whether the first or the last is cut.
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $Path
$back = $bmp.GetPixel($X, $Y)
$runs = @(); $start = -1
for ($row = 0; $row -lt $H; $row++) {
    $ink = 0
    for ($col = 0; $col -lt $W; $col++) {
        $c = $bmp.GetPixel($X + $col, $Y + $row)
        if ([math]::Abs($c.R - $back.R) + [math]::Abs($c.G - $back.G) + [math]::Abs($c.B - $back.B) -gt 60) { $ink++ }
    }
    if ($ink -gt 0 -and $start -lt 0) { $start = $row }
    if ($ink -eq 0 -and $start -ge 0) { $runs += "$start-$($row - 1)"; $start = -1 }
}
if ($start -ge 0) { $runs += "$start-$($H - 1)" }
$bmp.Dispose()
"background $($back.R),$($back.G),$($back.B); rows with ink: " + ($runs -join ' ')

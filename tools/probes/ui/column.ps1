param([string]$Path, [int]$X, [int]$Top, [int]$Bottom)
# The runs of a pixel column that stand out from black-ish background: where a scrollbar's thumb is.
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile($Path)
$runStart = -1
for ($y = $Top; $y -le $Bottom; $y++) {
    $p = $src.GetPixel($X, $y)
    $lit = ($p.R + $p.G + $p.B) -gt 90
    if ($lit -and $runStart -lt 0) { $runStart = $y }
    if (-not $lit -and $runStart -ge 0) { "run {0}..{1} (rgb {2},{3},{4})" -f $runStart, ($y - 1), $q.R, $q.G, $q.B; $runStart = -1 }
    $q = $p
}
if ($runStart -ge 0) { "run {0}..{1}" -f $runStart, $Bottom }
$src.Dispose()

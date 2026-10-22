param([int]$ProcessId, [string[]]$Steps)
# Mouse messages posted straight to the window (the real pointer stays where it is): each step is
# "down|move|up|mdown|mup|click:x,y", "wheel:x,y,notches" (positive = up) or "wait:ms". Coordinates are the client's, in pixels.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Post {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public struct POINT { public int X, Y; }
}
"@
$h = (Get-Process -Id $ProcessId).MainWindowHandle
function Send([uint32]$message, [int]$keys, [int]$x, [int]$y) {
    $l = [IntPtr](($y -shl 16) -bor ($x -band 0xffff))
    [Post]::PostMessageW($h, $message, [IntPtr]$keys, $l) | Out-Null
}
foreach ($step in $Steps) {
    $kind, $arg = $step -split ':', 2
    if ($kind -eq 'wait') { Start-Sleep -Milliseconds ([int]$arg); continue }
    $x, $y, $notches = $arg -split ',' | ForEach-Object { [int]$_ }
    switch ($kind) {
        'move'  { Send 0x0200 0 $x $y }
        'drag'  { Send 0x0200 1 $x $y }
        'down'  { Send 0x0200 0 $x $y; Send 0x0201 1 $x $y }
        'up'    { Send 0x0202 0 $x $y }
        'click' { Send 0x0200 0 $x $y; Send 0x0201 1 $x $y; Send 0x0202 0 $x $y }
        'rclick' { Send 0x0204 2 $x $y; Send 0x0205 0 $x $y }
        'wheel' { $pt = New-Object Post+POINT; $pt.X = $x; $pt.Y = $y; [Post]::ClientToScreen($h, [ref]$pt) | Out-Null; $w = [IntPtr]((($notches * 120) -band 0xffff) -shl 16); [Post]::PostMessageW($h, 0x020A, $w, [IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xffff))) | Out-Null }
        'mdown' { Send 0x0207 0x10 $x $y }
        'mup'   { Send 0x0208 0 $x $y }
    }
    Start-Sleep -Milliseconds 120
}

param([int]$ProcessId, [string[]]$Steps)
# Mouse messages posted straight to the window (the real pointer stays where it is): each step is
# "down|move|up|mdown|mup|click:x,y" or "wait:ms". Coordinates are the client's, in pixels.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Post {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
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
    $x, $y = $arg -split ',' | ForEach-Object { [int]$_ }
    switch ($kind) {
        'move'  { Send 0x0200 0 $x $y }
        'drag'  { Send 0x0200 1 $x $y }
        'down'  { Send 0x0200 0 $x $y; Send 0x0201 1 $x $y }
        'up'    { Send 0x0202 0 $x $y }
        'click' { Send 0x0200 0 $x $y; Send 0x0201 1 $x $y; Send 0x0202 0 $x $y }
        'rclick' { Send 0x0204 2 $x $y; Send 0x0205 0 $x $y }
        'mdown' { Send 0x0207 0x10 $x $y }
        'mup'   { Send 0x0208 0 $x $y }
    }
    Start-Sleep -Milliseconds 120
}

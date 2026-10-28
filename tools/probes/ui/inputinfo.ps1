param([int]$ProcessId)
# What the input's RichEdit says about itself, as far as messages without pointers reach another process:
# its window size, the number of lines and the first visible one. (Messages that carry a pointer, EM_GETRECT and
# EM_GETSCROLLPOS among them, are not marshalled between processes: they crash the target.)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Info {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static IntPtr Find(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => {
            var name = new StringBuilder(64);
            GetClassNameW(h, name, 64);
            if (found == IntPtr.Zero && name.ToString().StartsWith("RICHEDIT") && IsWindowVisible(h)) found = h;
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
$main = (Get-Process -Id $ProcessId).MainWindowHandle
$e = [Info]::Find($main)
if ($e -eq [IntPtr]::Zero) { throw 'no input found' }
$client = New-Object Info+RECT; [Info]::GetClientRect($e, [ref]$client) | Out-Null
$inputRect = New-Object Info+RECT; [Info]::GetWindowRect($e, [ref]$inputRect) | Out-Null
$mainRect = New-Object Info+RECT; [Info]::GetWindowRect($main, [ref]$mainRect) | Out-Null
"in a screenshot (shot.ps1) the input is at x={0} y={1}" -f ($inputRect.Left - $mainRect.Left), ($inputRect.Top - $mainRect.Top)
$lines = [int][Info]::SendMessageW($e, 0xBA, [IntPtr]::Zero, [IntPtr]::Zero)
$first = [int][Info]::SendMessageW($e, 0xCE, [IntPtr]::Zero, [IntPtr]::Zero)
"client {0}x{1}, lines {2}, first visible {3}" -f $client.Right, $client.Bottom, $lines, $first

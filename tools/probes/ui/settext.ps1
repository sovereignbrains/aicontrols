param([int]$ProcessId, [int]$Lines = 30, [string]$Text = '')
# Fills the input of the window of the given process (WM_SETTEXT to its RichEdit): -Lines numbered lines, or -Text as it is.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Edit {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
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
$edit = [Edit]::Find($main)
if ($edit -eq [IntPtr]::Zero) { throw 'no input found' }
if ($Text -eq '') { $Text = (1..$Lines | ForEach-Object { "line $_ - some text to check how the input scrolls, pgy" }) -join "`r" }
[Edit]::SendMessageW($edit, 0x000C, [IntPtr]::Zero, $Text) | Out-Null

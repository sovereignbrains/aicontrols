param([int]$ProcessId, [string]$Message, [int]$W = 0, [int]$L = 0)
# Sends one message without pointers to the input's RichEdit (e.g. -Message 0xB1 -W 0 -L -1 selects all; 0xB7 scrolls to the caret;
# 0xB6 EM_LINESCROLL: -L lines; 0x20A is not for this: use mouse.ps1 wheel). Returns its result.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Poke {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
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
$e = [Poke]::Find((Get-Process -Id $ProcessId).MainWindowHandle)
if ($e -eq [IntPtr]::Zero) { throw 'no input found' }
[Poke]::SendMessageW($e, [Convert]::ToUInt32($Message, 16), [IntPtr]$W, [IntPtr]$L)

# Walk the configuration utility with MSAA and report what a screen reader
# would announce for each control.
#
# oleacc, not UIA: PowerShell's UI Automation client reports almost every
# Win32 control as a bare "Pane" with no name, which makes an accessible
# dialog and an inaccessible one look identical. MSAA is what NVDA and
# Narrator actually use for a classic dialog, and it tells the truth.

param(
    [string]$Exe,
    [int]$WaitSeconds = 10
)

$signature = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Msaa {
    // id is declared signed: OBJID_CLIENT is 0xFFFFFFFC, and PowerShell
    // parses that literal as the Int32 -4, which will not convert to a UInt32
    // parameter at all.
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(
        IntPtr hwnd, int id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object ppvObject);

    [DllImport("oleacc.dll")]
    public static extern int AccessibleChildren(
        [MarshalAs(UnmanagedType.IUnknown)] object paccContainer,
        int iChildStart, int cChildren,
        [Out, MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.Struct)] object[] rgvarChildren,
        out int pcObtained);

    // CharSet matters: without it the marshaller hands an ANSI string to a
    // wide-character entry point and the title never matches.
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindWindowW(string cls, string title);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr hwnd, EnumProc proc, IntPtr lParam);

    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int max);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder buf, int max);

    [DllImport("user32.dll")]
    public static extern int GetWindowLongW(IntPtr hwnd, int index);

    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr hwnd);
}
'@

Add-Type -TypeDefinition $signature -Language CSharp

$proc = $null
if ($Exe) {
    $proc = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Seconds 2
}

# The process's own main window, rather than FindWindow: PowerShell turns a
# $null class-name argument into an empty string, which FindWindowW rejects
# outright with ERROR_INVALID_NAME.
$hwnd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ($hwnd -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    $live = if ($proc) { Get-Process -Id $proc.Id -ErrorAction SilentlyContinue }
            else { Get-Process -Name NokiaKlattConfig -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if ($live) {
        $live.Refresh()
        if ($live.MainWindowHandle -ne [IntPtr]::Zero -and
            $live.MainWindowTitle -eq "Nokia Klatt Speech Settings") {
            $hwnd = $live.MainWindowHandle
            break
        }
    }
    Start-Sleep -Milliseconds 300
}

if ($hwnd -eq [IntPtr]::Zero) {
    Write-Output "FAIL: the settings window did not appear"
    if ($proc) { $proc.Kill() }
    exit 1
}

Write-Output "Found the dialog. Walking its controls with MSAA."
Write-Output ""

$WS_TABSTOP  = 0x00010000
$WS_VISIBLE  = 0x10000000
$WS_DISABLED = 0x08000000

$rows = New-Object System.Collections.ArrayList
$callback = [Msaa+EnumProc]{
    param($child, $lparam)

    $cls = New-Object System.Text.StringBuilder 128
    [void][Msaa]::GetClassNameW($child, $cls, 128)
    $text = New-Object System.Text.StringBuilder 512
    [void][Msaa]::GetWindowTextW($child, $text, 512)
    $style = [Msaa]::GetWindowLongW($child, -16)
    $id = [Msaa]::GetDlgCtrlID($child)

    # What MSAA reports: the accessible name and role of this control.
    $iid = [Guid]"618736e0-3c3d-11cf-810c-00aa00389b71"   # IAccessible
    $acc = $null
    $name = ""
    $role = ""
    if ([Msaa]::AccessibleObjectFromWindow($child, -4, [ref]$iid, [ref]$acc) -eq 0 -and $acc) {
        try {
            $type = $acc.GetType()
            $name = $type.InvokeMember("accName", "GetProperty", $null, $acc, @([int]0))
            $roleId = $type.InvokeMember("accRole", "GetProperty", $null, $acc, @([int]0))
            # oleacc.h role constants. Getting these wrong makes the report
            # lie about what a screen reader would call each control.
            if ($roleId -is [array]) { $roleId = $roleId[0] }
            if ($name -is [array]) { $name = $name[0] }
            $role = switch ([int]$roleId) {
                9  { "window" }
                10 { "client" }
                20 { "grouping" }
                33 { "list" }
                41 { "static text" }
                42 { "text" }
                43 { "push button" }
                44 { "check box" }
                45 { "radio button" }
                46 { "combo box" }
                51 { "slider" }
                default { "role $roleId" }
            }
        } catch { $name = "<MSAA query failed>" }
    }

    [void]$rows.Add([pscustomobject]@{
        Id      = $id
        Class   = $cls.ToString()
        Text    = $text.ToString()
        Name    = $name
        Role    = $role
        Tabstop = [bool]($style -band $WS_TABSTOP)
        Visible = [bool]($style -band $WS_VISIBLE)
        Enabled = -not [bool]($style -band $WS_DISABLED)
    })
    return $true
}
[void][Msaa]::EnumChildWindows($hwnd, $callback, [IntPtr]::Zero)

$problems = 0
Write-Output ("{0,-6} {1,-14} {2,-12} {3,-4} {4}" -f "Id", "Class", "Role", "Tab", "Accessible name / label text")
Write-Output ("-" * 110)
foreach ($r in $rows) {
    $flag = ""
    $interactive = $r.Class -match "ComboBox|Edit|Button" -and $r.Class -notmatch "Static"
    $isGroupOrLabel = $r.Class -match "Static" -or ($r.Class -eq "Button" -and -not $r.Tabstop)

    if ($interactive -and -not $isGroupOrLabel) {
        if ([string]::IsNullOrWhiteSpace($r.Name)) {
            $flag = "  <-- NO ACCESSIBLE NAME"
            $problems++
        }
        if (-not $r.Tabstop) {
            $flag += "  <-- NOT IN TAB ORDER"
            $problems++
        }
    }
    $shown = if ([string]::IsNullOrWhiteSpace($r.Name)) { $r.Text } else { $r.Name }
    Write-Output ("{0,-6} {1,-14} {2,-12} {3,-4} {4}{5}" -f `
        $r.Id, $r.Class, $r.Role, $(if ($r.Tabstop) { "yes" } else { "-" }), $shown, $flag)
}

Write-Output ""
$tabbable = ($rows | Where-Object { $_.Tabstop }).Count
Write-Output "$($rows.Count) control(s), $tabbable in the tab order."
if ($problems -eq 0) {
    Write-Output "PASS: every interactive control has an accessible name and a tab stop."
} else {
    Write-Output "FAIL: $problems problem(s) found."
}

if ($proc -and -not $proc.HasExited) { $proc.Kill() }
exit $(if ($problems -eq 0) { 0 } else { 1 })

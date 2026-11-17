# ---------------------------------------------------------------------------
#  Проверка HidePanel "в бою": прячет ли панель, показывается ли мгновенно,
#  возвращается ли всё назад после выхода.
#  
#  .\scripts\test.ps1
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Exe,
    # Тест с отключённым fullscreen-guard (если на переднем плане fullscreen-видео)
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'dist\HidePanel.exe' }
if (-not (Test-Path $Exe)) { throw "not found: $Exe" }

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Nat {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, ref RECT r, uint c);
    [DllImport("shell32.dll")] public static extern uint SHAppBarMessage(uint msg, ref APPBARDATA abd);

    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct APPBARDATA {
        public uint cbSize; public IntPtr hWnd; public uint uCallbackMessage; public uint uEdge;
        public RECT rc; public IntPtr lParam;
    }
    public const uint ABM_GETSTATE = 4;
    public const uint ABM_SETSTATE = 10;
    public const uint ABS_AUTOHIDE = 1;
    public const uint ABS_ALWAYSONTOP = 2;
    public const uint SPI_GETWORKAREA = 0x30;
}
"@

# физические пиксели
[Nat]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

Add-Type -AssemblyName System.Drawing

$script:fail = 0
function Check([string]$name, [bool]$ok, [string]$extra = '') {
    if ($ok) { Write-Host ("  PASS  {0} {1}" -f $name, $extra) -ForegroundColor Green }
    else     { Write-Host ("  FAIL  {0} {1}" -f $name, $extra) -ForegroundColor Red; $script:fail++ }
}

function Get-Tray {
    $h = [Nat]::FindWindow('Shell_TrayWnd', $null)
    if ($h -eq [IntPtr]::Zero) { throw 'Shell_TrayWnd not found' }
    return $h
}

function Get-AutoHide {
    $abd = New-Object Nat+APPBARDATA
    $abd.cbSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type]([Nat+APPBARDATA]))
    $state = [Nat]::SHAppBarMessage([Nat]::ABM_GETSTATE, [ref]$abd)
    return (($state -band [Nat]::ABS_AUTOHIDE) -ne 0)
}

function Get-WorkArea {
    $r = New-Object Nat+RECT
    [Nat]::SystemParametersInfo([Nat]::SPI_GETWORKAREA, 0, [ref]$r, 0) | Out-Null
    return $r
}

function Set-AutoHide([bool]$on) {
    $abd = New-Object Nat+APPBARDATA
    $abd.cbSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type]([Nat+APPBARDATA]))
    $abd.hWnd = Get-Tray
    $abd.lParam = [IntPtr]$(if ($on) { 3 } else { 2 })   # ABS_AUTOHIDE|ABS_ALWAYSONTOP / ABS_ALWAYSONTOP
    [Nat]::SHAppBarMessage([Nat]::ABM_SETSTATE, [ref]$abd) | Out-Null
}

function Shot([string]$path, [int]$height = 320) {
    $w = [Nat]::GetSystemMetrics(0)
    $h = [Nat]::GetSystemMetrics(1)
    $bmp = New-Object System.Drawing.Bitmap($w, $height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(0, $h - $height, 0, 0, (New-Object System.Drawing.Size($w, $height)))
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function Wait-Visible([IntPtr]$tray, [bool]$want, [int]$timeoutMs = 2000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ([Nat]::IsWindowVisible($tray) -eq $want) { return $sw.ElapsedMilliseconds }
        Start-Sleep -Milliseconds 1
    }
    return -1
}

# Ставит курсор и проверяет, что он там остался (пользователь может двигать мышью)
function Set-Cursor([int]$x, [int]$y, [int]$tries = 4) {
    for ($i = 0; $i -lt $tries; $i++) {
        [Nat]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds 25
        $p = New-Object Nat+POINT
        [Nat]::GetCursorPos([ref]$p) | Out-Null
        if ($p.X -eq $x -and $p.Y -eq $y) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------------------

$tray = Get-Tray
$origAutoHide  = Get-AutoHide
$origWork      = Get-WorkArea
$origCursor    = New-Object Nat+POINT
[Nat]::GetCursorPos([ref]$origCursor) | Out-Null
$screenW = [Nat]::GetSystemMetrics(0)
$screenH = [Nat]::GetSystemMetrics(1)

$shots = Join-Path $root 'build\shots'
New-Item -ItemType Directory -Force -Path $shots | Out-Null

Write-Host ""
Write-Host "screen $screenW x $screenH, auto-hide before: $origAutoHide"
Write-Host "work area: $($origWork.Left),$($origWork.Top) - $($origWork.Right),$($origWork.Bottom)"
Write-Host ""

$proc = $null
try {
    # --- запуск -----------------------------------------------------------
    $appArgs = @('--debug')
    if ($Force) { $appArgs += '--no-fullscreen-guard' }
    $proc = Start-Process -FilePath $Exe -ArgumentList $appArgs -PassThru
    Start-Sleep -Milliseconds 800
    if ($proc.HasExited) { throw "HidePanel exited immediately (code $($proc.ExitCode))" }

    # --- 1. панель спрятана ----------------------------------------------
    Start-Sleep -Milliseconds 400
    $hiddenNow = -not [Nat]::IsWindowVisible($tray)
    Check 'taskbar hidden after start' $hiddenNow
    Check 'native auto-hide disabled while running' (-not (Get-AutoHide))

    # Рабочая область: нативное авто-скрытие резервирует весь экран, наша
    # программа — место под панель. Главное, чтобы она не "прыгала" в работе.
    $wa = Get-WorkArea
    Check 'work area reserves taskbar slot' ($wa.Bottom -le $screenH -and $wa.Bottom -ge $screenH - 96) `
        "($($wa.Right)x$($wa.Bottom))"
    Shot (Join-Path $shots '01-hidden.png')
    Start-Sleep -Milliseconds 350
    Shot (Join-Path $shots '02-still-hidden.png')

    # --- 2. курсор в нижние 2 px -> мгновенно показать -------------------
    $cx = [int]($screenW / 2)
    Set-Cursor $cx ($screenH - 1) | Out-Null
    $ms = Wait-Visible $tray $true
    Check 'taskbar appears on bottom-edge hover' ($ms -ge 0) "($ms ms)"
    Check 'appears fast (< 40 ms)' ($ms -ge 0 -and $ms -lt 40) "($ms ms)"
    Start-Sleep -Milliseconds 120
    Shot (Join-Path $shots '03-shown.png')

    # панель должна остаться, пока курсор на ней (переставляем курсор, если
    # пользователь случайно сдвинул мышь)
    $stays = $true
    for ($i = 0; $i -lt 8; $i++) {
        if (-not (Set-Cursor $cx ($screenH - 1))) { $stays = $false }
        Start-Sleep -Milliseconds 60
    }
    Check 'taskbar stays while cursor is on it' ($stays -and [Nat]::IsWindowVisible($tray))

    # --- 3. курсор ушёл -> мгновенно спрятать -----------------------------
    Set-Cursor $cx ($screenH - 200) | Out-Null
    $ms = Wait-Visible $tray $false
    Check 'taskbar hides when cursor leaves' ($ms -ge 0) "($ms ms)"
    Check 'hides fast (< 40 ms)' ($ms -ge 0 -and $ms -lt 40) "($ms ms)"
    Start-Sleep -Milliseconds 120
    Shot (Join-Path $shots '04-hidden-again.png')

    # --- 4. ещё несколько циклов ------------------------------------------
    $ok = $true
    for ($i = 0; $i -lt 5; $i++) {
        Set-Cursor $cx ($screenH - 1) | Out-Null
        if ((Wait-Visible $tray $true 500) -lt 0) { $ok = $false; break }
        Set-Cursor $cx ($screenH - 120) | Out-Null
        if ((Wait-Visible $tray $false 500) -lt 0) { $ok = $false; break }
    }
    Check '5 show/hide cycles' $ok

    # --- 5. выход по --quit -----------------------------------------------
    Start-Sleep -Milliseconds 150
    Set-Cursor $cx ($screenH - 120) | Out-Null
    & $Exe --quit | Out-Null
    $exited = $proc.WaitForExit(4000)
    Check 'process exits on --quit' $exited
    Start-Sleep -Milliseconds 400

    $tray = Get-Tray
    Check 'taskbar visible after exit' ([Nat]::IsWindowVisible($tray))
    Check 'native auto-hide restored' ((Get-AutoHide) -eq $origAutoHide) `
        "(now $(Get-AutoHide), was $origAutoHide)"
}
finally {
    # что бы ни случилось — вернуть панель и настройки
    if ($proc -and -not $proc.HasExited) {
        & $Exe --quit 2>$null | Out-Null
        $proc.WaitForExit(3000) | Out-Null
    }
    Start-Sleep -Milliseconds 200
    $tray = [Nat]::FindWindow('Shell_TrayWnd', $null)
    if ($tray -ne [IntPtr]::Zero) {
        Set-AutoHide $origAutoHide
        [Nat]::ShowWindow($tray, 5) | Out-Null
    }
    [Nat]::SetCursorPos($origCursor.X, $origCursor.Y) | Out-Null
    Write-Host ""
    Write-Host "screenshots: $shots"
}

Write-Host ""
if ($script:fail -eq 0) { Write-Host 'ALL TESTS PASSED' -ForegroundColor Green; exit 0 }
else { Write-Host "$($script:fail) TEST(S) FAILED" -ForegroundColor Red; exit 1 }

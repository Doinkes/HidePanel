# ---------------------------------------------------------------------------
#  Удаление HidePanel: остановка, снятие автозапуска, удаление файлов.
#
#  .\scripts\uninstall.ps1
#  .\scripts\uninstall.ps1 -KeepSettings
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Target = (Join-Path $env:LOCALAPPDATA 'Programs\HidePanel'),
    [switch]$KeepSettings
)

$ErrorActionPreference = 'Stop'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$exePath = Join-Path $Target 'HidePanel.exe'

# остановить (и восстановить панель задач)
if (Test-Path $exePath) {
    & $exePath --quit | Out-Null
    Start-Sleep -Milliseconds 800
}
$running = Get-Process -Name HidePanel -ErrorAction SilentlyContinue
if ($running) {
    # на всякий случай: вернуть панель перед убийством процесса
    if (Test-Path $exePath) { & $exePath --restore | Out-Null }
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

# автозапуск
if (Get-ItemProperty -Path $runKey -Name 'HidePanel' -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $runKey -Name 'HidePanel'
    Write-Host 'Автозапуск удалён'
}

# файлы
if (Test-Path $Target) {
    Remove-Item $Target -Recurse -Force
    Write-Host "Удалено: $Target"
}

if (-not $KeepSettings) {
    $data = Join-Path $env:LOCALAPPDATA 'HidePanel'
    if (Test-Path $data) {
        Remove-Item $data -Recurse -Force
        Write-Host "Удалено: $data (лог и состояние)"
    }
}

Write-Host 'HidePanel удалён.'

# ---------------------------------------------------------------------------
#  Установка HidePanel: копирование в %LOCALAPPDATA%\Programs\HidePanel
#  и автозапуск при входе в систему.
#
#  .\scripts\install.ps1
#  .\scripts\install.ps1 -NoAutostart
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Target = (Join-Path $env:LOCALAPPDATA 'Programs\HidePanel'),
    [switch]$NoAutostart,
    [string]$Source
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root 'dist\HidePanel.exe' }
if (-not (Test-Path $Source)) { throw "Не найден $Source — сначала соберите: .\build.ps1" }

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# остановить работающий экземпляр
$running = Get-Process -Name HidePanel -ErrorAction SilentlyContinue
if ($running) {
    Write-Host 'Останавливаю запущенный HidePanel...'
    & $Source --quit | Out-Null
    Start-Sleep -Milliseconds 800
    $running = Get-Process -Name HidePanel -ErrorAction SilentlyContinue
    if ($running) { $running | Stop-Process -Force; Start-Sleep -Milliseconds 300 }
}

New-Item -ItemType Directory -Force -Path $Target | Out-Null
Copy-Item $Source (Join-Path $Target 'HidePanel.exe') -Force

# пользовательский конфиг, если он есть рядом с исходником
$ini = Join-Path (Split-Path -Parent $Source) 'HidePanel.ini'
if (Test-Path $ini) {
    Copy-Item $ini (Join-Path $Target 'HidePanel.ini') -Force
    Write-Host 'Скопирован HidePanel.ini'
}

$exePath = Join-Path $Target 'HidePanel.exe'

if (-not $NoAutostart) {
    if (-not (Test-Path $runKey)) { New-Item -Path $runKey -Force | Out-Null }
    Set-ItemProperty -Path $runKey -Name 'HidePanel' -Value ('"{0}"' -f $exePath)
    Write-Host "Автозапуск добавлен: $runKey\HidePanel"
}

Start-Process -FilePath $exePath
Write-Host ''
Write-Host "Установлено: $exePath"
Write-Host 'Выход: Ctrl+Alt+Q (или HidePanel.exe --quit)'

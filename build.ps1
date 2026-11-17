# ---------------------------------------------------------------------------
#  Сборка HidePanel
#  
#  .\build.ps1                 # авто: MSVC, если найден, иначе MinGW
#  .\build.ps1 -Compiler mingw # принудительно MinGW (g++)
#  .\build.ps1 -Zip            # + dist\HidePanel-<ver>-win64.zip
#  .\build.ps1 -Clean
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [ValidateSet('auto', 'msvc', 'mingw')]
    [string]$Compiler = 'auto',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [switch]$Zip,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$src   = Join-Path $root 'src'
$build = Join-Path $root 'build'
$dist  = Join-Path $root 'dist'

Set-Location $root

$version = '1.0.0'
if (Test-Path (Join-Path $root 'VERSION')) {
    $version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
}

if ($Clean) {
    Remove-Item $build, $dist -Recurse -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force -Path $build, $dist | Out-Null

# --- иконка (нужна ресурсному компилятору) ---------------------------------
if (-not (Test-Path (Join-Path $root 'assets\icon.ico'))) {
    Write-Host 'icon.ico not found, generating...'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\make-icon.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'icon generation failed' }
}

$exePath = Join-Path $dist 'HidePanel.exe'

function Find-MsvcVcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }

    $vs = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
    if (-not $vs) { return $null }

    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $vcvars) { return $vcvars }
    return $null
}

function Build-Msvc([string]$vcvars) {
    $defines = '/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX'
    if ($Config -eq 'Release') {
        $flags = "/O2 /MT /GS- /guard:cf- /DNDEBUG"
    } else {
        $flags = "/Od /MTd /Zi /DDEBUG"
    }

    $cmd = @(
        "call `"$vcvars`" >nul",
        "rc /nologo /I `"$src`" /fo `"$build\resources.res`" `"src\resources.rc`"",
        "cl /nologo /std:c++17 /EHsc /W4 $flags $defines `/Fo`"$build\main.obj`" `/Fe`"$exePath`" `"src\main.cpp`" `"$build\resources.res`" /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF user32.lib shell32.lib"
    ) -join ' && '

    Write-Host "MSVC: $vcvars"
    & cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "MSVC build failed (exit $LASTEXITCODE)" }
}

function Build-Mingw {
    $gxx = (Get-Command g++ -ErrorAction SilentlyContinue)
    if (-not $gxx) { throw 'g++ not found' }

    $flags = if ($Config -eq 'Release') {
        '-O2 -s -DNDEBUG'
    } else {
        '-O0 -g -DDEBUG'
    }

    Write-Host "MinGW: $($gxx.Source)"
    & windres -I "$src" -i "src\resources.rc" -o "$build\resources.o" --output-format=coff
    if ($LASTEXITCODE -ne 0) { throw "windres failed (exit $LASTEXITCODE)" }

    $args = @(
        '-std=c++17', '-mwindows', '-static', '-static-libgcc', '-static-libstdc++',
        '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
        '-Isrc', 'src\main.cpp', 'build\resources.o',
        '-o', $exePath,
        '-luser32', '-lshell32', '-lgdi32'
    ) + ($flags -split '\s+')

    & $gxx.Source @args
    if ($LASTEXITCODE -ne 0) { throw "MinGW build failed (exit $LASTEXITCODE)" }
}

$vcvars = Find-MsvcVcVars
$done   = $false

switch ($Compiler) {
    'msvc' {
        if (-not $vcvars) { throw 'MSVC not found (need Visual Studio Build Tools with C++)' }
        Build-Msvc $vcvars
        $done = $true
    }
    'mingw' {
        Build-Mingw
        $done = $true
    }
    'auto' {
        if ($vcvars) {
            try { Build-Msvc $vcvars; $done = $true }
            catch {
                Write-Warning $_
                Write-Host 'Falling back to MinGW...'
            }
        }
        if (-not $done) { Build-Mingw; $done = $true }
    }
}

if (-not (Test-Path $exePath)) { throw 'HidePanel.exe was not produced' }

$fi = Get-Item $exePath
Write-Host ("OK  {0}  ({1:N1} KB)  {2}" -f $fi.FullName, ($fi.Length / 1KB), $version)

if ($Zip) {
    $zipPath = Join-Path $dist ("HidePanel-{0}-win64.zip" -f $version)
    Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
    $stage = Join-Path $build 'stage'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item $exePath $stage
    Copy-Item (Join-Path $root 'README.md') $stage
    Copy-Item (Join-Path $root 'LICENSE') $stage
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "ZIP $zipPath"
}

# Renders the main screens, light and dark, into dist\windows\screenshots with the app's own
# --screenshot mode (the counterpart of `make screenshots` on Linux). Run after `make app`.
#   scripts\screenshots-windows.ps1 [-Pages overview,processes,...]
param(
    [string[]]$Pages = @('overview', 'processes', 'cpu', 'history', 'inspect', 'startup', 'services')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root 'dist\windows\Procyon.exe'
$out = Join-Path $root 'dist\windows\screenshots'
if (-not (Test-Path $app)) { throw "$app is missing: run make app first" }
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($page in $Pages) {
    foreach ($theme in 'light', 'dark') {
        $file = Join-Path $out "$page-$theme.png"
        $process = Start-Process -FilePath $app -ArgumentList @('--screenshot', "`"$file`"", "--$theme", '--page', $page) `
            -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "couldn't render $page ($theme)" }
    }
}
Write-Output $out

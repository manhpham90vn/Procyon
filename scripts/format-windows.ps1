# Rewrites sources in place on Windows: clang-format for C/C++, and the generated token files
# when python is available (the counterpart of scripts/format.sh; Swift is formatted on macOS).
#
#   powershell -ExecutionPolicy Bypass -File scripts\format-windows.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
. (Join-Path $PSScriptRoot "dev-env-windows.ps1")

$python = Find-Python
if ($python) { & $python scripts/gen-tokens.py | Out-Null }

$clangFormat = Find-ClangFormat
if (-not $clangFormat) {
    Write-Error "clang-format not found: run scripts\tools-windows.ps1 (or make tools)"
    exit 1
}
& $clangFormat -i @(Get-CppFiles)
if ($LASTEXITCODE -ne 0) { exit 1 }
Write-Host "Formatted."

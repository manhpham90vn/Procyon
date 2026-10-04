# Read-only checks on Windows, the counterpart of scripts/lint.sh: generated files up to date,
# clang-format, Python syntax. Exits non-zero on the first failing group.
#
#   powershell -ExecutionPolicy Bypass -File scripts\lint-windows.ps1
$ErrorActionPreference = "Stop"
# Exit codes are checked explicitly below; pwsh 7.4 would otherwise turn every non-zero native
# exit into a terminating error before the message is printed.
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
. (Join-Path $PSScriptRoot "dev-env-windows.ps1")

function Step([string]$title) { Write-Host ""; Write-Host "==> $title" }

$python = Find-Python
if ($python) {
    Step "Generated files are up to date (tokens, process catalog)"
    & $python scripts/gen-tokens.py --check
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & $python scripts/gen-catalog.py --check
    if ($LASTEXITCODE -ne 0) { exit 1 }
} else {
    Step "Generated files: skipped (no python on PATH; CI checks them)"
}

Step "clang-format"
$clangFormat = Find-ClangFormat
if (-not $clangFormat) {
    Write-Error "clang-format not found: run scripts\tools-windows.ps1 (or make tools)"
    exit 1
}
& $clangFormat --dry-run -Werror @(Get-CppFiles)
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($python) {
    Step "python"
    & $python -m py_compile scripts/gen-tokens.py scripts/gen-catalog.py scripts/bench-macos.py scripts/bench-windows.py
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

Write-Host ""
Write-Host "All checks passed."

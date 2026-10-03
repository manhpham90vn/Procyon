# Fetches the build tools the Windows build needs but Visual Studio may not provide, into
# build\tools (ignored by git): CMake, Ninja and clang-format. scripts\build-windows.cmd and the
# Makefile put that folder on PATH, so nothing has to be installed system-wide.
#
#   powershell -ExecutionPolicy Bypass -File scripts\tools-windows.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tools = Join-Path $root "build\tools"
New-Item -ItemType Directory -Force $tools | Out-Null

$cmakeVersion = "3.31.6"
$ninjaVersion = "1.12.1"
$clangFormatRelease = "master-f4f85437"  # muttleyxd/clang-format-static-binaries, clang-format 19

function Fetch([string]$url, [string]$target) {
    Write-Host "  $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing
}

if (-not (Test-Path (Join-Path $tools "cmake\bin\cmake.exe"))) {
    Write-Host "CMake $cmakeVersion"
    $zip = Join-Path $tools "cmake.zip"
    Fetch "https://github.com/Kitware/CMake/releases/download/v$cmakeVersion/cmake-$cmakeVersion-windows-x86_64.zip" $zip
    Expand-Archive -Path $zip -DestinationPath $tools -Force
    if (Test-Path (Join-Path $tools "cmake")) { Remove-Item -Recurse -Force (Join-Path $tools "cmake") }
    Rename-Item (Join-Path $tools "cmake-$cmakeVersion-windows-x86_64") "cmake"
    Remove-Item $zip
}
if (-not (Test-Path (Join-Path $tools "ninja.exe"))) {
    Write-Host "Ninja $ninjaVersion"
    $zip = Join-Path $tools "ninja.zip"
    Fetch "https://github.com/ninja-build/ninja/releases/download/v$ninjaVersion/ninja-win.zip" $zip
    Expand-Archive -Path $zip -DestinationPath $tools -Force
    Remove-Item $zip
}
if (-not (Test-Path (Join-Path $tools "clang-format.exe"))) {
    Write-Host "clang-format ($clangFormatRelease)"
    Fetch "https://github.com/muttleyxd/clang-format-static-binaries/releases/download/$clangFormatRelease/clang-format-19_windows-amd64.exe" (Join-Path $tools "clang-format.exe")
}

& (Join-Path $tools "cmake\bin\cmake.exe") --version | Select-Object -First 1
"ninja " + (& (Join-Path $tools "ninja.exe") --version)
& (Join-Path $tools "clang-format.exe") --version
Write-Host "Tools are in $tools"

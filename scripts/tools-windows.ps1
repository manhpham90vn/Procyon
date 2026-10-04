# Fetches the build tools the Windows build needs but Visual Studio may not provide, into
# build\tools (ignored by git): CMake, Ninja, clang-format and, when the machine has no Python, the
# embeddable Python the lint and bench scripts run on. scripts\build-windows.cmd and the Makefile
# put that folder on PATH, so nothing has to be installed system-wide.
#
#   powershell -ExecutionPolicy Bypass -File scripts\tools-windows.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tools = Join-Path $root "build\tools"
New-Item -ItemType Directory -Force $tools | Out-Null
. (Join-Path $PSScriptRoot "dev-env-windows.ps1")

$cmakeVersion = "3.31.6"
$ninjaVersion = "1.12.1"
$pythonVersion = "3.12.7"
# muttleyxd/clang-tools-static-binaries (the old clang-format-static-binaries name redirects, but
# release downloads under it 404): the release that ships clang-format 19.1.0.
$clangFormatRelease = "master-796e77c"
$clangFormatAsset = "clang-format-19_windows-amd64"

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
    $base = "https://github.com/muttleyxd/clang-tools-static-binaries/releases/download/$clangFormatRelease/$clangFormatAsset"
    $exe = Join-Path $tools "clang-format.exe"
    $sum = Join-Path $tools "clang-format.sha512sum"
    Fetch "$base.exe" $exe
    Fetch "$base.sha512sum" $sum
    # The published checksum, so a swapped binary never formats the tree.
    $expected = ((Get-Content $sum -Raw) -split '\s+')[0].ToLowerInvariant()
    $actual = (Get-FileHash -Algorithm SHA512 $exe).Hash.ToLowerInvariant()
    Remove-Item $sum
    if ($expected -ne $actual) {
        Remove-Item $exe
        throw "clang-format download does not match its sha512sum"
    }
}
if (-not (Find-Python)) {
    # The embeddable distribution: python.exe and the standard library, nothing registered or on PATH.
    Write-Host "Python $pythonVersion (embeddable)"
    $zip = Join-Path $tools "python.zip"
    Fetch "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip" $zip
    Expand-Archive -Path $zip -DestinationPath (Join-Path $tools "python") -Force
    Remove-Item $zip
}

# Versions, read whole: a native command cut off mid-pipeline is an error in pwsh 7.
$cmakeOut = @(& (Join-Path $tools "cmake\bin\cmake.exe") --version)
Write-Host $cmakeOut[0]
$ninjaOut = @(& (Join-Path $tools "ninja.exe") --version)
Write-Host "ninja $($ninjaOut[0])"
$clangOut = @(& (Join-Path $tools "clang-format.exe") --version)
Write-Host $clangOut[0]
$pythonOut = @(& (Find-Python) --version)
Write-Host $pythonOut[0]
Write-Host "Tools are in $tools"

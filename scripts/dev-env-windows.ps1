# Dot-sourced by the Windows format/lint scripts: locates tools and lists the files they work on
# (the counterpart of scripts/dev-env.sh).

function Find-ClangFormat {
    $root = Split-Path -Parent $PSScriptRoot
    $local = Join-Path $root "build\tools\clang-format.exe"
    if (Test-Path $local) { return $local }
    $onPath = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    # Visual Studio's optional LLVM component.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vs) {
            $candidate = Join-Path $vs "VC\Tools\Llvm\x64\bin\clang-format.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    return $null
}

function Find-Python {
    # The embeddable Python scripts\tools-windows.ps1 fetches when the machine has none.
    $local = Join-Path (Split-Path -Parent $PSScriptRoot) "build\tools\python\python.exe"
    if (Test-Path $local) { return $local }
    # The Microsoft Store alias answers to `python` but is not an interpreter: check it runs.
    foreach ($name in @("python", "python3", "py")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        # Run it through cmd so the Store alias's complaint on stderr is text, not an error.
        $out = cmd /c "`"$($cmd.Source)`" --version 2>&1"
        if ($LASTEXITCODE -eq 0 -and "$out" -match "Python 3") { return $cmd.Source }
    }
    return $null
}

function Get-CppFiles {
    $root = Split-Path -Parent $PSScriptRoot
    Get-ChildItem -Recurse -File -Path (Join-Path $root "core"), (Join-Path $root "apps\ui\src"), (Join-Path $root "apps\windows\src"), (Join-Path $root "apps\windows\tools") -Include *.cpp, *.hpp, *.h, *.c |
        Where-Object { $_.Name -ne "Tokens.generated.h" -and $_.Name -ne "icons_data.hpp" } |
        Sort-Object FullName |
        ForEach-Object { $_.FullName }
}

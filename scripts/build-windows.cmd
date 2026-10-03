@echo off
rem Builds the core, the CLI, the tests and the Win32 app with MSVC. Needs Visual Studio 2022 (the
rem "Desktop development with C++" workload) and CMake + Ninja (the workload's "C++ CMake tools"
rem component, or any copy on PATH).
rem
rem   scripts\build-windows.cmd            release build into build\windows, runs the core tests
rem   scripts\build-windows.cmd debug      debug build into build\windows-debug
rem   scripts\build-windows.cmd release --no-tests
rem
rem Outputs: build\<dir>\procyon-cli.exe, procyon-core-tests.exe, apps\windows\Procyon.exe,
rem and dist\windows\Procyon.exe (release).
setlocal EnableDelayedExpansion
cd /d "%~dp0.."

set CONFIG=Release
set BUILD_DIR=build\windows
set RUN_TESTS=1
for %%a in (%*) do (
    if /i "%%a"=="debug" set CONFIG=Debug
    if /i "%%a"=="debug" set BUILD_DIR=build\windows-debug
    if /i "%%a"=="release" set CONFIG=Release
    if /i "%%a"=="release" set BUILD_DIR=build\windows
    if /i "%%a"=="--no-tests" set RUN_TESTS=0
)

rem MSVC environment: skip when a developer prompt already provides it.
where cl.exe >nul 2>nul
if not errorlevel 1 goto :tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
rem Visual Studio's own CMake and Ninja, when the component is installed.
set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

:tools
rem Tools fetched by scripts\tools-windows.ps1 (`make tools`) come first.
if exist "build\tools\cmake\bin\cmake.exe" set "PATH=%CD%\build\tools\cmake\bin;%CD%\build\tools;%PATH%"
where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo cmake not found: run scripts\tools-windows.ps1 ^(make tools^), install the "C++ CMake tools for Windows" component, or put cmake on PATH. >&2
    exit /b 1
)
where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo ninja not found: run scripts\tools-windows.ps1 ^(make tools^), install the "C++ CMake tools for Windows" component, or put ninja on PATH. >&2
    exit /b 1
)

set /p VERSION=<VERSION
cmake -S core -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DPROCYON_VERSION=%VERSION%
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
if "%RUN_TESTS%"=="1" (
    ctest --test-dir "%BUILD_DIR%" --output-on-failure
    if errorlevel 1 exit /b 1
)

if /i not "%CONFIG%"=="Release" goto :done
if not exist "%BUILD_DIR%\apps\windows\Procyon.exe" goto :done
if not exist dist\windows mkdir dist\windows
copy /y "%BUILD_DIR%\apps\windows\Procyon.exe" dist\windows\Procyon.exe >nul
if errorlevel 1 (
    echo Could not update dist\windows\Procyon.exe: close the running Procyon and build again. 1>&2
    exit /b 1
)
echo Built dist\windows\Procyon.exe
goto :done

:novs
echo Visual Studio 2022 with the C++ workload is required. >&2
exit /b 1

:done
endlocal

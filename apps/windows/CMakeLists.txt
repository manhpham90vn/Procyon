# Procyon for Windows: a Win32 window and a Direct2D canvas around the shared UI (apps/ui). Built
# by core/CMakeLists.txt (add_subdirectory) so everything shares one build tree;
# scripts/build-windows.cmd drives it.
cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
  return()
endif()

add_executable(Procyon WIN32
  src/main.cpp
  src/window.cpp
  src/canvas_d2d.cpp
  src/platform_windows.cpp
  res/Procyon.rc
  res/Procyon.manifest
)
# `design` is on the include path for the fonts the .rc embeds (fonts/InterVariable.ttf).
target_include_directories(Procyon PRIVATE src res "${CMAKE_CURRENT_SOURCE_DIR}/../../design")
target_link_libraries(Procyon PRIVATE procyon_ui d2d1 dwrite d3d11 dxgi dcomp dxguid dwmapi comctl32 shell32 ole32 uxtheme gdi32 windowscodecs)
target_compile_options(Procyon PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus /wd4100)
target_compile_definitions(Procyon PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set_target_properties(Procyon PROPERTIES OUTPUT_NAME "Procyon")

# Measurement tool for scripts/bench-windows.py: launches the app, times its first window and
# samples its CPU and private working set (the counterpart of scripts/bench-probe.swift).
add_executable(procyon-bench-probe tools/bench_probe.cpp)
target_include_directories(procyon-bench-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../ui/src")
target_link_libraries(procyon-bench-probe PRIVATE ntdll user32)
target_compile_options(procyon-bench-probe PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
target_compile_definitions(procyon-bench-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

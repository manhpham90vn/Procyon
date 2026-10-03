# Procyon for Windows: a Win32 + Direct2D app over the shared core. Built by core/CMakeLists.txt
# (add_subdirectory) so the two share one build tree; scripts/build-windows.cmd drives it.
cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
  return()
endif()

# Version from the VERSION file (or -DPROCYON_VERSION=…), plus the commit for the About page.
if(NOT PROCYON_VERSION)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" PROCYON_VERSION)
  string(STRIP "${PROCYON_VERSION}" PROCYON_VERSION)
endif()
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${PROCYON_VERSION}")
set(PROCYON_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PROCYON_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PROCYON_VERSION_PATCH "${CMAKE_MATCH_3}")
if(NOT PROCYON_VERSION_MAJOR)
  set(PROCYON_VERSION_MAJOR 0)
  set(PROCYON_VERSION_MINOR 0)
  set(PROCYON_VERSION_PATCH 0)
endif()
find_package(Git QUIET)
set(PROCYON_COMMIT "")
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE PROCYON_COMMIT
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

add_executable(Procyon WIN32
  src/main.cpp
  src/window.cpp
  src/render.cpp
  src/format.cpp
  src/store.cpp
  src/history.cpp
  src/alerts.cpp
  src/widgets.cpp
  src/pages_common.cpp
  src/page_overview.cpp
  src/page_processes.cpp
  src/page_performance.cpp
  src/page_services.cpp
  src/page_inspect.cpp
  src/page_history.cpp
  src/page_system.cpp
  src/overlays.cpp
  res/Procyon.rc
  res/Procyon.manifest
)
target_include_directories(Procyon PRIVATE src res "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(Procyon PRIVATE procyon_core d2d1 dwrite dwmapi comctl32 shell32 ole32 uxtheme gdi32)
target_compile_options(Procyon PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus /wd4100)
target_compile_definitions(Procyon PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set_target_properties(Procyon PROPERTIES OUTPUT_NAME "Procyon")

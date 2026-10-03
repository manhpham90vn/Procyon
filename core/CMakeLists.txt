cmake_minimum_required(VERSION 3.20)
project(procyon_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The portable half: rates and deltas, views, the C ABI and the helper client.
set(PROCYON_CORE_SOURCES
  src/monitor.cpp
  src/view.cpp
  src/helper_client.cpp
)
if(APPLE)
  list(APPEND PROCYON_CORE_SOURCES
    src/helper_server.cpp
    src/platform/macos.cpp
    src/platform/macos_netstat.cpp
    src/platform/macos_gpu.cpp
    src/platform/macos_power.cpp
    src/platform/macos_launchd.cpp
    src/platform/macos_handles.cpp
  )
elseif(WIN32)
  list(APPEND PROCYON_CORE_SOURCES
    src/platform/windows.cpp
    src/platform/windows_services.cpp
    src/platform/windows_handles.cpp
    src/platform/windows_gpu.cpp
    src/platform/windows_power.cpp
    src/platform/windows_network.cpp
  )
else()
  message(FATAL_ERROR "No platform adapter for ${CMAKE_SYSTEM_NAME} yet (see docs/procyon-spec.md)")
endif()

add_library(procyon_core STATIC ${PROCYON_CORE_SOURCES})
target_include_directories(procyon_core PUBLIC include PRIVATE src)

if(MSVC)
  target_compile_options(procyon_core PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus)
  target_compile_definitions(procyon_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(procyon_core PRIVATE -Wall -Wextra)
endif()

if(APPLE)
  target_link_libraries(procyon_core PUBLIC "-framework CoreFoundation" "-framework CoreServices" "-framework IOKit" "-framework Security" bsm)
elseif(WIN32)
  target_link_libraries(procyon_core PUBLIC
    advapi32 iphlpapi ws2_32 version pdh dxgi setupapi ole32 oleaut32 wbemuuid shell32 user32 taskschd)
endif()

# Prototype/measurement tool: prints live samples and the core's own overhead.
add_executable(procyon-cli tools/procyon_cli.cpp)
target_link_libraries(procyon-cli PRIVATE procyon_core)

# Privileged helper: LaunchDaemon (--daemon) or launched with elevated rights by the UI. Windows
# has none: the app relaunches itself elevated instead.
if(APPLE)
  add_executable(procyon-helper helper/main.c)
  target_link_libraries(procyon-helper PRIVATE procyon_core)
endif()

# Wire format, validation and refusal checks (`ctest`, run by `make core`).
enable_testing()
add_executable(procyon-core-tests tests/core_tests.cpp)
target_include_directories(procyon-core-tests PRIVATE src)
target_link_libraries(procyon-core-tests PRIVATE procyon_core)
if(MSVC)
  target_compile_options(procyon-core-tests PRIVATE /utf-8)
  target_compile_options(procyon-cli PRIVATE /utf-8)
endif()
add_test(NAME core COMMAND procyon-core-tests)

# The Win32 app lives next to the core and is built from the same tree.
option(PROCYON_BUILD_APP "Build the native app for this platform (Windows)" ON)
if(WIN32 AND PROCYON_BUILD_APP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../apps/windows/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../apps/windows" apps/windows)
endif()

cmake_minimum_required(VERSION 3.20)
project(procyon_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(procyon_core STATIC
  src/monitor.cpp
  src/view.cpp
  src/helper_client.cpp
  src/helper_server.cpp
  src/platform/macos.cpp
  src/platform/macos_netstat.cpp
  src/platform/macos_gpu.cpp
  src/platform/macos_power.cpp
  src/platform/macos_launchd.cpp
  src/platform/macos_handles.cpp
)
target_include_directories(procyon_core PUBLIC include PRIVATE src)
target_compile_options(procyon_core PRIVATE -Wall -Wextra)

if(APPLE)
  target_link_libraries(procyon_core PUBLIC "-framework CoreFoundation" "-framework CoreServices" "-framework IOKit" "-framework Security" bsm)
endif()

# Prototype/measurement tool: prints live samples and the core's own overhead.
add_executable(procyon-cli tools/procyon_cli.cpp)
target_link_libraries(procyon-cli PRIVATE procyon_core)

# Privileged helper: LaunchDaemon (--daemon) or launched with elevated rights by the UI.
add_executable(procyon-helper helper/main.c)
target_link_libraries(procyon-helper PRIVATE procyon_core)

# Wire format, validation and refusal checks (`ctest`, run by `make core`).
enable_testing()
add_executable(procyon-core-tests tests/core_tests.cpp)
target_include_directories(procyon-core-tests PRIVATE src)
target_link_libraries(procyon-core-tests PRIVATE procyon_core)
add_test(NAME core COMMAND procyon-core-tests)

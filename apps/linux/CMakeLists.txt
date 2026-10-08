# Procyon for Linux: a GTK 4 window and a Cairo/Pango canvas around the shared UI (apps/ui). Built
# by core/CMakeLists.txt (add_subdirectory) so everything shares one build tree; `make app` drives it.
cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)

set(PROCYON_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../design/fonts")
get_filename_component(PROCYON_FONT_DIR "${PROCYON_FONT_DIR}" ABSOLUTE)

add_executable(procyon
  src/main.cpp
  src/window.cpp
  src/canvas_cairo.cpp
  src/platform_linux.cpp
  src/tray_linux.cpp
  src/fonts.cpp
)
target_link_libraries(procyon PRIVATE procyon_ui PkgConfig::GTK4 PkgConfig::FONTCONFIG PkgConfig::GIO_UNIX)
target_compile_options(procyon PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# The fonts are assembled into the binary (.incbin): rebuild fonts.cpp when they change.
target_compile_definitions(procyon PRIVATE PROCYON_FONT_DIR="${PROCYON_FONT_DIR}")
set_source_files_properties(src/fonts.cpp PROPERTIES OBJECT_DEPENDS
  "${PROCYON_FONT_DIR}/InterVariable.ttf;${PROCYON_FONT_DIR}/NunitoVariable.ttf")

include(GNUInstallDirs)
install(TARGETS procyon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/dev.procyon.Procyon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/dev.procyon.Procyon.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ../macos/Resources/AppIcon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME dev.procyon.Procyon.png)
install(FILES ../../LICENSE ../../design/fonts/LICENSE-Inter.txt ../../design/fonts/LICENSE-Nunito.txt
              ../../design/icons/LICENSE-Lucide.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/procyon)

cmake_minimum_required(VERSION 3.16)
project(lhctl VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
find_package(Threads REQUIRED)

# The SDK ships a prebuilt libopenvr_api.so. The arm64 one (for the Steam Frame) is vendored;
# point OPENVR_LIB_DIR somewhere else to build on another architecture.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_ovr_default ${CMAKE_SOURCE_DIR}/third_party/openvr/lib/linuxarm64)
else()
  set(_ovr_default "")
endif()
set(OPENVR_LIB_DIR "${_ovr_default}" CACHE PATH "Folder with libopenvr_api.so")
if(NOT EXISTS "${OPENVR_LIB_DIR}/libopenvr_api.so")
  message(FATAL_ERROR "libopenvr_api.so not found; set -DOPENVR_LIB_DIR=<openvr>/bin/<platform>")
endif()

add_executable(lhctl
  src/main.cpp
  src/ble.cpp
  src/model.cpp
  src/canvas.cpp
  src/ui.cpp
  src/overlay.cpp)
target_include_directories(lhctl SYSTEM PRIVATE third_party/openvr third_party/stb)
target_compile_definitions(lhctl PRIVATE LHCTL_VERSION="${PROJECT_VERSION}")
target_compile_options(lhctl PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(lhctl PRIVATE PkgConfig::GIO Threads::Threads ${OPENVR_LIB_DIR}/libopenvr_api.so)
# libopenvr_api.so is installed next to the binary's lib folder.
set_target_properties(lhctl PROPERTIES
  BUILD_RPATH "${OPENVR_LIB_DIR}"
  INSTALL_RPATH "$ORIGIN/../lib")

include(GNUInstallDirs)
install(TARGETS lhctl RUNTIME DESTINATION bin)
install(FILES ${OPENVR_LIB_DIR}/libopenvr_api.so DESTINATION lib)
install(DIRECTORY resources/fonts DESTINATION share/lhctl)

# Desktop integration (app menu / Discover entry, icon, AppStream metadata).
set(LHCTL_APP_ID com.phoenixnet_labs.LighthouseCtl)
install(FILES packaging/${LHCTL_APP_ID}.desktop DESTINATION share/applications)
install(FILES packaging/${LHCTL_APP_ID}.metainfo.xml DESTINATION share/metainfo)
foreach(size 128 256 512)
  install(FILES resources/icons/${size}.png
          DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME ${LHCTL_APP_ID}.png)
endforeach()

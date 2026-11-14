cmake_minimum_required(VERSION 3.28)

project(temporal-denoise VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Dependencies: locate libobs.
#
# Supported layouts:
#   1. In-tree build (add_subdirectory from obs-studio): OBS::libobs exists.
#   2. Standalone against an installed OBS: find_package(libobs) / pkg-config.
#   3. Standalone against an obs-studio source checkout:
#        -DOBS_SOURCE_DIR=/path/to/obs-studio
#      (links against libobs if OBS_LIBRARY is found or given, otherwise the
#       module is built with undefined symbols resolved at load time, which
#       is fine on Linux/macOS for OBS modules)
# ---------------------------------------------------------------------------

if(TARGET OBS::libobs)
  set(OBS_LINK_TARGET OBS::libobs)
elseif(TARGET libobs)
  set(OBS_LINK_TARGET libobs)
else()
  find_package(libobs QUIET)
  if(libobs_FOUND)
    set(OBS_LINK_TARGET libobs)
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LIBOBS_PC QUIET libobs)
    endif()
  endif()
endif()

add_library(temporal-denoise MODULE)
add_library(OBS::temporal-denoise ALIAS temporal-denoise)

target_sources(temporal-denoise PRIVATE temporal-denoise.c)
set_target_properties(temporal-denoise PROPERTIES PREFIX "")

if(OBS_LINK_TARGET)
  target_link_libraries(temporal-denoise PRIVATE ${OBS_LINK_TARGET})
elseif(LIBOBS_PC_FOUND)
  target_include_directories(temporal-denoise PRIVATE ${LIBOBS_PC_INCLUDE_DIRS})
  target_link_libraries(temporal-denoise PRIVATE ${LIBOBS_PC_LIBRARIES})
  target_link_directories(temporal-denoise PRIVATE ${LIBOBS_PC_LIBRARY_DIRS})
else()
  set(OBS_SOURCE_DIR "" CACHE PATH "Path to an obs-studio source checkout")
  if(NOT OBS_SOURCE_DIR)
    message(FATAL_ERROR
      "libobs not found. Either build inside the obs-studio tree, install "
      "OBS development files (find_package/pkg-config), or configure with "
      "-DOBS_SOURCE_DIR=/path/to/obs-studio")
  endif()
  # obs-config.h pulls in the generated obsconfig.h; synthesize a minimal
  # one when the obs tree has not been configured/built.
  if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obsconfig.h")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obs-shim/obsconfig.h"
"#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
")
    target_include_directories(temporal-denoise PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/obs-shim")
  endif()
  target_include_directories(temporal-denoise PRIVATE "${OBS_SOURCE_DIR}/libobs")
  # libobs headers pull in simde (util/sse-intrin.h); locate it.
  set(SIMDE_INCLUDE_DIR "" CACHE PATH "Path to simde headers (prefix containing simde/)")
  foreach(candidate "${SIMDE_INCLUDE_DIR}" /usr/include /usr/local/include
                     "${OBS_SOURCE_DIR}/deps/simde")
    if(EXISTS "${candidate}/simde/x86/sse2.h")
      target_include_directories(temporal-denoise PRIVATE "${candidate}")
      set(SIMDE_FOUND TRUE)
      break()
    endif()
  endforeach()
  if(NOT SIMDE_FOUND)
    message(FATAL_ERROR "simde headers not found; set -DSIMDE_INCLUDE_DIR=...")
  endif()
  set(OBS_LIBRARY "" CACHE FILEPATH "Path to a built libobs shared library (optional)")
  if(OBS_LIBRARY)
    target_link_libraries(temporal-denoise PRIVATE "${OBS_LIBRARY}")
  endif()
  if(APPLE)
    target_link_options(temporal-denoise PRIVATE -undefined dynamic_lookup)
  endif()
endif()

# Offline algorithm test (no OBS dependency).
option(BUILD_OFFLINE_TEST "Build the offline denoise test" ON)
if(BUILD_OFFLINE_TEST)
  add_executable(denoise_offline_test test/offline_test.cpp)
  target_compile_features(denoise_offline_test PRIVATE cxx_std_17)
  if(NOT MSVC)
    target_link_libraries(denoise_offline_test PRIVATE m)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# Default: per-user OBS plugin directory on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED OBS_PLUGIN_DESTINATION)
  set(OBS_PLUGIN_DESTINATION
      "$ENV{HOME}/.config/obs-studio/plugins/temporal-denoise"
      CACHE PATH "Where to install the plugin")
else()
  set(OBS_PLUGIN_DESTINATION "obs-plugins/temporal-denoise"
      CACHE PATH "Where to install the plugin")
endif()

install(TARGETS temporal-denoise
        LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}/bin/64bit"
        RUNTIME DESTINATION "${OBS_PLUGIN_DESTINATION}/bin/64bit")
install(FILES data/temporal-denoise.effect
        DESTINATION "${OBS_PLUGIN_DESTINATION}/data")
install(DIRECTORY data/locale
        DESTINATION "${OBS_PLUGIN_DESTINATION}/data")

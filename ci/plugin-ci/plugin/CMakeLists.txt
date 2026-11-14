# Standalone, out-of-tree build for an OBS Studio plugin.
#
# Build:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DPLUGIN_VERSION=1.2.3
#   cmake --build build -j
#
# libobs discovery (first match wins):
#   1. -Dlibobs_DIR=/path/to/libobs-cmake  (upstream OBS package config)
#   2. -DLibObs_ROOT=/opt/obs              (custom install prefix)
#   3. system paths via the bundled FindLibObs.cmake

cmake_minimum_required(VERSION 3.22)

# Legacy build scripts historically used LIBOBS_ROOT; accept it as an
# undocumented fallback but normalize to the CMake-standard variable.
if(DEFINED LIBOBS_ROOT AND NOT DEFINED LibObs_ROOT)
	set(LibObs_ROOT "${LIBOBS_ROOT}")
endif()

if(POLICY CMP0144)
	cmake_policy(SET CMP0144 NEW)
endif()

# --- Plugin version ----------------------------------------------------------
# The version is always explicit so artifact names are reproducible. CI passes
# -DPLUGIN_VERSION from the release tag; local builds fall back to 0.1.0.
set(PLUGIN_VERSION "0.1.0" CACHE STRING "Plugin semantic version, e.g. 1.2.3")
# Optional pre-release/build suffix appended to artifact names and the
# reported version string, e.g. "rc.1" or "ci.42". The CMake project version
# itself stays a numeric MAJOR.MINOR.PATCH triplet.
set(PLUGIN_VERSION_SUFFIX "" CACHE STRING "Optional pre-release suffix, e.g. rc.1")

if(NOT PLUGIN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
	message(FATAL_ERROR
	  "PLUGIN_VERSION must be a numeric MAJOR.MINOR.PATCH triplet (got: ${PLUGIN_VERSION}); "
	  "pass pre-release tags via -DPLUGIN_VERSION_SUFFIX=rc.1")
endif()

project(obs-ci-demo VERSION "${PLUGIN_VERSION}" LANGUAGES C)

if(PLUGIN_VERSION_SUFFIX)
	set(PROJECT_VERSION_FULL "${PROJECT_VERSION}-${PLUGIN_VERSION_SUFFIX}")
else()
	set(PROJECT_VERSION_FULL "${PROJECT_VERSION}")
endif()

set(PLUGIN_TARGET "obs-ci-demo" CACHE STRING "Output module file name without extension")

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Locate libobs -----------------------------------------------------------
find_package(libobs QUIET CONFIG)
if(NOT TARGET OBS::libobs)
	find_package(LibObs REQUIRED)
endif()

# --- Plugin module -----------------------------------------------------------
add_library(${PLUGIN_TARGET} MODULE src/plugin.c)

target_link_libraries(${PLUGIN_TARGET} PRIVATE OBS::libobs)

target_compile_features(${PLUGIN_TARGET} PRIVATE c_std_11)

set_target_properties(
  ${PLUGIN_TARGET}
  PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)

configure_file(src/plugin-version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin-version.h" @ONLY)
target_include_directories(${PLUGIN_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# OBS >= 31 reads module metadata (including the version) from manifest.json
# in the module data directory. Generate it so the smoke test and OBS itself
# observe the same version string baked into the binary.
configure_file(src/manifest.json.in "${CMAKE_CURRENT_BINARY_DIR}/generated/manifest.json" @ONLY)

# --- Install layout ----------------------------------------------------------
# Matches the OBS plugin search paths on Linux. GNUInstallDirs resolves
# CMAKE_INSTALL_LIBDIR to the multiarch directory (lib/x86_64-linux-gnu) on
# Debian/Ubuntu and plain lib/ on non-multiarch systems:
#   <libdir>/obs-plugins/<module>.so
#   <datadir>/obs/obs-plugins/<module>/manifest.json (+ data)
include(GNUInstallDirs)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/etc/debian_version")
	# Match the distro multiarch layout where OBS looks for plugins:
	# /usr/lib/x86_64-linux-gnu/obs-plugins. Non-Debian distros keep lib/.
	find_program(DPKG_ARCHITECTURE_EXE dpkg-architecture)
	if(DPKG_ARCHITECTURE_EXE)
		execute_process(
		  COMMAND "${DPKG_ARCHITECTURE_EXE}" -q DEB_HOST_MULTIARCH
		  OUTPUT_VARIABLE DEB_HOST_MULTIARCH
		  OUTPUT_STRIP_TRAILING_WHITESPACE
		)
	endif()
endif()

if(DEB_HOST_MULTIARCH)
	set(OBS_PLUGIN_LIBDIR "lib/${DEB_HOST_MULTIARCH}/obs-plugins")
else()
	set(OBS_PLUGIN_LIBDIR "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
endif()

install(TARGETS ${PLUGIN_TARGET} LIBRARY DESTINATION "${OBS_PLUGIN_LIBDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
	DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${PLUGIN_TARGET}"
	OPTIONAL)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/manifest.json"
	DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${PLUGIN_TARGET}")

# Emit machine-readable metadata for the packaging stage. file(GENERATE) is
# used so the target file name generator expression is evaluated correctly.
file(GENERATE
     OUTPUT "${CMAKE_BINARY_DIR}/plugin-meta.txt"
     CONTENT "PLUGIN_NAME=${PROJECT_NAME}\nPLUGIN_VERSION=${PROJECT_VERSION_FULL}\nPLUGIN_TARGET=${PLUGIN_TARGET}\nPLUGIN_FILE=$<TARGET_FILE_NAME:${PLUGIN_TARGET}>\n")

message(STATUS "Configured ${PROJECT_NAME} ${PROJECT_VERSION_FULL} -> ${PLUGIN_TARGET}.so")

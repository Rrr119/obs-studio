# FindLibObs.cmake - locate a system-installed libobs for out-of-tree plugins.
#
# This mirrors how third-party OBS plugins discover an OBS installation that
# was built and installed with `cmake --install` (Debian package or the
# official PPA/AppImage runtime).
#
# Variables:
#   LibObs_ROOT      - hint directory containing the OBS installation prefix
#   LIBOBS_ROOT      - deprecated alias, kept for the Docker/CI build args
#
# Imported targets:
#   OBS::libobs      - shared library to link the plugin module against

find_path(
  LibObs_INCLUDE_DIR
  NAMES obs.h
  PATH_SUFFIXES include include/obs
  HINTS "${LibObs_ROOT}" "${LibObs_ROOT}/include"
  PATHS /usr /usr/local /opt/obs
)

find_library(
  LibObs_LIBRARY
  NAMES obs libobs
  HINTS "${LibObs_ROOT}"
  PATH_SUFFIXES lib lib/x86_64-linux-gnu lib64
  PATHS /usr /usr/local /opt/obs
)

if(LibObs_INCLUDE_DIR AND EXISTS "${LibObs_INCLUDE_DIR}/obs-config.h")
	file(STRINGS "${LibObs_INCLUDE_DIR}/obs-config.h" _api_major
	     REGEX "^#define[ \t]+LIBOBS_API_MAJOR_VER[ \t]+[0-9]+")
	file(STRINGS "${LibObs_INCLUDE_DIR}/obs-config.h" _api_minor
	     REGEX "^#define[ \t]+LIBOBS_API_MINOR_VER[ \t]+[0-9]+")
	file(STRINGS "${LibObs_INCLUDE_DIR}/obs-config.h" _api_patch
	     REGEX "^#define[ \t]+LIBOBS_API_PATCH_VER[ \t]+[0-9]+")
	string(REGEX REPLACE "[^0-9]" "" LibObs_VERSION_MAJOR "${_api_major}")
	string(REGEX REPLACE "[^0-9]" "" LibObs_VERSION_MINOR "${_api_minor}")
	string(REGEX REPLACE "[^0-9]" "" LibObs_VERSION_PATCH "${_api_patch}")
	set(LibObs_VERSION
	    "${LibObs_VERSION_MAJOR}.${LibObs_VERSION_MINOR}.${LibObs_VERSION_PATCH}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  LibObs
  REQUIRED_VARS LibObs_LIBRARY LibObs_INCLUDE_DIR
  VERSION_VAR LibObs_VERSION
)

if(LibObs_FOUND AND NOT TARGET OBS::libobs)
	add_library(OBS::libobs SHARED IMPORTED)
	# obs.h lives under <prefix>/include/obs. OBS plugin sources include
	# headers as <obs-module.h>, so expose both the obs/ directory and its
	# parent (the latter covers <graphics/...> style includes).
	get_filename_component(_libobs_include_parent "${LibObs_INCLUDE_DIR}" DIRECTORY)
	set_target_properties(
	  OBS::libobs
	  PROPERTIES
	    IMPORTED_LOCATION "${LibObs_LIBRARY}"
	    INTERFACE_INCLUDE_DIRECTORIES "${LibObs_INCLUDE_DIR};${_libobs_include_parent}"
	)
endif()

mark_as_advanced(LibObs_INCLUDE_DIR LibObs_LIBRARY)

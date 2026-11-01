# Finds the Zstandard library and defines the zstd::libzstd target.
#
# Uses zstd's own CMake package when there is one (e.g. vcpkg, or zstd built
# with CMake); otherwise looks for zstd.h and the library directly, as most
# Linux distributions only ship a pkg-config file.

find_package(zstd CONFIG QUIET)
if(zstd_FOUND)
	if(NOT TARGET zstd::libzstd)
		add_library(zstd::libzstd INTERFACE IMPORTED)
		if(TARGET zstd::libzstd_shared)
			target_link_libraries(zstd::libzstd INTERFACE zstd::libzstd_shared)
		else()
			target_link_libraries(zstd::libzstd INTERFACE zstd::libzstd_static)
		endif()
	endif()
	return()
endif()

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
find_library(ZSTD_LIBRARY NAMES zstd zstd_static libzstd)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(zstd DEFAULT_MSG ZSTD_LIBRARY ZSTD_INCLUDE_DIR)
mark_as_advanced(ZSTD_INCLUDE_DIR ZSTD_LIBRARY)

if(zstd_FOUND AND NOT TARGET zstd::libzstd)
	add_library(zstd::libzstd UNKNOWN IMPORTED)
	set_target_properties(zstd::libzstd PROPERTIES
		IMPORTED_LOCATION "${ZSTD_LIBRARY}"
		INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
endif()

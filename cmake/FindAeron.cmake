# Find Aeron C++ library
#
# This module defines:
#   Aeron_FOUND - True if Aeron is found
#   Aeron_INCLUDE_DIRS - Include directories for Aeron
#   Aeron_LIBRARIES - Libraries to link against
#   aeron - Imported target for Aeron

find_path(Aeron_INCLUDE_DIR
    NAMES Aeron.h
    PATHS
        ${AERON_ROOT}/include
        /usr/local/include
        /usr/include
    PATH_SUFFIXES aeron
)

find_library(Aeron_LIBRARY
    NAMES aeron
    PATHS
        ${AERON_ROOT}/lib
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Aeron
    REQUIRED_VARS Aeron_LIBRARY Aeron_INCLUDE_DIR
)

if(Aeron_FOUND)
    set(Aeron_LIBRARIES ${Aeron_LIBRARY})
    set(Aeron_INCLUDE_DIRS ${Aeron_INCLUDE_DIR})
    
    if(NOT TARGET aeron)
        add_library(aeron UNKNOWN IMPORTED)
        set_target_properties(aeron PROPERTIES
            IMPORTED_LOCATION "${Aeron_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Aeron_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(Aeron_INCLUDE_DIR Aeron_LIBRARY)

find_path(LIBOLM_INCLUDE_DIR
    NAMES olm/olm.h
    PATHS
        "${CMAKE_SOURCE_DIR}/../libolm/include"
        /usr/include
        /usr/local/include
)

find_library(LIBOLM_LIBRARY
    NAMES olm libolm
    PATHS
        "${CMAKE_SOURCE_DIR}/../libolm/build"
        /usr/lib
        /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibOlm
    REQUIRED_VARS LIBOLM_LIBRARY LIBOLM_INCLUDE_DIR
)

if(LIBOLM_FOUND AND NOT TARGET LibOlm::LibOlm)
    add_library(LibOlm::LibOlm UNKNOWN IMPORTED)
    set_target_properties(LibOlm::LibOlm PROPERTIES
        IMPORTED_LOCATION "${LIBOLM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOLM_INCLUDE_DIR}"
    )
endif()

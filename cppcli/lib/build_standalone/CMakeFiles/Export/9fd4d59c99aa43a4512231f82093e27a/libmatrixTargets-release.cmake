#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libmatrix::libmatrix-core" for configuration "Release"
set_property(TARGET libmatrix::libmatrix-core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libmatrix::libmatrix-core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/liblibmatrix-core.a"
  )

list(APPEND _cmake_import_check_targets libmatrix::libmatrix-core )
list(APPEND _cmake_import_check_files_for_libmatrix::libmatrix-core "${_IMPORT_PREFIX}/lib/liblibmatrix-core.a" )

# Import target "libmatrix::libmatrix-http" for configuration "Release"
set_property(TARGET libmatrix::libmatrix-http APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libmatrix::libmatrix-http PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/liblibmatrix-http.a"
  )

list(APPEND _cmake_import_check_targets libmatrix::libmatrix-http )
list(APPEND _cmake_import_check_files_for_libmatrix::libmatrix-http "${_IMPORT_PREFIX}/lib/liblibmatrix-http.a" )

# Import target "libmatrix::libmatrix-db" for configuration "Release"
set_property(TARGET libmatrix::libmatrix-db APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libmatrix::libmatrix-db PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/liblibmatrix-db.a"
  )

list(APPEND _cmake_import_check_targets libmatrix::libmatrix-db )
list(APPEND _cmake_import_check_files_for_libmatrix::libmatrix-db "${_IMPORT_PREFIX}/lib/liblibmatrix-db.a" )

# Import target "libmatrix::libmatrix-e2ee" for configuration "Release"
set_property(TARGET libmatrix::libmatrix-e2ee APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libmatrix::libmatrix-e2ee PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/liblibmatrix-e2ee.a"
  )

list(APPEND _cmake_import_check_targets libmatrix::libmatrix-e2ee )
list(APPEND _cmake_import_check_files_for_libmatrix::libmatrix-e2ee "${_IMPORT_PREFIX}/lib/liblibmatrix-e2ee.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

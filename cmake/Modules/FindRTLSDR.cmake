# Find librtlsdr
#
# RTLSDR_FOUND        - system has librtlsdr
# RTLSDR_INCLUDE_DIR  - the librtlsdr include directory
# RTLSDR_LIBRARIES    - librtlsdr library to link against

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_RTLSDR QUIET librtlsdr)
endif()

find_path(RTLSDR_INCLUDE_DIR
    NAMES rtl-sdr.h
    HINTS ${PC_RTLSDR_INCLUDE_DIRS}
    PATHS /usr/include /usr/local/include /opt/local/include /opt/homebrew/include
)

find_library(RTLSDR_LIBRARIES
    NAMES rtlsdr
    HINTS ${PC_RTLSDR_LIBRARY_DIRS}
    PATHS /usr/lib /usr/local/lib /opt/local/lib /opt/homebrew/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RTLSDR DEFAULT_MSG RTLSDR_LIBRARIES RTLSDR_INCLUDE_DIR)
mark_as_advanced(RTLSDR_INCLUDE_DIR RTLSDR_LIBRARIES)

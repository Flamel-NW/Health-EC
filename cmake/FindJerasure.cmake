# FindJerasure.cmake
#
# Locates the Jerasure 2.x library (+ gf-complete dependency) as installed by
# the Debian/Ubuntu package libjerasure-dev / libgf-complete-dev.
#
# Two Debian packaging quirks handled here:
#   1. The shared library is named libJerasure.so (capital J), not libjerasure.so.
#   2. jerasure.h includes "galois.h" with a plain name, but galois.h lives in
#      the jerasure/ subdirectory.  Both the parent dir and the subdirectory
#      must therefore appear on the include path.
#
# Imported target:
#   Jerasure::Jerasure   (INTERFACE_INCLUDE_DIRECTORIES + INTERFACE_LINK_LIBRARIES)
#
# Cache variables set on success:
#   JERASURE_INCLUDE_DIR     path containing jerasure.h  (e.g. /usr/include)
#   JERASURE_LIBRARY         path to libJerasure.so
#   GF_COMPLETE_LIBRARY      path to libgf_complete.so

find_path(JERASURE_INCLUDE_DIR
    NAMES jerasure.h
    PATHS /usr/include /usr/local/include
)

find_library(JERASURE_LIBRARY
    NAMES Jerasure          # capital J – Debian packaging quirk
    PATHS /usr/lib /usr/local/lib
    PATH_SUFFIXES x86_64-linux-gnu
)

find_library(GF_COMPLETE_LIBRARY
    NAMES gf_complete       # underscore, not hyphen
    PATHS /usr/lib /usr/local/lib
    PATH_SUFFIXES x86_64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Jerasure
    REQUIRED_VARS JERASURE_INCLUDE_DIR JERASURE_LIBRARY GF_COMPLETE_LIBRARY
)

if(Jerasure_FOUND AND NOT TARGET Jerasure::Jerasure)
    add_library(Jerasure::Jerasure INTERFACE IMPORTED)
    set_target_properties(Jerasure::Jerasure PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES
            "${JERASURE_INCLUDE_DIR};${JERASURE_INCLUDE_DIR}/jerasure"
        INTERFACE_LINK_LIBRARIES
            "${JERASURE_LIBRARY};${GF_COMPLETE_LIBRARY}"
    )
endif()

mark_as_advanced(JERASURE_INCLUDE_DIR JERASURE_LIBRARY GF_COMPLETE_LIBRARY)

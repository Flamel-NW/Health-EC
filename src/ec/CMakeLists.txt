add_library(healthec_ec STATIC
    jerasure_wrapper.cpp
)

target_include_directories(healthec_ec PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."   # allows #include "ec/jerasure_wrapper.h"
    "${CMAKE_CURRENT_SOURCE_DIR}"      # allows #include "jerasure_wrapper.h" within ec/
)

target_link_libraries(healthec_ec PUBLIC Jerasure::Jerasure)

target_compile_options(healthec_ec PRIVATE -Wall -Wextra -Wpedantic)

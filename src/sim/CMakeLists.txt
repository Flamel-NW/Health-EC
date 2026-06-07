add_library(healthec_sim STATIC
    disk_simulator.cpp
)

target_include_directories(healthec_sim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # allows #include "sim/disk_simulator.h"
    ${CMAKE_CURRENT_SOURCE_DIR}      # allows #include "disk_simulator.h" within sim/
)

target_link_libraries(healthec_sim PUBLIC healthec_core)

target_compile_options(healthec_sim PRIVATE -Wall -Wextra)

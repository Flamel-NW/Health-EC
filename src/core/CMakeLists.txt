add_library(healthec_core STATIC
    score_manager.cpp
    placement_policy.cpp
    read_scheduler.cpp
)

target_include_directories(healthec_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # allows #include "core/score_manager.h"
    ${CMAKE_CURRENT_SOURCE_DIR}      # allows #include "score_manager.h" within core/
)

target_compile_options(healthec_core PRIVATE -Wall -Wextra)

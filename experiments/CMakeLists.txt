add_executable(calibrate_params tools/calibrate_params.cpp)

target_include_directories(calibrate_params PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(calibrate_params PRIVATE healthec_sim)

target_compile_options(calibrate_params PRIVATE -Wall -Wextra)

add_executable(calibrate_thresholds tools/calibrate_thresholds.cpp)

target_include_directories(calibrate_thresholds PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(calibrate_thresholds PRIVATE healthec_sim)

target_compile_options(calibrate_thresholds PRIVATE -Wall -Wextra)

add_executable(compare_policies tools/compare_policies.cpp)

target_include_directories(compare_policies PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(compare_policies PRIVATE healthec_sim)

target_compile_options(compare_policies PRIVATE -Wall -Wextra)

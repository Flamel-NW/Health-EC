add_executable(calibrate_params tools/calibrate_params.cpp)

target_include_directories(calibrate_params PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(calibrate_params PRIVATE healthec_sim)

target_compile_options(calibrate_params PRIVATE -Wall -Wextra)

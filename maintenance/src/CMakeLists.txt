add_subdirectory(ec)
add_subdirectory(core)
add_subdirectory(sim)

add_executable(healthec_demo main.cpp)
target_link_libraries(healthec_demo PRIVATE healthec_sim healthec_ec)
target_compile_options(healthec_demo PRIVATE -Wall -Wextra)

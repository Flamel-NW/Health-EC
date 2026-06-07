add_executable(test_jerasure_wrapper test_jerasure_wrapper.cpp)

target_link_libraries(test_jerasure_wrapper PRIVATE healthec_ec)

target_compile_options(test_jerasure_wrapper PRIVATE -Wall -Wextra)

add_test(NAME jerasure_wrapper COMMAND test_jerasure_wrapper)

add_executable(test_jerasure_wrapper test_jerasure_wrapper.cpp)

target_link_libraries(test_jerasure_wrapper PRIVATE healthec_ec)

target_compile_options(test_jerasure_wrapper PRIVATE -Wall -Wextra)

add_test(NAME jerasure_wrapper COMMAND test_jerasure_wrapper)

add_executable(test_score_manager test_score_manager.cpp)

target_link_libraries(test_score_manager PRIVATE healthec_core)

target_compile_options(test_score_manager PRIVATE -Wall -Wextra)

add_test(NAME score_manager COMMAND test_score_manager)

add_executable(test_read_scheduler test_read_scheduler.cpp)

target_link_libraries(test_read_scheduler PRIVATE healthec_core)

target_compile_options(test_read_scheduler PRIVATE -Wall -Wextra)

add_test(NAME read_scheduler COMMAND test_read_scheduler)

add_executable(test_disk_simulator test_disk_simulator.cpp)

target_link_libraries(test_disk_simulator PRIVATE healthec_sim)

target_compile_options(test_disk_simulator PRIVATE -Wall -Wextra)

add_test(NAME disk_simulator COMMAND test_disk_simulator)

add_executable(test_migration_scheduler test_migration_scheduler.cpp)

target_link_libraries(test_migration_scheduler PRIVATE healthec_sim)

target_compile_options(test_migration_scheduler PRIVATE -Wall -Wextra)

add_test(NAME migration_scheduler COMMAND test_migration_scheduler)

add_executable(test_integration test_integration.cpp)

target_link_libraries(test_integration PRIVATE healthec_sim healthec_ec)

target_compile_options(test_integration PRIVATE -Wall -Wextra)

add_test(NAME integration COMMAND test_integration)

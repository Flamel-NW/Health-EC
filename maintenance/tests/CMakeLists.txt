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

add_executable(test_loser_significance test_loser_significance.cpp)

target_link_libraries(test_loser_significance PRIVATE healthec_core)

target_compile_options(test_loser_significance PRIVATE -Wall -Wextra)

add_test(NAME loser_significance COMMAND test_loser_significance)

add_executable(test_fixed_target_observer test_fixed_target_observer.cpp)
target_compile_options(test_fixed_target_observer PRIVATE -Wall -Wextra)
add_test(NAME fixed_target_observer COMMAND test_fixed_target_observer)

add_executable(test_maintenance test_maintenance.cpp)
target_include_directories(test_maintenance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../experiments/tools)
target_link_libraries(test_maintenance PRIVATE healthec_sim)
add_library(maintenance_io_fault SHARED io_fault.cpp)
target_link_libraries(maintenance_io_fault PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME maintenance_core COMMAND test_maintenance)
set_tests_properties(maintenance_core PROPERTIES TIMEOUT 30)
add_test(NAME maintenance_io_write COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=$<TARGET_FILE:maintenance_io_fault> HEALTHEC_IO_FAULT=write $<TARGET_FILE:test_maintenance>)
add_test(NAME maintenance_io_close COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=$<TARGET_FILE:maintenance_io_fault> HEALTHEC_IO_FAULT=close $<TARGET_FILE:test_maintenance>)
set_tests_properties(maintenance_io_write maintenance_io_close PROPERTIES TIMEOUT 10)

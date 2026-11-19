add_test(NAME cli_help COMMAND rea_jackcess_cli --help)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: rea-jackcess")

add_test(NAME cli_version COMMAND rea_jackcess_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "rea-jackcess ${PROJECT_VERSION}")

add_executable(rea_jackcess_tests database_file_test.cpp)
target_link_libraries(rea_jackcess_tests PRIVATE rea_jackcess::core)
target_include_directories(rea_jackcess_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME database_file COMMAND rea_jackcess_tests)

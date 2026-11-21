add_test(NAME cli_help COMMAND rea_jackcess_cli --help)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: rea-jackcess")

add_test(NAME cli_version COMMAND rea_jackcess_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "rea-jackcess ${PROJECT_VERSION}")

add_test(NAME cli_create_v2000
    COMMAND rea_jackcess_cli create "${CMAKE_CURRENT_BINARY_DIR}/cli-created-v2000.mdb" v2000)
set_tests_properties(cli_create_v2000 PROPERTIES PASS_REGULAR_EXPRESSION "Created empty Jet4 V2000")

add_test(NAME cli_create_v2003
    COMMAND rea_jackcess_cli create "${CMAKE_CURRENT_BINARY_DIR}/cli-created-v2003.mdb" v2003)
set_tests_properties(cli_create_v2003 PROPERTIES PASS_REGULAR_EXPRESSION "Created empty Jet4 V2003")

add_executable(rea_jackcess_tests database_file_test.cpp)
target_link_libraries(rea_jackcess_tests PRIVATE rea_jackcess::core)
target_include_directories(rea_jackcess_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME database_file COMMAND rea_jackcess_tests)

add_executable(rea_jackcess_page_writer_tests page_writer_test.cpp)
target_link_libraries(rea_jackcess_page_writer_tests PRIVATE rea_jackcess::core)
target_include_directories(rea_jackcess_page_writer_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME page_writer COMMAND rea_jackcess_page_writer_tests)

add_executable(rea_jackcess_database_create_tests database_create_test.cpp)
target_link_libraries(rea_jackcess_database_create_tests PRIVATE rea_jackcess::core)
target_include_directories(rea_jackcess_database_create_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME database_create COMMAND rea_jackcess_database_create_tests)

add_executable(rea_jackcess_catalog_tests catalog_test.cpp)
target_link_libraries(rea_jackcess_catalog_tests PRIVATE rea_jackcess::core)
target_sources(rea_jackcess_catalog_tests PRIVATE "${PROJECT_SOURCE_DIR}/tools/cli/canonical_json.cpp")
target_include_directories(rea_jackcess_catalog_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tools/cli"
)
add_test(NAME catalog COMMAND rea_jackcess_catalog_tests)

# The PGeo sample is intentionally untracked; only register the real-file regression
# when a local copy exists, so the default test suite stays self-contained.
set(REA_JACKCESS_PGEO_SAMPLE "${PROJECT_SOURCE_DIR}/test_data_only_read/test_mdb.mdb")
if(EXISTS "${REA_JACKCESS_PGEO_SAMPLE}")
    add_executable(rea_jackcess_real_sample_tests real_sample_test.cpp)
    target_link_libraries(rea_jackcess_real_sample_tests PRIVATE rea_jackcess::core)
    target_include_directories(rea_jackcess_real_sample_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    add_test(NAME real_sample_boolean
        COMMAND rea_jackcess_real_sample_tests "${REA_JACKCESS_PGEO_SAMPLE}")
endif()

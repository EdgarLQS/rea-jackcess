add_test(NAME cli_help COMMAND rea_jackcess_cli --help)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: rea-jackcess")

add_test(NAME cli_version COMMAND rea_jackcess_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "rea-jackcess ${PROJECT_VERSION}")

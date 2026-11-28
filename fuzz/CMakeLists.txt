if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "REA_JACKCESS_BUILD_FUZZERS requires Clang with libFuzzer support")
endif()

add_executable(rea_jackcess_fuzz_open EXCLUDE_FROM_ALL open_database_fuzz.cpp)
target_link_libraries(rea_jackcess_fuzz_open PRIVATE rea_jackcess::core)
target_compile_options(rea_jackcess_fuzz_open PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -g
)
target_link_options(rea_jackcess_fuzz_open PRIVATE
    -fsanitize=fuzzer,address,undefined
)

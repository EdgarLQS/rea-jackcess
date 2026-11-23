cmake_minimum_required(VERSION 3.16)
project(rea_jackcess VERSION 0.0.0 LANGUAGES CXX)

find_package(ZLIB REQUIRED)

add_subdirectory(src)

add_executable(rea_jackcess_cli tools/cli/main.cpp)
target_sources(rea_jackcess_cli PRIVATE tools/cli/canonical_json.cpp)
set_target_properties(rea_jackcess_cli PROPERTIES OUTPUT_NAME rea-jackcess CXX_EXTENSIONS OFF)
target_include_directories(rea_jackcess_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rea_jackcess_cli PRIVATE rea_jackcess::core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

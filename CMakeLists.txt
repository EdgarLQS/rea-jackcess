cmake_minimum_required(VERSION 3.16)
project(rea_jackcess VERSION 0.0.0 LANGUAGES CXX)

# Pin the declared standard so a newer compiler default (for example C++20 with char8_t
# `path::u8string()`) cannot change what the sources mean.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)

add_subdirectory(src)
add_subdirectory(adapters/pgeo)

add_executable(rea_jackcess_cli tools/cli/main.cpp)
target_sources(rea_jackcess_cli PRIVATE tools/cli/canonical_json.cpp)
set_target_properties(rea_jackcess_cli PROPERTIES OUTPUT_NAME rea-jackcess CXX_EXTENSIONS OFF)
target_include_directories(rea_jackcess_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rea_jackcess_cli PRIVATE rea_jackcess::core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

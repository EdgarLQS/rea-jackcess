cmake_minimum_required(VERSION 3.16)
project(rea_jackcess VERSION 0.0.0 LANGUAGES CXX)

add_library(rea_jackcess STATIC
    src/database.cpp
    src/database_file.cpp
    src/catalog/system_catalog.cpp
    src/catalog/row_reader.cpp
    src/catalog/table_catalog.cpp
    src/catalog/table_definition.cpp
    src/catalog/usage_map.cpp
    src/format/jet_format.cpp
    src/page/data_page.cpp
    src/page/page_file.cpp
    src/value/text_codec.cpp
    src/value/jet4_value.cpp
    src/util/sha256.cpp
    src/version.cpp
)
add_library(rea_jackcess::core ALIAS rea_jackcess)
target_compile_features(rea_jackcess PUBLIC cxx_std_17)
set_target_properties(rea_jackcess PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(rea_jackcess PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(rea_jackcess PRIVATE REA_JACKCESS_VERSION="${PROJECT_VERSION}")

add_executable(rea_jackcess_cli tools/cli/main.cpp)
target_sources(rea_jackcess_cli PRIVATE tools/cli/canonical_json.cpp)
set_target_properties(rea_jackcess_cli PROPERTIES OUTPUT_NAME rea-jackcess CXX_EXTENSIONS OFF)
target_include_directories(rea_jackcess_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rea_jackcess_cli PRIVATE rea_jackcess::core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

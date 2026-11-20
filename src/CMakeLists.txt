add_library(rea_jackcess STATIC)
add_library(rea_jackcess::core ALIAS rea_jackcess)
target_compile_features(rea_jackcess PUBLIC cxx_std_17)
set_target_properties(rea_jackcess PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    CXX_EXTENSIONS OFF
)
target_include_directories(rea_jackcess PUBLIC "${PROJECT_SOURCE_DIR}/include")

function(rea_jackcess_add_package package)
    set(target "rea_jackcess_${package}_objects")
    add_library(${target} OBJECT ${ARGN})
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_sources(rea_jackcess PRIVATE "$<TARGET_OBJECTS:${target}>")
endfunction()

rea_jackcess_add_package(api
    api/database.cpp
    api/database_file.cpp
    api/version.cpp
)
target_compile_definitions(rea_jackcess_api_objects PRIVATE
    REA_JACKCESS_VERSION="${PROJECT_VERSION}"
)

rea_jackcess_add_package(format
    format/jet_format.cpp
)

rea_jackcess_add_package(page
    page/data_page.cpp
    page/page_file.cpp
)

rea_jackcess_add_package(catalog
    catalog/property_map.cpp
    catalog/row_reader.cpp
    catalog/system_catalog.cpp
    catalog/table_catalog.cpp
    catalog/table_definition.cpp
    catalog/usage_map.cpp
)

rea_jackcess_add_package(value
    value/jet4_value.cpp
    value/text_codec.cpp
)

rea_jackcess_add_package(index
    index/index_cursor.cpp
)

rea_jackcess_add_package(util
    util/sha256.cpp
)

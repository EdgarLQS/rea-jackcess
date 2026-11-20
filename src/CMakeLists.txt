add_library(rea_jackcess STATIC)
add_library(rea_jackcess::core ALIAS rea_jackcess)
target_compile_features(rea_jackcess PUBLIC cxx_std_17)
set_target_properties(rea_jackcess PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    CXX_EXTENSIONS OFF
)
target_include_directories(rea_jackcess PUBLIC "${PROJECT_SOURCE_DIR}/include")

set(rea_jackcess_template_dir "${PROJECT_SOURCE_DIR}/third_party/jackcess-5.2.0")
set(rea_jackcess_v2000_template "${rea_jackcess_template_dir}/empty-v2000.mdb")
set(rea_jackcess_v2003_template "${rea_jackcess_template_dir}/empty-v2003.mdb")
set(rea_jackcess_template_source "${PROJECT_BINARY_DIR}/generated/jet4_template_data.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${rea_jackcess_v2000_template}"
    "${rea_jackcess_v2003_template}"
)
file(SHA256 "${rea_jackcess_v2000_template}" rea_jackcess_v2000_sha256)
file(SHA256 "${rea_jackcess_v2003_template}" rea_jackcess_v2003_sha256)
if(NOT rea_jackcess_v2000_sha256 STREQUAL "521c6f8a60f94273866367f69c772eb590fc7fcb8bc91bcb134d567b1e9b6566")
    message(FATAL_ERROR "The pinned Jackcess V2000 template has an unexpected SHA-256")
endif()
if(NOT rea_jackcess_v2003_sha256 STREQUAL "c7dc939c0afb5706d209ea54cfd1dfc7e67996767a0ed357c0fd981b9012c955")
    message(FATAL_ERROR "The pinned Jackcess V2003 template has an unexpected SHA-256")
endif()
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
file(READ "${rea_jackcess_v2000_template}" rea_jackcess_v2000_hex HEX)
file(READ "${rea_jackcess_v2003_template}" rea_jackcess_v2003_hex HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," rea_jackcess_v2000_bytes "${rea_jackcess_v2000_hex}")
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," rea_jackcess_v2003_bytes "${rea_jackcess_v2003_hex}")
file(WRITE "${rea_jackcess_template_source}"
    "#include \"write/jet4_template_data.hpp\"\n\n"
    "namespace rea_jackcess::detail {\nnamespace {\n"
    "constexpr std::uint8_t v2000_bytes[] = {${rea_jackcess_v2000_bytes}};\n"
    "constexpr std::uint8_t v2003_bytes[] = {${rea_jackcess_v2003_bytes}};\n"
    "}\n\n"
    "Jet4TemplateView jet4_template(DatabaseFormat format) noexcept {\n"
    "    switch (format) {\n"
    "        case DatabaseFormat::v2000: return {v2000_bytes, sizeof(v2000_bytes)};\n"
    "        case DatabaseFormat::v2003: return {v2003_bytes, sizeof(v2003_bytes)};\n"
    "    }\n"
    "    return {nullptr, 0};\n"
    "}\n\n"
    "}\n"
)

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
    api/table.cpp
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
    page/page_file_write_lock.cpp
    page/page_writer.cpp
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

rea_jackcess_add_package(write
    write/jet4_byte_order.hpp
    write/jet4_database_creator.cpp
    write/jet4_database_creator.hpp
    write/jet4_global_usage_map.cpp
    write/jet4_global_usage_map.hpp
    write/jet4_row_writer.cpp
    write/jet4_row_writer.hpp
    write/jet4_table_creator.cpp
    write/jet4_table_creator.hpp
    write/jet4_template_data.hpp
    "${rea_jackcess_template_source}"
)

rea_jackcess_add_package(util
    util/sha256.cpp
)

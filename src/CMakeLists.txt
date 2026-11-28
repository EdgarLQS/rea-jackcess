add_library(rea_jackcess STATIC)
add_library(rea_jackcess::core ALIAS rea_jackcess)
target_link_libraries(rea_jackcess PUBLIC ZLIB::ZLIB)
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
    api/complex_values.cpp
    api/database.cpp
    api/ole_stream.cpp
    api/database_file.cpp
    api/relationships.cpp
    api/queries.cpp
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
    page/write_journal.cpp
)

rea_jackcess_add_package(catalog
    catalog/complex_column.cpp
    catalog/property_map.cpp
    catalog/row_reader.cpp
    catalog/system_catalog.cpp
    catalog/table_catalog.cpp
    catalog/table_definition.cpp
    catalog/jet3_table_definition.cpp
    catalog/usage_map.cpp
)

rea_jackcess_add_package(value
    value/attachment_value.cpp
    value/jet4_value.cpp
    value/jet3_value.cpp
    value/text_codec.cpp
    value/unicode_case.cpp
)
target_link_libraries(rea_jackcess_value_objects PRIVATE ZLIB::ZLIB)

set(rea_jackcess_basic_weights "${rea_jackcess_template_dir}/index_codes_genleg.txt")
set(rea_jackcess_extended_weights "${rea_jackcess_template_dir}/index_codes_ext_genleg.txt")
set(rea_jackcess_general_basic_weights "${rea_jackcess_template_dir}/index_codes_gen.txt")
set(rea_jackcess_general_extended_weights "${rea_jackcess_template_dir}/index_codes_ext_gen.txt")
set(rea_jackcess_general97_basic_weights "${rea_jackcess_template_dir}/index_codes_gen_97.txt")
set(rea_jackcess_general97_extended_mappings "${rea_jackcess_template_dir}/index_mappings_ext_gen_97.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${rea_jackcess_basic_weights}" "${rea_jackcess_extended_weights}"
    "${rea_jackcess_general_basic_weights}" "${rea_jackcess_general_extended_weights}"
    "${rea_jackcess_general97_basic_weights}" "${rea_jackcess_general97_extended_mappings}")
# The pinned tables are hashed and embedded with LF line endings, so a work tree checked out with
# core.autocrlf=true produces the same hashes and the same generated string literals.
function(rea_jackcess_read_pinned_text path content_var hash_var)
    file(READ "${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(SHA256 hash "${content}")
    set(${content_var} "${content}" PARENT_SCOPE)
    set(${hash_var} "${hash}" PARENT_SCOPE)
endfunction()

rea_jackcess_read_pinned_text("${rea_jackcess_basic_weights}"
    rea_jackcess_basic_weight_data rea_jackcess_basic_weights_sha256)
rea_jackcess_read_pinned_text("${rea_jackcess_extended_weights}"
    rea_jackcess_extended_weight_data rea_jackcess_extended_weights_sha256)
rea_jackcess_read_pinned_text("${rea_jackcess_general_basic_weights}"
    rea_jackcess_general_basic_weight_data rea_jackcess_general_basic_weights_sha256)
rea_jackcess_read_pinned_text("${rea_jackcess_general_extended_weights}"
    rea_jackcess_general_extended_weight_data rea_jackcess_general_extended_weights_sha256)
rea_jackcess_read_pinned_text("${rea_jackcess_general97_basic_weights}"
    rea_jackcess_general97_basic_weight_data rea_jackcess_general97_basic_weights_sha256)
rea_jackcess_read_pinned_text("${rea_jackcess_general97_extended_mappings}"
    rea_jackcess_general97_extended_mapping_data rea_jackcess_general97_extended_mappings_sha256)
if(NOT rea_jackcess_basic_weights_sha256 STREQUAL "acc97846553560c0b2cdde822c990cea2a2bea733ca071e850e180b9ce3cd9ee" OR
   NOT rea_jackcess_extended_weights_sha256 STREQUAL "d60378601aebc22f579b8c5dd35a4c0d3e77c371275372f25511d73fe3e92ae7" OR
   NOT rea_jackcess_general_basic_weights_sha256 STREQUAL "a0554f81d328732d3d6fcac5d44476130efeaccb96bdc1eda30590dd71ecb8b5" OR
   NOT rea_jackcess_general_extended_weights_sha256 STREQUAL "0f54f68c0592ba5066d505cb3b6782e64edb344da6a0a24959277dea9a478c28" OR
   NOT rea_jackcess_general97_basic_weights_sha256 STREQUAL "ad74d251bd8cf441976446e50d84bc7366c28e0ba2c970ada1b52904d816bad9" OR
   NOT rea_jackcess_general97_extended_mappings_sha256 STREQUAL "60cb99d942c69c778bc4f2162af54168b6b3d631db502bce5d4b3a318883935c")
    message(FATAL_ERROR "A pinned Jackcess text-index weight table has an unexpected SHA-256")
endif()
file(WRITE "${PROJECT_BINARY_DIR}/generated/jet4_text_index_weights.inc"
    "constexpr char basic_weights[] = R\"weights(${rea_jackcess_basic_weight_data})weights\";\n"
    "constexpr char extended_weights[] = R\"weights(${rea_jackcess_extended_weight_data})weights\";\n"
    "constexpr char general_basic_weights[] = R\"weights(${rea_jackcess_general_basic_weight_data})weights\";\n"
    "constexpr char general_extended_weights[] = R\"weights(${rea_jackcess_general_extended_weight_data})weights\";\n"
    "constexpr char general97_basic_weights[] = R\"weights(${rea_jackcess_general97_basic_weight_data})weights\";\n"
    "constexpr char general97_mapping_data[] = R\"weights(${rea_jackcess_general97_extended_mapping_data})weights\";\n")

rea_jackcess_add_package(index
    index/index_cursor.cpp
    index/jet4_text_key.cpp
)
target_include_directories(rea_jackcess_index_objects PRIVATE "${PROJECT_BINARY_DIR}/generated")

rea_jackcess_add_package(io
    io/csv.cpp
)

rea_jackcess_add_package(write
    write/jet4_database_creator.cpp
    write/jet4_database_creator.hpp
    write/jet4_global_usage_map.cpp
    write/jet4_index_entry.cpp
    write/jet4_index_entry.hpp
    write/jet4_global_usage_map.hpp
    write/jet4_relationship_creator.cpp
    write/jet4_relationship_creator.hpp
    write/jet4_row_writer.cpp
    write/jet4_row_writer.hpp
    write/jet4_variable_type.hpp
    write/jet4_table_creator.cpp
    write/jet4_table_creator.hpp
    write/jet4_template_data.hpp
    "${rea_jackcess_template_source}"
)

rea_jackcess_add_package(util
    util/little_endian.hpp
    util/sha256.cpp
    util/code_page.cpp
)

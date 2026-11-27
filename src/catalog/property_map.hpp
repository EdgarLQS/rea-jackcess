#pragma once

#include "../page/page_file.hpp"
#include "rea_jackcess/property.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

// Decodes the property blocks contained in `property_bytes` (an object's MSysObjects.LvProp value).
// `object_name`, `page_number` and `system_row_number` are used only for error context.
std::vector<PropertyMapInfo> read_property_maps(const PageFile& file,
                                                const std::string& object_name,
                                                std::uint64_t page_number,
                                                std::uint16_t system_row_number,
                                                const std::vector<std::uint8_t>& property_bytes);

// Serializes raw payloads and flags for an MSysObjects.LvProp update.
std::vector<std::uint8_t> encode_property_maps(const std::vector<PropertyMapInfo>& maps);

// Fills `PropertyInfo::value` for each property in `maps` by decoding its raw_value/type_code.
void decode_property_values(const PageFile& file,
                            const std::string& object_name,
                            std::uint64_t page_number,
                            std::uint16_t system_row_number,
                            std::vector<PropertyMapInfo>& maps);

// For each named column, returns the value of the named text property (e.g. "DefaultValue"),
// or nullopt when the column does not declare it.
std::vector<std::optional<std::string>> read_column_text_properties(
    const PageFile& file, const std::string& table_name, std::uint64_t page_number,
    std::uint16_t system_row_number, const std::vector<std::string>& column_names,
    const std::vector<std::uint8_t>& property_bytes, std::string_view property_name);

// For each named column, returns whether it is nullable (from its Required property); a column
// whose Required property is absent is reported as nullable.
std::vector<bool> read_column_nullability(const PageFile& file,
                                         const std::string& table_name,
                                         std::uint64_t page_number,
                                         std::uint16_t system_row_number,
                                         const std::vector<std::string>& column_names,
                                         const std::vector<std::uint8_t>& property_bytes);

// For each named column, returns the stored result type of a calculated column (nullopt when the
// column is not calculated or the property is absent).
std::vector<std::optional<std::uint8_t>> read_calculated_column_result_types(
    const PageFile& file, const std::string& table_name, std::uint64_t page_number,
    std::uint16_t system_row_number, const std::vector<std::string>& column_names,
    const std::vector<std::uint8_t>& property_bytes);

// Returns the value of one named text property for an object, or nullopt when it is absent.
std::optional<std::string> read_text_property(const PageFile& file,
                                              const std::string& object_name,
                                              std::uint64_t page_number,
                                              std::uint16_t system_row_number,
                                              const std::vector<std::uint8_t>& property_bytes,
                                              std::string_view property_name);

}

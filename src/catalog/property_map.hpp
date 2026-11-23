#pragma once

#include "../page/page_file.hpp"
#include "rea_jackcess/property.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

std::vector<PropertyMapInfo> read_property_maps(const PageFile& file,
                                                const std::string& object_name,
                                                std::uint64_t page_number,
                                                std::uint16_t system_row_number,
                                                const std::vector<std::uint8_t>& property_bytes);

// Serializes raw payloads and flags for an MSysObjects.LvProp update.
std::vector<std::uint8_t> encode_property_maps(const std::vector<PropertyMapInfo>& maps);

void decode_property_values(const PageFile& file,
                            const std::string& object_name,
                            std::uint64_t page_number,
                            std::uint16_t system_row_number,
                            std::vector<PropertyMapInfo>& maps);

std::vector<std::optional<std::string>> read_column_text_properties(
    const PageFile& file, const std::string& table_name, std::uint64_t page_number,
    std::uint16_t system_row_number, const std::vector<std::string>& column_names,
    const std::vector<std::uint8_t>& property_bytes, std::string_view property_name);

std::vector<bool> read_column_nullability(const PageFile& file,
                                         const std::string& table_name,
                                         std::uint64_t page_number,
                                         std::uint16_t system_row_number,
                                         const std::vector<std::string>& column_names,
                                         const std::vector<std::uint8_t>& property_bytes);

std::vector<std::optional<std::uint8_t>> read_calculated_column_result_types(
    const PageFile& file, const std::string& table_name, std::uint64_t page_number,
    std::uint16_t system_row_number, const std::vector<std::string>& column_names,
    const std::vector<std::uint8_t>& property_bytes);

std::optional<std::string> read_text_property(const PageFile& file,
                                              const std::string& object_name,
                                              std::uint64_t page_number,
                                              std::uint16_t system_row_number,
                                              const std::vector<std::uint8_t>& property_bytes,
                                              std::string_view property_name);

}

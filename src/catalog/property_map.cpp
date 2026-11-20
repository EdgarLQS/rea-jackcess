#include "property_map.hpp"

#include "../value/text_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint16_t property_name_list = 0x80;
constexpr std::uint16_t column_property_value_list = 0x01;
constexpr std::uint8_t boolean_type = 0x01;

[[noreturn]] void invalid_property_map(const PageFile& file,
                                       const std::string& table_name,
                                       std::uint64_t page_number,
                                       std::uint16_t system_row_number,
                                       const char* message) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    context.row_number = system_row_number;
    context.column_name = "LvProp";
    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                        message,
                        file.path(),
                        page_number,
                        std::move(context));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool equals_ignore_ascii_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

std::string read_property_name(const PageFile& file,
                               const std::string& table_name,
                               std::uint64_t page_number,
                               std::uint16_t system_row_number,
                               const std::vector<std::uint8_t>& bytes,
                               std::size_t& offset,
                               std::size_t end) {
    if (offset > end || end - offset < 2) {
        invalid_property_map(file, table_name, page_number, system_row_number,
                             "Property map name length is truncated");
    }
    const std::uint16_t length = read_u16(bytes, offset);
    offset += 2;
    if (length > end - offset || length % 2 != 0) {
        invalid_property_map(file, table_name, page_number, system_row_number,
                             "Property map name has an invalid length");
    }

    std::vector<std::uint8_t> encoded(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    return decode_utf16le(encoded);
}

std::optional<std::size_t> find_column_index(const std::vector<std::string>& column_names,
                                             std::string_view name) {
    for (std::size_t index = 0; index < column_names.size(); ++index) {
        if (column_names[index] == name || equals_ignore_ascii_case(column_names[index], name)) {
            return index;
        }
    }
    return std::nullopt;
}

}

std::vector<bool> read_column_nullability(const PageFile& file,
                                         const std::string& table_name,
                                         std::uint64_t page_number,
                                         std::uint16_t system_row_number,
                                         const std::vector<std::string>& column_names,
                                         const std::vector<std::uint8_t>& property_bytes) {
    std::vector<bool> nullable(column_names.size(), true);
    if (property_bytes.empty()) {
        return nullable;
    }
    if (property_bytes.size() < 4) {
        invalid_property_map(file, table_name, page_number, system_row_number,
                             "Property map header is truncated");
    }

    const bool unicode_names = property_bytes[0] == 'M' && property_bytes[1] == 'R' &&
                               property_bytes[2] == '2' && property_bytes[3] == 0;
    if (!unicode_names) {
        invalid_property_map(file, table_name, page_number, system_row_number,
                             "Property map header is not recognized");
    }

    std::vector<std::string> property_names;
    std::size_t offset = 4;
    while (offset < property_bytes.size()) {
        if (property_bytes.size() - offset < 6) {
            invalid_property_map(file, table_name, page_number, system_row_number,
                                 "Property map block header is truncated");
        }
        const std::uint32_t block_length = read_u32(property_bytes, offset);
        const std::uint16_t block_type = read_u16(property_bytes, offset + 4);
        if (block_length < 6 || block_length > property_bytes.size() - offset) {
            invalid_property_map(file, table_name, page_number, system_row_number,
                                 "Property map block has an invalid length");
        }
        const std::size_t block_end = offset + block_length;
        std::size_t block_offset = offset + 6;

        if (block_type == property_name_list) {
            property_names.clear();
            while (block_offset < block_end) {
                property_names.push_back(read_property_name(file,
                                                            table_name,
                                                            page_number,
                                                            system_row_number,
                                                            property_bytes,
                                                            block_offset,
                                                            block_end));
            }
        } else {
            if (block_offset < block_end) {
                if (block_end - block_offset < 4) {
                    invalid_property_map(file, table_name, page_number, system_row_number,
                                         "Property map name block is truncated");
                }
                const std::uint32_t map_name_length = read_u32(property_bytes, block_offset);
                if (map_name_length < 6 || map_name_length > block_end - block_offset) {
                    invalid_property_map(file, table_name, page_number, system_row_number,
                                         "Property map name block has an invalid length");
                }
                const std::size_t map_name_end = block_offset + map_name_length;
                std::size_t name_offset = block_offset + 4;
                const std::string map_name = read_property_name(file,
                                                               table_name,
                                                               page_number,
                                                               system_row_number,
                                                               property_bytes,
                                                               name_offset,
                                                               map_name_end);
                if (name_offset != map_name_end) {
                    invalid_property_map(file, table_name, page_number, system_row_number,
                                         "Property map name block contains trailing bytes");
                }
                block_offset = map_name_end;

                const auto column_index = block_type == column_property_value_list
                                              ? find_column_index(column_names, map_name)
                                              : std::nullopt;
                while (block_offset < block_end) {
                    if (block_end - block_offset < 8) {
                        invalid_property_map(file, table_name, page_number, system_row_number,
                                             "Property map value is truncated");
                    }
                    const std::uint16_t value_length = read_u16(property_bytes, block_offset);
                    if (value_length < 8 || value_length > block_end - block_offset) {
                        invalid_property_map(file, table_name, page_number, system_row_number,
                                             "Property map value has an invalid length");
                    }
                    const std::size_t value_end = block_offset + value_length;
                    const std::uint8_t type = property_bytes[block_offset + 3];
                    const std::uint16_t name_index = read_u16(property_bytes, block_offset + 4);
                    const std::uint16_t data_size = read_u16(property_bytes, block_offset + 6);
                    const std::size_t value_data = block_offset + 8;
                    if (data_size > value_end - value_data) {
                        invalid_property_map(file, table_name, page_number, system_row_number,
                                             "Property map value data is truncated");
                    }
                    if (name_index >= property_names.size()) {
                        invalid_property_map(file, table_name, page_number, system_row_number,
                                             "Property map refers to an unknown property name");
                    }
                    if (column_index && equals_ignore_ascii_case(property_names[name_index], "Required")) {
                        if (type != boolean_type || data_size != 1 || value_length != 9) {
                            invalid_property_map(file, table_name, page_number, system_row_number,
                                                 "Column Required property is not a one-byte Boolean");
                        }
                        nullable[*column_index] = property_bytes[value_data] == 0;
                    }
                    block_offset = value_end;
                }
            }
        }
        offset = block_end;
    }
    return nullable;
}

}

#include "property_map.hpp"

#include "../util/little_endian.hpp"
#include "../value/jet4_value.hpp"
#include "../value/text_codec.hpp"
#include "../value/unicode_case.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
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

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return java_string_uppercase_key_equal_utf8(left, right);
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
        if (column_names[index] == name || equals_ignore_case(column_names[index], name)) {
            return index;
        }
    }
    return std::nullopt;
}

}

std::vector<PropertyMapInfo> read_property_maps(const PageFile& file,
                                         const std::string& table_name,
                                         std::uint64_t page_number,
                                         std::uint16_t system_row_number,
                                         const std::vector<std::uint8_t>& property_bytes) {
    std::vector<PropertyMapInfo> maps;
    if (property_bytes.empty()) {
        return maps;
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

                PropertyMapInfo map{map_name, block_type, {}};
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
                    const auto data_begin = property_bytes.begin() + static_cast<std::ptrdiff_t>(value_data);
                    const auto data_end = data_begin + data_size;
                    map.properties.push_back({
                        property_names[name_index], type, property_bytes[block_offset + 2],
                        {data_begin, data_end},
                        {data_end, property_bytes.begin() + static_cast<std::ptrdiff_t>(value_end)}, {}});
                    block_offset = value_end;
                }
                maps.push_back(std::move(map));
            }
        }
        offset = block_end;
    }
    return maps;
}

std::vector<std::uint8_t> encode_property_maps(const std::vector<PropertyMapInfo>& maps) {
    if (maps.empty()) {
        return {};
    }

    constexpr auto max_short = static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max());
    constexpr auto max_block = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    std::vector<std::string> names;
    std::map<std::string, std::uint16_t> name_indexes;
    for (const auto& map : maps) {
        if (map.type_code == property_name_list) {
            throw std::invalid_argument("A property map cannot use the name-list block type");
        }
        for (const auto& property : map.properties) {
            if (property.name.empty()) {
                throw std::invalid_argument("A property name cannot be empty");
            }
            if (name_indexes.count(property.name) == 0) {
                if (names.size() > max_short) {
                    throw std::invalid_argument("Too many property names for a Jet4 name list");
                }
                name_indexes.emplace(property.name, static_cast<std::uint16_t>(names.size()));
                names.push_back(property.name);
            }
        }
    }

    std::vector<std::uint8_t> bytes{'M', 'R', '2', 0};
    const auto append_u16 = [&](std::uint16_t value) {
        const auto offset = bytes.size();
        bytes.resize(offset + 2);
        write_u16(bytes, offset, value);
    };
    const auto append_name = [&](const std::string& name) {
        const auto units = utf8_to_utf16(name, max_short / 2, "Property name exceeds Jet4 limits");
        append_u16(static_cast<std::uint16_t>(units.size() * 2));
        for (const auto unit : units) {
            append_u16(unit);
        }
    };
    const auto begin_block = [&](std::uint16_t type) {
        const auto start = bytes.size();
        bytes.resize(start + 4);
        append_u16(type);
        return start;
    };
    const auto finish_length = [&](std::size_t start) {
        const auto size = bytes.size() - start;
        if (size > max_block) {
            throw std::invalid_argument("Property block exceeds Jet4 size limits");
        }
        write_u32(bytes, start, static_cast<std::uint32_t>(size));
    };

    const auto names_block = begin_block(property_name_list);
    for (const auto& name : names) {
        append_name(name);
    }
    finish_length(names_block);
    for (const auto& map : maps) {
        const auto block = begin_block(map.type_code);
        const auto map_name = bytes.size();
        bytes.resize(map_name + 4);
        append_name(map.name);
        finish_length(map_name);
        for (const auto& property : map.properties) {
            if (property.value_is_null) {
                continue;
            }
            constexpr std::size_t record_header_size = 8;
            if (property.raw_value.size() > max_short - record_header_size ||
                property.trailing_bytes.size() > max_short - record_header_size - property.raw_value.size()) {
                throw std::invalid_argument("Property value exceeds Jet4 record size limits");
            }
            append_u16(static_cast<std::uint16_t>(record_header_size + property.raw_value.size() +
                                                  property.trailing_bytes.size()));
            bytes.push_back(property.flags);
            bytes.push_back(property.type_code);
            append_u16(name_indexes.at(property.name));
            append_u16(static_cast<std::uint16_t>(property.raw_value.size()));
            bytes.insert(bytes.end(), property.raw_value.begin(), property.raw_value.end());
            bytes.insert(bytes.end(), property.trailing_bytes.begin(), property.trailing_bytes.end());
        }
        finish_length(block);
    }
    return bytes;
}

void decode_property_values(const PageFile& file,
                            const std::string& object_name,
                            std::uint64_t page_number,
                            std::uint16_t system_row_number,
                            std::vector<PropertyMapInfo>& maps) {
    constexpr std::uint8_t binary = 0x09;
    constexpr std::uint8_t text = 0x0a;
    constexpr std::uint8_t ole = 0x0b;
    constexpr std::uint8_t memo = 0x0c;
    constexpr std::uint8_t guid = 0x0f;
    for (auto& map : maps) {
        for (auto& property : map.properties) {
            if (property.type_code == boolean_type) {
                if (property.raw_value.empty()) {
                    property.value = UnsupportedValue{"Boolean property has no value byte"};
                    continue;
                }
                property.value = property.raw_value.front() != 0;
                continue;
            }

            ColumnDefinition column{};
            column.name = property.name;
            column.type = property.type_code;
            switch (column.type) {
                case memo:
                    column.type = text;
                    break;
                case ole:
                    column.type = binary;
                    break;
                case binary:
                    if (property.raw_value.size() == 16 && equals_ignore_case(property.name, "GUID")) {
                        column.type = guid;
                    }
                    break;
                default:
                    break;
            }
            // Jackcess reads the first scalar from the property buffer. Some template
            // properties contain extra payload bytes, which remain in raw_value.
            constexpr std::size_t scalar_sizes[]{0, 1, 1, 2, 4, 8, 4, 8, 8};
            auto size = property.raw_value.size();
            if (column.type >= 2 && column.type <= 8) {
                size = std::min(size, scalar_sizes[column.type]);
            }
            std::vector<std::uint8_t> bytes(property.raw_value.begin(), property.raw_value.begin() + size);
            EncodedTableRow row{page_number, system_row_number, {{false, std::move(bytes)}}};
            try {
                property.value = decode_jet4_value(file, object_name, row, column);
            } catch (const DatabaseError& error) {
                if (error.code() != DatabaseErrorCode::corrupted_file) {
                    throw;
                }
                property.value = UnsupportedValue{error.what()};
            }
        }
    }
}

std::vector<std::optional<std::string>> read_column_text_properties(
    const PageFile& file, const std::string& table_name, std::uint64_t page_number,
    std::uint16_t system_row_number, const std::vector<std::string>& column_names,
    const std::vector<std::uint8_t>& property_bytes, std::string_view property_name) {
    std::vector<std::optional<std::string>> defaults(column_names.size());
    for (const auto& map : read_property_maps(file, table_name, page_number, system_row_number, property_bytes)) {
        if (map.type_code != column_property_value_list) {
            continue;
        }
        const auto index = find_column_index(column_names, map.name);
        if (!index) {
            continue;
        }
        if (const auto* property = map.find(property_name)) {
            if (property->type_code == 0x0a || property->type_code == 0x0c) {
                defaults[*index] = decode_jet4_text(property->raw_value);
            }
        }
    }
    return defaults;
}

std::vector<bool> read_column_nullability(const PageFile& file,
                                         const std::string& table_name,
                                         std::uint64_t page_number,
                                         std::uint16_t system_row_number,
                                         const std::vector<std::string>& column_names,
                                         const std::vector<std::uint8_t>& property_bytes) {
    std::vector<bool> nullable(column_names.size(), true);
    for (const auto& map : read_property_maps(file, table_name, page_number, system_row_number, property_bytes)) {
        if (map.type_code != column_property_value_list) {
            continue;
        }
        const auto index = find_column_index(column_names, map.name);
        if (!index) {
            continue;
        }
        for (const auto& property : map.properties) {
            if (equals_ignore_case(property.name, "Required")) {
                if (property.type_code != boolean_type || property.raw_value.size() != 1 ||
                    !property.trailing_bytes.empty()) {
                    invalid_property_map(file, table_name, page_number, system_row_number,
                                         "Column Required property is not a one-byte Boolean");
                }
                nullable[*index] = property.raw_value.front() == 0;
            }
        }
    }
    return nullable;
}

std::vector<std::optional<std::uint8_t>> read_calculated_column_result_types(
    const PageFile& file,
    const std::string& table_name,
    std::uint64_t page_number,
    std::uint16_t system_row_number,
    const std::vector<std::string>& column_names,
    const std::vector<std::uint8_t>& property_bytes) {
    std::vector<std::optional<std::uint8_t>> result_types(column_names.size());
    for (const auto& map : read_property_maps(file, table_name, page_number, system_row_number, property_bytes)) {
        if (map.type_code != column_property_value_list) {
            continue;
        }
        const auto index = find_column_index(column_names, map.name);
        if (!index) {
            continue;
        }
        for (const auto& property : map.properties) {
            if (!equals_ignore_case(property.name, "ResultType")) {
                continue;
            }
            if (result_types[*index] || property.type_code != 0x02 || property.raw_value.size() != 1 ||
                !property.trailing_bytes.empty()) {
                invalid_property_map(file, table_name, page_number, system_row_number,
                                     "Calculated column ResultType property is invalid");
            }
            result_types[*index] = property.raw_value.front();
        }
    }
    return result_types;
}

std::optional<std::string> read_text_property(const PageFile& file,
                                              const std::string& object_name,
                                              std::uint64_t page_number,
                                              std::uint16_t system_row_number,
                                              const std::vector<std::uint8_t>& property_bytes,
                                              std::string_view property_name) {
    std::optional<std::string> result;
    for (const auto& map : read_property_maps(file, object_name, page_number, system_row_number, property_bytes)) {
        if (map.type_code != 0 || !map.name.empty()) {
            continue;
        }
        for (const auto& property : map.properties) {
            if (equals_ignore_case(property.name, property_name)) {
                if (result || property.type_code != 0x0a || property.raw_value.size() % 2 != 0 ||
                    !property.trailing_bytes.empty()) {
                    invalid_property_map(file, object_name, page_number, system_row_number,
                                         "Text property has an invalid type, size, or duplicate value");
                }
                result = decode_utf16le(property.raw_value);
            }
        }
    }
    return result;
}

}

namespace rea_jackcess {
namespace {

struct PropertyTypeDefinition {
    std::uint8_t type_code;
    bool ddl;
};

std::optional<PropertyTypeDefinition> built_in_property_type(std::string_view name) {
    if (name == "AccessVersion" || name == "Title" || name == "Author" || name == "Company") {
        return PropertyTypeDefinition{0x0a, false};
    }
    if (name == "DefaultValue" || name == "Expression") {
        return PropertyTypeDefinition{0x0c, true};
    }
    if (name == "Required" || name == "AllowZeroLength") {
        return PropertyTypeDefinition{0x01, true};
    }
    if (name == "DecimalPlaces" || name == "ResultType") {
        return PropertyTypeDefinition{0x02, true};
    }
    if (name == "Format" || name == "InputMask" || name == "ValidationRule" ||
        name == "ValidationText") {
        return PropertyTypeDefinition{0x0a, true};
    }
    if (name == "Caption" || name == "Description") {
        return PropertyTypeDefinition{0x0c, false};
    }
    if (name == "GUID") {
        return PropertyTypeDefinition{0x09, true};
    }
    if (name == "DisplayControl") {
        return PropertyTypeDefinition{0x03, false};
    }
    if (name == "TextFormat" || name == "IMEMode" || name == "IMESentenceMode") {
        return PropertyTypeDefinition{0x02, false};
    }
    return std::nullopt;
}

std::optional<std::uint8_t> inferred_property_type(const Value& value) {
    return std::visit([](const auto& item) -> std::optional<std::uint8_t> {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, bool>) {
            return 0x01;
        } else if constexpr (std::is_same_v<Type, std::int8_t>) {
            return 0x02;
        } else if constexpr (std::is_same_v<Type, std::int16_t>) {
            return 0x03;
        } else if constexpr (std::is_same_v<Type, std::int32_t>) {
            return 0x04;
        } else if constexpr (std::is_same_v<Type, float>) {
            return 0x06;
        } else if constexpr (std::is_same_v<Type, double>) {
            return 0x07;
        } else if constexpr (std::is_same_v<Type, DateTimeValue>) {
            return 0x08;
        } else if constexpr (std::is_same_v<Type, std::string>) {
            return 0x0a;
        } else if constexpr (std::is_same_v<Type, BlobValue>) {
            return 0x0b;
        } else if constexpr (std::is_same_v<Type, std::int64_t>) {
            return 0x13;
        } else {
            return std::nullopt;
        }
    }, value);
}

}

PropertyInfo* PropertyMapInfo::find(std::string_view property_name) {
    return const_cast<PropertyInfo*>(std::as_const(*this).find(property_name));
}

const PropertyInfo* PropertyMapInfo::find(std::string_view property_name) const {
    const auto found = std::find_if(properties.begin(), properties.end(), [&](const auto& property) {
        return detail::equals_ignore_case(property.name, property_name);
    });
    return found == properties.end() ? nullptr : &*found;
}

Value PropertyMapInfo::get_value(std::string_view property_name, Value default_value) const {
    const auto* property = find(property_name);
    if (!property || property->value_is_null ||
        std::holds_alternative<std::monostate>(property->value)) {
        return default_value;
    }
    return property->value;
}

PropertyInfo& PropertyMapInfo::put(PropertyInfo property) {
    if (property.name.empty()) {
        throw std::invalid_argument("A property name cannot be empty");
    }
    if (auto* existing = find(property.name)) {
        *existing = std::move(property);
        return *existing;
    }
    properties.push_back(std::move(property));
    return properties.back();
}

PropertyInfo& PropertyMapInfo::put(std::string property_name, Value property_value) {
    if (property_name.empty()) {
        throw std::invalid_argument("A property name cannot be empty");
    }

    const auto built_in = built_in_property_type(property_name);
    const auto type_code = built_in ? std::optional<std::uint8_t>(built_in->type_code)
                                    : inferred_property_type(property_value);
    if (!type_code) {
        throw std::invalid_argument("Could not determine a Jet4 property type for '" + property_name + "'");
    }

    return put(std::move(property_name), *type_code, std::move(property_value),
               built_in && built_in->ddl);
}

PropertyInfo& PropertyMapInfo::put(std::string property_name,
                                   std::uint8_t type_code,
                                   Value property_value,
                                   bool is_ddl) {
    if (property_name.empty()) {
        throw std::invalid_argument("A property name cannot be empty");
    }

    const auto built_in = built_in_property_type(property_name);
    const auto flags = static_cast<std::uint8_t>((is_ddl || (built_in && built_in->ddl)) ? 0x01 : 0);
    PropertyInfo property{std::move(property_name), type_code, flags, {}, {}, {}, false};
    property.set_value(std::move(property_value));
    return put(std::move(property));
}

void PropertyMapInfo::put_all(const std::vector<PropertyInfo>& new_properties) {
    for (const auto& property : new_properties) {
        put(property);
    }
}

bool PropertyMapInfo::remove(std::string_view property_name) {
    const auto* existing = find(property_name);
    if (!existing) {
        return false;
    }
    properties.erase(properties.begin() + (existing - properties.data()));
    return true;
}

void PropertyInfo::set_value(Value new_value) {
    if (std::holds_alternative<std::monostate>(new_value)) {
        raw_value.clear();
        trailing_bytes.clear();
        value = std::move(new_value);
        value_is_null = true;
        return;
    }

    std::vector<std::uint8_t> encoded;
    const auto append_scalar = [&](std::uint64_t bits, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            encoded.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        }
    };
    bool matched = true;
    switch (type_code) {
        case 0x01:
            if (const auto* item = std::get_if<bool>(&new_value)) append_scalar(*item, 1);
            else matched = false;
            break;
        case 0x02:
            if (const auto* item = std::get_if<std::int8_t>(&new_value)) append_scalar(*item, 1);
            else matched = false;
            break;
        case 0x03:
            if (const auto* item = std::get_if<std::int16_t>(&new_value)) append_scalar(*item, 2);
            else matched = false;
            break;
        case 0x04:
            if (const auto* item = std::get_if<std::int32_t>(&new_value)) append_scalar(*item, 4);
            else matched = false;
            break;
        case 0x05:
            if (const auto* item = std::get_if<DecimalValue>(&new_value)) {
                append_scalar(detail::jet4_money_scaled_value(item->text), 8);
            } else {
                matched = false;
            }
            break;
        case 0x06:
            if (const auto* item = std::get_if<float>(&new_value)) {
                std::uint32_t bits;
                std::memcpy(&bits, item, sizeof(bits));
                append_scalar(bits, sizeof(bits));
            } else {
                matched = false;
            }
            break;
        case 0x07:
            if (const auto* item = std::get_if<double>(&new_value)) {
                std::uint64_t bits;
                std::memcpy(&bits, item, sizeof(bits));
                append_scalar(bits, sizeof(bits));
            } else {
                matched = false;
            }
            break;
        case 0x08:
            if (const auto* item = std::get_if<DateTimeValue>(&new_value)) {
                const auto serial = detail::jet4_date_serial(item->iso8601);
                if (serial) {
                    std::uint64_t bits;
                    std::memcpy(&bits, &*serial, sizeof(bits));
                    append_scalar(bits, sizeof(bits));
                } else {
                    matched = false;
                }
            } else if (const auto* item = std::get_if<DateOutOfRangeValue>(&new_value)) {
                std::uint64_t bits;
                std::memcpy(&bits, &item->serial, sizeof(bits));
                append_scalar(bits, sizeof(bits));
            } else {
                matched = false;
            }
            break;
        case 0x09:
        case 0x0b:
            if (const auto* item = std::get_if<BlobValue>(&new_value)) {
                encoded = item->bytes;
            } else if (type_code == 0x09 && detail::equals_ignore_case(name, "GUID") &&
                       std::holds_alternative<GuidValue>(new_value)) {
                const auto guid = detail::jet4_guid_bytes(std::get<GuidValue>(new_value).text);
                encoded.assign(guid.begin(), guid.end());
            } else {
                matched = false;
            }
            break;
        case 0x0a:
        case 0x0c:
            if (const auto* item = std::get_if<std::string>(&new_value)) {
                const auto units = detail::utf8_to_utf16(*item, (std::numeric_limits<std::int16_t>::max() - 8) / 2,
                                                        "Property text exceeds Jet4 limits");
                for (const auto unit : units) append_scalar(unit, 2);
            } else {
                matched = false;
            }
            break;
        case 0x0f:
            if (const auto* item = std::get_if<GuidValue>(&new_value)) {
                const auto guid = detail::jet4_guid_bytes(item->text);
                encoded.assign(guid.begin(), guid.end());
            } else {
                matched = false;
            }
            break;
        case 0x13:
            if (const auto* item = std::get_if<std::int64_t>(&new_value)) append_scalar(*item, 8);
            else matched = false;
            break;
        default:
            matched = false;
            break;
    }
    if (!matched) {
        throw std::invalid_argument("Value does not match a supported Jet4 property type");
    }
    raw_value = std::move(encoded);
    trailing_bytes.clear();
    value = std::move(new_value);
    value_is_null = false;
}

}

#include "jet4_table_creator.hpp"
#include "jet4_fixed_type.hpp"
#include "jet4_global_usage_map.hpp"
#include "jet4_variable_type.hpp"

#include "../util/little_endian.hpp"
#include "../catalog/row_reader.hpp"
#include "../catalog/system_catalog.hpp"
#include "../catalog/table_definition.hpp"
#include "../catalog/usage_map.hpp"
#include "../page/page_file.hpp"
#include "../page/page_types.hpp"
#include "../page/page_writer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t table_definition_header_size = 63;
constexpr std::uint64_t column_definition_size = 25;
constexpr std::uint64_t table_definition_trailer_size = 2;
constexpr std::uint64_t data_page_row_count_offset = 12;
constexpr std::uint64_t data_page_row_directory_offset = 14;
constexpr std::uint64_t index_table_definition_offset = 4;
constexpr std::uint64_t index_prefix_length_offset = 24;
constexpr std::uint64_t index_entry_mask_offset = 27;
constexpr std::uint64_t index_entry_mask_size = 453;
constexpr std::uint64_t index_entry_data_offset = index_entry_mask_offset + index_entry_mask_size;
constexpr std::uint64_t index_leaf_trailer_size = 4;
constexpr std::uint32_t table_definition_magic = 0x659;
constexpr std::uint8_t user_table_type = 0x4e;
constexpr std::uint8_t short_type = jet4_short_type_code;
constexpr std::uint8_t long_type = jet4_long_type_code;
constexpr std::uint8_t date_type = 0x08;
constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t binary_type = 0x09;
constexpr std::int16_t table_object_type = 1;
constexpr std::int16_t database_properties_object_type = 2;
constexpr std::int16_t directory_object_type = 3;

[[noreturn]] void unsupported_operation(const std::filesystem::path& path, const char* message) {
    throw DatabaseError(DatabaseErrorCode::unsupported_operation, message, path);
}

[[noreturn]] void corrupted_file(const std::filesystem::path& path,
                                 std::uint64_t page_number,
                                 const char* message) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, message, path, page_number);
}

void write_u24_le(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

bool is_supported_name_character(unsigned char value) noexcept {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_' || value == ' ';
}

void validate_name(const std::filesystem::path& path,
                   std::string_view name,
                   const char* kind) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') {
        unsupported_operation(path, kind);
    }
    if (!std::all_of(name.begin(), name.end(), [](unsigned char value) {
            return is_supported_name_character(value);
        })) {
        unsupported_operation(path, kind);
    }
}

void validate_table_builder(const std::filesystem::path& path, const TableBuilder& table) {
    validate_name(path,
                  table.name(),
                  "Table names currently support 1-64 ASCII letters, digits, spaces, and underscores");
    if (table.columns().empty() || table.columns().size() > 255) {
        unsupported_operation(path, "A Jet4 table must contain between 1 and 255 columns");
    }
    if (table.name().size() >= 4 && equals_ascii_case_insensitive(table.name().substr(0, 4), "MSys")) {
        unsupported_operation(path, "User table names beginning with MSys are reserved");
    }

    std::vector<std::string_view> names;
    names.reserve(table.columns().size());
    for (const auto& column : table.columns()) {
        validate_name(path,
                      column.name(),
                      "Column names currently support 1-64 ASCII letters, digits, spaces, and underscores");
        if (jet4_fixed_type(column.type()) == nullptr && jet4_variable_type(column.type()) == nullptr &&
            !jet4_is_boolean_type(column.type())) {
            unsupported_operation(path,
                                 "Initial Jet4 table creation currently supports BOOLEAN, BYTE, SHORT, LONG, MONEY, SINGLE, DOUBLE, GUID, TEXT, BINARY, MEMO, and OLE columns only");
        }
        if (std::any_of(names.begin(), names.end(), [&](std::string_view previous) {
                return equals_ascii_case_insensitive(previous, column.name());
            })) {
            unsupported_operation(path, "Column names must be unique without regard to ASCII letter case");
        }
        names.push_back(column.name());
    }
}

std::vector<std::uint8_t> encode_utf16le_ascii(std::string_view value) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(value.size() * 2);
    for (unsigned char character : value) {
        bytes.push_back(character);
        bytes.push_back(0);
    }
    return bytes;
}

std::vector<std::uint8_t> encode_int32_index_key(std::int32_t value) {
    const std::uint32_t unsigned_value = static_cast<std::uint32_t>(value);
    std::vector<std::uint8_t> key{
        0x7f,
        static_cast<std::uint8_t>(((unsigned_value >> 24) & 0xff) ^ 0x80),
        static_cast<std::uint8_t>((unsigned_value >> 16) & 0xff),
        static_cast<std::uint8_t>((unsigned_value >> 8) & 0xff),
        static_cast<std::uint8_t>(unsigned_value & 0xff),
    };
    return key;
}

std::array<std::uint8_t, 2> ascii_legacy_index_code(char value) {
    static constexpr std::array<std::uint8_t, 26> letter_codes{
        0x4a, 0x4c, 0x4d, 0x4f, 0x51, 0x53, 0x55, 0x57, 0x59,
        0x5b, 0x5c, 0x5e, 0x60, 0x62, 0x64, 0x66, 0x68, 0x69,
        0x6b, 0x6d, 0x6f, 0x71, 0x73, 0x75, 0x76, 0x78,
    };
    static constexpr std::array<std::uint8_t, 10> digit_codes{
        0x36, 0x38, 0x3a, 0x3c, 0x3e, 0x40, 0x42, 0x44, 0x46, 0x48,
    };

    if (value >= 'A' && value <= 'Z') {
        return {letter_codes[static_cast<std::size_t>(value - 'A')], 0};
    }
    if (value >= 'a' && value <= 'z') {
        return {letter_codes[static_cast<std::size_t>(value - 'a')], 0};
    }
    if (value >= '0' && value <= '9') {
        return {digit_codes[static_cast<std::size_t>(value - '0')], 0};
    }
    if (value == ' ') {
        return {0x07, 0};
    }
    if (value == '_') {
        return {0x2b, 0x03};
    }
    return {0, 0};
}

std::vector<std::uint8_t> encode_parent_name_index_key(std::int32_t parent_id,
                                                      std::string_view table_name) {
    auto key = encode_int32_index_key(parent_id);
    key.push_back(0x7f);
    for (char character : table_name) {
        const auto code = ascii_legacy_index_code(character);
        key.push_back(code[0]);
        if (code[1] != 0) {
            key.push_back(code[1]);
        }
    }
    key.push_back(0x01);
    key.push_back(0x00);
    return key;
}

struct IndexEntry {
    std::vector<std::uint8_t> key;
    std::uint32_t row_page_number;
    std::uint8_t row_number;
};

bool index_entry_less(const IndexEntry& left, const IndexEntry& right) {
    if (left.key != right.key) {
        return std::lexicographical_compare(left.key.begin(), left.key.end(),
                                            right.key.begin(), right.key.end());
    }
    if (left.row_page_number != right.row_page_number) {
        return left.row_page_number < right.row_page_number;
    }
    return left.row_number < right.row_number;
}

std::vector<IndexEntry> read_index_entries(const PageFile& file,
                                           std::uint64_t definition_page_number,
                                           std::uint32_t root_page_number,
                                           const std::vector<std::uint8_t>& page) {
    if (page.size() != file.format().page_size || page[0] != static_cast<std::uint8_t>(PageType::index_leaf)) {
        corrupted_file(file.path(), root_page_number, "System index root is not a Jet4 leaf page");
    }
    if (read_u32(page, index_table_definition_offset) != definition_page_number) {
        corrupted_file(file.path(), root_page_number, "System index page references the wrong table definition");
    }
    if (read_u16(page, index_prefix_length_offset) != 0) {
        unsupported_operation(file.path(), "System index pages with compressed key prefixes are not yet writable");
    }

    const std::uint64_t data_capacity = page.size() - index_entry_data_offset;
    std::vector<IndexEntry> entries;
    std::uint64_t previous_end = 0;
    for (std::uint64_t mask_byte = 0; mask_byte < index_entry_mask_size; ++mask_byte) {
        const std::uint8_t mask = page[index_entry_mask_offset + mask_byte];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((mask & static_cast<std::uint8_t>(1u << bit)) == 0) {
                continue;
            }
            const std::uint64_t end = mask_byte * 8 + bit;
            if (end <= previous_end || end > data_capacity || end - previous_end < index_leaf_trailer_size) {
                corrupted_file(file.path(), root_page_number, "System index entry mask is outside the leaf page");
            }

            const std::uint64_t start = index_entry_data_offset + previous_end;
            const std::uint64_t entry_size = end - previous_end;
            const std::uint64_t key_size = entry_size - index_leaf_trailer_size;
            const std::uint64_t row_offset = start + key_size;
            const std::uint32_t row_page =
                (static_cast<std::uint32_t>(page[row_offset]) << 16) |
                (static_cast<std::uint32_t>(page[row_offset + 1]) << 8) |
                page[row_offset + 2];
            if (row_page == 0 || row_page >= file.page_count()) {
                corrupted_file(file.path(), root_page_number, "System index entry has an invalid row page reference");
            }

            IndexEntry entry{
                std::vector<std::uint8_t>(page.begin() + static_cast<std::ptrdiff_t>(start),
                                          page.begin() + static_cast<std::ptrdiff_t>(row_offset)),
                row_page,
                page[row_offset + 3],
            };
            if (!entries.empty() && !index_entry_less(entries.back(), entry)) {
                corrupted_file(file.path(), root_page_number, "System index entries are not strictly ordered");
            }
            entries.push_back(std::move(entry));
            previous_end = end;
        }
    }

    if (previous_end + read_u16(page, 2) != data_capacity) {
        corrupted_file(file.path(), root_page_number, "System index free-space count is inconsistent");
    }
    return entries;
}

std::vector<std::uint8_t> insert_index_entry(const PageFile& file,
                                            std::uint64_t definition_page_number,
                                            std::uint32_t root_page_number,
                                            std::vector<std::uint8_t> page,
                                            IndexEntry entry) {
    auto entries = read_index_entries(file, definition_page_number, root_page_number, page);
    const auto position = std::lower_bound(entries.begin(), entries.end(), entry, index_entry_less);
    if (position != entries.end() && position->key == entry.key) {
        unsupported_operation(file.path(), "A system index already contains the new table key");
    }
    entries.insert(position, std::move(entry));

    const std::uint64_t data_capacity = page.size() - index_entry_data_offset;
    std::uint64_t used = 0;
    for (const auto& current : entries) {
        used += current.key.size() + index_leaf_trailer_size;
    }
    if (used > data_capacity || used >= index_entry_mask_size * 8) {
        unsupported_operation(file.path(), "System index leaf is full; page splitting is not yet implemented");
    }

    std::fill(page.begin() + index_entry_mask_offset, page.end(), 0);
    std::uint64_t data_offset = index_entry_data_offset;
    std::uint64_t cumulative_end = 0;
    for (const auto& current : entries) {
        std::copy(current.key.begin(), current.key.end(), page.begin() + static_cast<std::ptrdiff_t>(data_offset));
        data_offset += current.key.size();
        page[data_offset++] = static_cast<std::uint8_t>((current.row_page_number >> 16) & 0xff);
        page[data_offset++] = static_cast<std::uint8_t>((current.row_page_number >> 8) & 0xff);
        page[data_offset++] = static_cast<std::uint8_t>(current.row_page_number & 0xff);
        page[data_offset++] = current.row_number;
        cumulative_end += current.key.size() + index_leaf_trailer_size;
        page[index_entry_mask_offset + cumulative_end / 8] |=
            static_cast<std::uint8_t>(1u << (cumulative_end % 8));
    }
    write_u16(page, 2, static_cast<std::uint16_t>(data_capacity - used));
    return page;
}

std::size_t find_system_index(const std::filesystem::path& path,
                              const TableDefinition& definition,
                              std::string_view name) {
    for (const auto& logical : definition.indexes) {
        if (!equals_ascii_case_insensitive(logical.name, name)) {
            continue;
        }
        if (logical.index_data_number >= definition.physical_indexes.size()) {
            corrupted_file(path, 2, "MSysObjects index references missing physical index data");
        }
        return logical.index_data_number;
    }
    unsupported_operation(path, "MSysObjects is missing an index required for table creation");
}

const ColumnDefinition& find_column(const std::filesystem::path& path,
                                    const TableDefinition& definition,
                                    std::string_view name,
                                    std::uint8_t expected_type) {
    const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& value) {
        return equals_ascii_case_insensitive(value.name, name);
    });
    if (column == definition.columns.end() || column->type != expected_type) {
        corrupted_file(path, 2, "MSysObjects is missing a required column or has an incompatible type");
    }
    return *column;
}

std::vector<std::uint8_t> database_timestamp_bytes() {
    using namespace std::chrono;
    constexpr double unix_epoch_access_days = 25569.0;
    constexpr double seconds_per_day = 86400.0;
    const auto now = system_clock::now().time_since_epoch();
    const double access_days = unix_epoch_access_days + duration<double>(now).count() / seconds_per_day;
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(access_days));
    std::memcpy(&bits, &access_days, sizeof(bits));
    std::vector<std::uint8_t> bytes(8);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((bits >> (index * 8)) & 0xff);
    }
    return bytes;
}

std::vector<std::uint8_t> read_database_owner(const PageFile& file,
                                             const TableDefinition& system_definition,
                                             const std::vector<SystemCatalogObject>& objects) {
    const auto owner_object = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
        return object.name && equals_ascii_case_insensitive(*object.name, "MSysDb") &&
               object.type && *object.type == database_properties_object_type;
    });
    if (owner_object != objects.end()) {
        const auto owner_column = std::find_if(system_definition.columns.begin(), system_definition.columns.end(),
                                               [](const auto& column) {
            return equals_ascii_case_insensitive(column.name, "Owner") && column.type == binary_type;
        });
        if (owner_column != system_definition.columns.end()) {
            TableRowReader reader(file, "MSysObjects", system_definition);
            const auto owner_row = reader.read_at(owner_object->page_number, owner_object->row_number);
            if (owner_column->column_number < owner_row.columns.size() &&
                owner_row.columns[owner_column->column_number].bytes &&
                !owner_row.columns[owner_column->column_number].bytes->empty()) {
                return *owner_row.columns[owner_column->column_number].bytes;
            }
        }
    }
    return {0xa6, 0x33};
}

std::vector<std::uint8_t> build_system_catalog_row(const std::filesystem::path& path,
                                                   const TableDefinition& definition,
                                                   std::int32_t object_id,
                                                   std::int32_t parent_id,
                                                   std::string_view table_name,
                                                   const std::vector<std::uint8_t>& owner,
                                                   const std::vector<std::uint8_t>& timestamp) {
    const auto& id_column = find_column(path, definition, "Id", long_type);
    const auto& parent_column = find_column(path, definition, "ParentId", long_type);
    const auto& name_column = find_column(path, definition, "Name", text_type);
    const auto& type_column = find_column(path, definition, "Type", short_type);
    const auto& create_column = find_column(path, definition, "DateCreate", date_type);
    const auto& update_column = find_column(path, definition, "DateUpdate", date_type);
    const auto& owner_column = find_column(path, definition, "Owner", binary_type);
    const auto& flags_column = find_column(path, definition, "Flags", long_type);
    if (definition.maximum_column_count > 255 || definition.variable_column_count == 0) {
        corrupted_file(path, 2, "MSysObjects has an unsupported row layout");
    }

    std::vector<std::optional<std::vector<std::uint8_t>>> values(definition.maximum_column_count);
    auto set_bytes = [&](const ColumnDefinition& column, std::vector<std::uint8_t> bytes) {
        if (column.column_number >= values.size()) {
            corrupted_file(path, 2, "MSysObjects column number exceeds its row column count");
        }
        values[column.column_number] = std::move(bytes);
    };
    auto int32_bytes = [](std::int32_t value) {
        std::vector<std::uint8_t> bytes;
        append_u32(bytes, static_cast<std::uint32_t>(value));
        return bytes;
    };
    auto int16_bytes = [](std::int16_t value) {
        std::vector<std::uint8_t> bytes;
        append_u16(bytes, static_cast<std::uint16_t>(value));
        return bytes;
    };

    set_bytes(id_column, int32_bytes(object_id));
    set_bytes(parent_column, int32_bytes(parent_id));
    set_bytes(name_column, encode_utf16le_ascii(table_name));
    set_bytes(type_column, int16_bytes(table_object_type));
    set_bytes(create_column, timestamp);
    set_bytes(update_column, timestamp);
    set_bytes(owner_column, owner);
    set_bytes(flags_column, int32_bytes(0));

    std::uint64_t fixed_data_size = 0;
    std::vector<const ColumnDefinition*> variable_columns(definition.variable_column_count, nullptr);
    for (const auto& column : definition.columns) {
        if ((column.flags & 0x01) != 0) {
            fixed_data_size = std::max(fixed_data_size,
                                       static_cast<std::uint64_t>(column.fixed_data_offset) + column.length);
        } else {
            if (column.variable_table_index >= variable_columns.size() ||
                variable_columns[column.variable_table_index] != nullptr) {
                corrupted_file(path, 2, "MSysObjects variable columns have inconsistent offsets");
            }
            variable_columns[column.variable_table_index] = &column;
        }
    }
    if (std::any_of(variable_columns.begin(), variable_columns.end(), [](const auto* column) {
            return column == nullptr;
        })) {
        corrupted_file(path, 2, "MSysObjects has a gap in its variable column offsets");
    }

    const std::uint64_t null_mask_size = (static_cast<std::uint64_t>(definition.maximum_column_count) + 7) / 8;
    std::vector<std::uint8_t> row(2 + static_cast<std::size_t>(fixed_data_size), 0);
    write_u16(row, 0, definition.maximum_column_count);
    std::vector<std::uint8_t> null_mask(static_cast<std::size_t>(null_mask_size), 0);
    std::vector<std::optional<std::vector<std::uint8_t>>> variable_values(definition.variable_column_count);

    for (const auto& column : definition.columns) {
        const auto& value = values[column.column_number];
        if (!value) {
            continue;
        }
        null_mask[column.column_number / 8] |= static_cast<std::uint8_t>(1u << (column.column_number % 8));
        if ((column.flags & 0x01) != 0) {
            if (value->size() != column.length ||
                column.fixed_data_offset > fixed_data_size ||
                value->size() > fixed_data_size - column.fixed_data_offset) {
                corrupted_file(path, 2, "MSysObjects fixed value does not match its column layout");
            }
            std::copy(value->begin(), value->end(),
                      row.begin() + static_cast<std::ptrdiff_t>(2 + column.fixed_data_offset));
        } else {
            variable_values[column.variable_table_index] = value;
        }
    }

    std::vector<std::uint16_t> variable_offsets(definition.variable_column_count);
    for (std::size_t index = 0; index < variable_columns.size(); ++index) {
        if (row.size() > std::numeric_limits<std::uint16_t>::max()) {
            unsupported_operation(path, "MSysObjects row exceeds Jet4 variable-column offset limits");
        }
        variable_offsets[index] = static_cast<std::uint16_t>(row.size());
        if (variable_values[index]) {
            row.insert(row.end(), variable_values[index]->begin(), variable_values[index]->end());
        }
    }
    if (row.size() > std::numeric_limits<std::uint16_t>::max()) {
        unsupported_operation(path, "MSysObjects row exceeds Jet4 variable-column offset limits");
    }
    append_u16(row, static_cast<std::uint16_t>(row.size()));
    for (auto offset = variable_offsets.rbegin(); offset != variable_offsets.rend(); ++offset) {
        append_u16(row, *offset);
    }
    append_u16(row, definition.variable_column_count);
    row.insert(row.end(), null_mask.begin(), null_mask.end());
    return row;
}

std::vector<std::uint8_t> build_table_definition_page(const std::filesystem::path& path,
                                                     std::uint32_t page_size,
                                                     std::uint32_t usage_map_page_number,
                                                     const TableBuilder& table) {
    std::uint64_t definition_size = table_definition_header_size +
                                   table.columns().size() * column_definition_size +
                                   table_definition_trailer_size;
    for (const auto& column : table.columns()) {
        definition_size += 2 + column.name().size() * 2;
    }
    const auto long_value_column_count = static_cast<std::uint64_t>(std::count_if(
        table.columns().begin(), table.columns().end(), [](const ColumnBuilder& column) {
            return jet4_is_long_value_type(column.type());
        }));
    definition_size += long_value_column_count * 10;
    if (definition_size > page_size || definition_size > std::numeric_limits<std::uint32_t>::max()) {
        unsupported_operation(path, "Table definitions larger than one Jet4 page are not yet supported");
    }

    std::vector<std::uint8_t> page(page_size, 0);
    page[0] = static_cast<std::uint8_t>(PageType::table_definition);
    page[1] = 0x01;
    const std::uint64_t free_space = definition_size + 8 <= page_size
                                         ? page_size - definition_size - 8
                                         : 0;
    write_u16(page, 2, static_cast<std::uint16_t>(free_space));
    write_u32(page, 8, static_cast<std::uint32_t>(definition_size));
    write_u32(page, 12, table_definition_magic);
    write_u32(page, 16, 0);
    write_u32(page, 20, 0);
    page[24] = 0x01;
    page[40] = user_table_type;
    write_u16(page, 41, static_cast<std::uint16_t>(table.columns().size()));
    const std::uint16_t variable_column_count = static_cast<std::uint16_t>(std::count_if(
        table.columns().begin(), table.columns().end(), [](const ColumnBuilder& column) {
            return jet4_variable_type(column.type()) != nullptr;
        }));
    write_u16(page, 43, variable_column_count);
    write_u16(page, 45, static_cast<std::uint16_t>(table.columns().size()));
    write_u32(page, 47, 0);
    write_u32(page, 51, 0);
    page[55] = 0;
    write_u24_le(page, 56, usage_map_page_number);
    page[59] = 1;
    write_u24_le(page, 60, usage_map_page_number);

    std::uint64_t column_offset = table_definition_header_size;
    std::uint64_t fixed_data_offset = 0;
    std::uint16_t variable_table_index = 0;
    std::uint16_t long_value_table_index = static_cast<std::uint16_t>(std::count_if(
        table.columns().begin(), table.columns().end(), [](const ColumnBuilder& column) {
            return jet4_variable_type(column.type()) != nullptr && !jet4_is_long_value_type(column.type());
        }));
    for (std::size_t index = 0; index < table.columns().size(); ++index) {
        const auto& column = table.columns()[index];
        const bool boolean_column = jet4_is_boolean_type(column.type());
        const auto* storage = jet4_fixed_type(column.type());
        const auto* variable_storage = jet4_variable_type(column.type());
        if (!boolean_column && (storage == nullptr) == (variable_storage == nullptr)) {
            unsupported_operation(path, "Jet4 table has an unsupported column storage layout");
        }
        if (fixed_data_offset > std::numeric_limits<std::uint16_t>::max()) {
            unsupported_operation(path, "Jet4 table fixed data layout exceeds the format limit");
        }
        const std::uint16_t variable_index =
            storage != nullptr
                ? 0
                : (jet4_is_long_value_type(column.type())
                       ? long_value_table_index++
                       : (variable_storage != nullptr ? variable_table_index++ : 0));
        page[column_offset] = boolean_column
                                  ? jet4_boolean_type_code
                                  : (storage != nullptr ? storage->jet_type_code : variable_storage->jet_type_code);
        write_u32(page, column_offset + 1, table_definition_magic);
        write_u16(page, column_offset + 5, static_cast<std::uint16_t>(index));
        write_u16(page, column_offset + 7, variable_index);
        write_u16(page, column_offset + 9, static_cast<std::uint16_t>(index));
        page[column_offset + 11] = 0;
        page[column_offset + 12] = 0;
        write_u16(page, column_offset + 13, 0);
        page[column_offset + 15] = boolean_column || storage != nullptr ? 0x03 : 0x02;
        page[column_offset + 16] = 0;
        write_u32(page, column_offset + 17, 0);
        write_u16(page,
                  column_offset + 21,
                  storage != nullptr ? static_cast<std::uint16_t>(fixed_data_offset) : 0);
        const std::uint16_t column_size =
            boolean_column
                ? 1
                : (storage != nullptr ? storage->byte_size : variable_storage->maximum_byte_size);
        write_u16(page, column_offset + 23, column_size);
        if (storage != nullptr) {
            fixed_data_offset += storage->byte_size;
        }
        column_offset += column_definition_size;
    }

    for (const auto& column : table.columns()) {
        const std::uint16_t encoded_name_size = static_cast<std::uint16_t>(column.name().size() * 2);
        write_u16(page, column_offset, encoded_name_size);
        column_offset += 2;
        for (unsigned char character : column.name()) {
            page[column_offset++] = character;
            page[column_offset++] = 0;
        }
    }

    std::uint8_t long_value_map_row = 2;
    for (std::size_t index = 0; index < table.columns().size(); ++index) {
        const auto& column = table.columns()[index];
        if (!jet4_is_long_value_type(column.type())) {
            continue;
        }
        write_u16(page, column_offset, static_cast<std::uint16_t>(index));
        page[column_offset + 2] = long_value_map_row;
        write_u24_le(page, column_offset + 3, usage_map_page_number);
        page[column_offset + 6] = static_cast<std::uint8_t>(long_value_map_row + 1);
        write_u24_le(page, column_offset + 7, usage_map_page_number);
        long_value_map_row = static_cast<std::uint8_t>(long_value_map_row + 2);
        column_offset += 10;
    }
    page[column_offset++] = 0xff;
    page[column_offset] = 0xff;
    return page;
}

std::vector<std::uint8_t> build_usage_map_page(const std::filesystem::path& path,
                                              std::uint32_t page_size,
                                              const TableBuilder& table) {
    constexpr std::uint64_t usage_map_row_size = 69;
    const auto long_value_columns = static_cast<std::uint64_t>(std::count_if(
        table.columns().begin(), table.columns().end(), [](const ColumnBuilder& column) {
            return jet4_is_long_value_type(column.type());
        }));
    const std::uint64_t usage_map_count = 2 + long_value_columns * 2;
    if (usage_map_count > 255) {
        unsupported_operation(path, "Too many Memo/OLE columns for one Jet4 usage-map page");
    }
    const std::uint64_t directory_size = usage_map_count * 2;
    const std::uint64_t header_size = data_page_row_directory_offset;
    const std::uint64_t total_row_size = usage_map_count * usage_map_row_size;
    if (header_size + directory_size + total_row_size > page_size) {
        unsupported_operation(path, "The Jet4 usage-map declarations do not fit on one page");
    }

    std::vector<std::uint8_t> page(page_size, 0);
    page[0] = static_cast<std::uint8_t>(PageType::data);
    page[1] = 0x01;
    const std::uint16_t free_space = static_cast<std::uint16_t>(page_size - header_size - directory_size - total_row_size);
    write_u16(page, 2, free_space);
    write_u16(page, data_page_row_count_offset, usage_map_count);

    for (std::uint64_t row_number = 0; row_number < usage_map_count; ++row_number) {
        const std::uint16_t row_start = static_cast<std::uint16_t>(page_size - (row_number + 1) * usage_map_row_size);
        write_u16(page, data_page_row_directory_offset + row_number * 2, row_start);
        page[row_start] = row_number == 0 ? 0x01 : 0x00;
    }
    return page;
}

std::vector<std::uint8_t> append_system_catalog_row(const std::filesystem::path& path,
                                                   const PageFile& file,
                                                   const TableDefinition& system_definition,
                                                   const std::vector<SystemCatalogObject>& objects,
                                                   std::int32_t object_id,
                                                   std::int32_t parent_id,
                                                   std::string_view table_name,
                                                   std::uint32_t& row_page_number,
                                                   std::uint8_t& row_number) {
    const auto owner = read_database_owner(file, system_definition, objects);
    const auto timestamp = database_timestamp_bytes();
    const auto row = build_system_catalog_row(path,
                                              system_definition,
                                              object_id,
                                              parent_id,
                                              table_name,
                                              owner,
                                              timestamp);

    const auto owned_pages = read_usage_map_pages(file, system_definition.owned_pages);
    for (const std::uint64_t page_number : owned_pages) {
        const auto page = file.read_page(page_number);
        if (page[0] != static_cast<std::uint8_t>(PageType::data)) {
            corrupted_file(path, page_number, "MSysObjects Usage Map references a non-data page");
        }
        const std::uint16_t count = read_u16(page, data_page_row_count_offset);
        if (count >= 255) {
            continue;
        }
        const std::uint64_t directory_end = data_page_row_directory_offset + static_cast<std::uint64_t>(count + 1) * 2;
        const std::uint16_t last_row_start = count == 0
                                                 ? static_cast<std::uint16_t>(page.size())
                                                 : static_cast<std::uint16_t>(read_u16(
                                                       page,
                                                       data_page_row_directory_offset + static_cast<std::uint64_t>(count - 1) * 2) &
                                                                           0x1fff);
        if (row.size() > last_row_start || last_row_start - row.size() < directory_end) {
            continue;
        }

        auto updated = page;
        const auto start_offset = static_cast<std::uint16_t>(last_row_start - row.size());
        std::copy(row.begin(), row.end(), updated.begin() + start_offset);
        write_u16(updated,
                  data_page_row_directory_offset + static_cast<std::uint64_t>(count) * 2,
                  start_offset);
        write_u16(updated, data_page_row_count_offset, static_cast<std::uint16_t>(count + 1));
        write_u16(updated, 2, static_cast<std::uint16_t>(start_offset - directory_end));
        row_page_number = static_cast<std::uint32_t>(page_number);
        row_number = static_cast<std::uint8_t>(count);
        return updated;
    }

    unsupported_operation(path, "MSysObjects has no data page with room for another row; page allocation is pending");
}

struct NamedIndex {
    std::size_t physical_index_number;
    std::uint32_t root_page_number;
};

NamedIndex find_named_index(const std::filesystem::path& path,
                            const TableDefinition& definition,
                            std::string_view name) {
    const std::size_t physical_index_number = find_system_index(path, definition, name);
    const auto& physical = definition.physical_indexes[physical_index_number];
    if (physical.root_page_number == 0) {
        unsupported_operation(path, "MSysObjects system index has no root page");
    }
    return {physical_index_number, physical.root_page_number};
}

}

void create_jet4_table(const std::filesystem::path& path,
                       DatabaseFormat format,
                       const TableBuilder& table) {
    validate_table_builder(path, table);
    if (format != DatabaseFormat::v2000 && format != DatabaseFormat::v2003) {
        unsupported_operation(path, "Initial table schema creation currently supports Jet4 V2000 and V2003 files only");
    }

    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> page_updates;
    std::vector<std::uint8_t> table_definition_page;
    std::vector<std::uint8_t> usage_map_page;
    std::uint64_t expected_page_count = 0;
    std::uint64_t table_definition_page_number = 0;
    std::uint64_t usage_map_page_number = 0;

    {
        const PageFile file = PageFile::open(path);
        if (file.format().version != EngineVersion::jet4 || file.format().page_size != 4096) {
            unsupported_operation(path, "Table schema creation requires a Jet4 4 KB page database");
        }
        expected_page_count = file.page_count();
        if (expected_page_count + 2 > 0x1000000) {
            unsupported_operation(path, "Jet4 table creation exceeds the three-byte page-reference limit");
        }
        table_definition_page_number = expected_page_count;
        usage_map_page_number = expected_page_count + 1;

        const TableDefinition system_definition = read_table_definition(file, 2);
        const auto objects = read_system_catalog_objects(file, system_definition);
        for (const auto& object : objects) {
            if (object.name && equals_ascii_case_insensitive(*object.name, table.name())) {
                unsupported_operation(path, "A table or system object with this name already exists");
            }
        }

        const auto parent = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
            return object.name && equals_ascii_case_insensitive(*object.name, "Tables") &&
                   object.type && *object.type == directory_object_type && object.id;
        });
        if (parent == objects.end()) {
            corrupted_file(path, 2, "MSysObjects has no Tables directory object");
        }
        if (table_definition_page_number > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            unsupported_operation(path, "Jet4 table definition page cannot be represented as an object ID");
        }

        std::uint32_t catalog_row_page_number = 0;
        std::uint8_t catalog_row_number = 0;
        auto catalog_data_page = append_system_catalog_row(path,
                                                           file,
                                                           system_definition,
                                                           objects,
                                                           static_cast<std::int32_t>(table_definition_page_number),
                                                           *parent->id,
                                                           table.name(),
                                                           catalog_row_page_number,
                                                           catalog_row_number);
        page_updates.emplace_back(catalog_row_page_number, std::move(catalog_data_page));

        auto updated_system_definition = file.read_page(2);
        if (system_definition.row_count == std::numeric_limits<std::uint32_t>::max()) {
            unsupported_operation(path, "MSysObjects row count is exhausted");
        }
        write_u32(updated_system_definition, 16, system_definition.row_count + 1);

        const NamedIndex parent_name_index = find_named_index(path, system_definition, "ParentIdName");
        const NamedIndex id_index = find_named_index(path, system_definition, "Id");
        const std::set<std::size_t> changed_index_numbers{
            parent_name_index.physical_index_number,
            id_index.physical_index_number,
        };
        for (const std::size_t physical_index_number : changed_index_numbers) {
            const std::uint64_t count_offset = table_definition_header_size +
                                               physical_index_number * 12 + 4;
            const auto& physical = system_definition.physical_indexes[physical_index_number];
            if (physical.unique_entry_count == std::numeric_limits<std::uint32_t>::max()) {
                unsupported_operation(path, "MSysObjects system index entry count is exhausted");
            }
            write_u32(updated_system_definition, count_offset, physical.unique_entry_count + 1);
        }
        page_updates.emplace_back(2, std::move(updated_system_definition));

        auto parent_name_page = file.read_page(parent_name_index.root_page_number);
        parent_name_page = insert_index_entry(file,
                                              2,
                                              parent_name_index.root_page_number,
                                              std::move(parent_name_page),
                                              {encode_parent_name_index_key(*parent->id, table.name()),
                                               catalog_row_page_number,
                                               catalog_row_number});
        page_updates.emplace_back(parent_name_index.root_page_number, std::move(parent_name_page));

        auto id_page = file.read_page(id_index.root_page_number);
        id_page = insert_index_entry(file,
                                     2,
                                     id_index.root_page_number,
                                     std::move(id_page),
                                     {encode_int32_index_key(static_cast<std::int32_t>(table_definition_page_number)),
                                      catalog_row_page_number,
                                      catalog_row_number});
        page_updates.emplace_back(id_index.root_page_number, std::move(id_page));

        table_definition_page = build_table_definition_page(path,
                                                            file.format().page_size,
                                                            static_cast<std::uint32_t>(usage_map_page_number),
                                                            table);
        usage_map_page = build_usage_map_page(path, file.format().page_size, table);
        page_updates.emplace_back(1,
                                  mark_global_pages_allocated(file,
                                                              {table_definition_page_number,
                                                               usage_map_page_number}));
    }

    PageFileWriter writer = PageFileWriter::open(path);
    if (writer.page_count() != expected_page_count) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database page count changed while creating a table",
                            path);
    }
    if (writer.append_page(table_definition_page) != table_definition_page_number ||
        writer.append_page(usage_map_page) != usage_map_page_number) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Jet4 table pages were appended at unexpected page numbers",
                            path);
    }
    for (const auto& update : page_updates) {
        writer.write_page(update.first, update.second);
    }
    writer.flush();
}

}

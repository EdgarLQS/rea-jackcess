#include "table_definition.hpp"

#include "../format/jet_format.hpp"
#include "../page/page_types.hpp"
#include "../value/text_codec.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t header_size = 63;
constexpr std::uint64_t page_type_offset = 0;
constexpr std::uint64_t next_page_offset = 4;
constexpr std::uint64_t definition_size_offset = 8;
constexpr std::uint64_t row_count_offset = 16;
constexpr std::uint64_t table_type_offset = 40;
constexpr std::uint64_t maximum_column_count_offset = 41;
constexpr std::uint64_t variable_column_count_offset = 43;
constexpr std::uint64_t column_count_offset = 45;
constexpr std::uint64_t logical_index_count_offset = 47;
constexpr std::uint64_t physical_index_count_offset = 51;
constexpr std::uint64_t owned_pages_offset = 55;
constexpr std::uint64_t free_space_pages_offset = 59;
constexpr std::uint64_t index_definition_size = 12;
constexpr std::uint64_t physical_index_definition_size = 52;
constexpr std::uint64_t logical_index_definition_size = 28;
constexpr std::uint64_t column_definition_size = 25;
constexpr std::uint64_t trailer_size = 2;
constexpr std::uint64_t index_definition_offset = 63;
constexpr std::uint16_t maximum_columns_per_table = 255;
constexpr std::uint32_t maximum_indexes_per_table = 32;
constexpr std::uint16_t maximum_column_name_bytes = 128;
constexpr std::uint16_t maximum_index_name_bytes = 128;
constexpr std::uint16_t maximum_index_columns = 10;
constexpr std::uint16_t unused_index_column = 0xffff;
constexpr std::uint8_t ascending_column_flag = 0x01;
constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t ole_type = 0x0b;
constexpr std::uint8_t memo_type = 0x0c;
constexpr std::uint8_t complex_type = 0x12;
constexpr std::uint8_t complex_value_foreign_key_flag = 0x08;
constexpr std::uint8_t calculated_column_flags = 0xc0;
constexpr std::uint64_t ace_complex_id_offset = 11;

bool has_ace_complex_columns(EngineVersion version) {
    switch (version) {
        case EngineVersion::ace2007:
        case EngineVersion::ace2010:
        case EngineVersion::ace2016:
        case EngineVersion::ace2019:
            return true;
        case EngineVersion::jet3:
        case EngineVersion::jet4:
        case EngineVersion::msisam:
            return false;
    }
    return false;
}

[[noreturn]] void corrupted_page(const PageFile& file,
                                 std::uint64_t page_number,
                                 const char* reason) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, reason, file.path(), page_number);
}

void require_bytes(const PageFile& file,
                   std::uint64_t page_number,
                   const std::vector<std::uint8_t>& bytes,
                   std::uint64_t offset,
                   std::uint64_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        corrupted_page(file, page_number, "Table definition field extends beyond its data");
    }
}

std::uint16_t read_u16(const PageFile& file,
                       std::uint64_t page_number,
                       const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    require_bytes(file, page_number, bytes, offset, 2);
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32(const PageFile& file,
                       std::uint64_t page_number,
                       const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    require_bytes(file, page_number, bytes, offset, 4);
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t read_u24(const PageFile& file,
                       std::uint64_t page_number,
                       const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    require_bytes(file, page_number, bytes, offset, 3);
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16);
}

std::vector<std::uint8_t> read_definition_span(const PageFile& file,
                                               std::uint64_t first_page_number,
                                               const std::vector<std::uint64_t>& continuation_pages,
                                               std::uint64_t offset,
                                               std::uint64_t count) {
    if (count > std::numeric_limits<std::size_t>::max()) {
        corrupted_page(file, first_page_number, "Table definition field is too large");
    }

    std::vector<std::uint8_t> result(static_cast<std::size_t>(count));
    const std::uint64_t page_size = file.format().page_size;
    const std::uint64_t continuation_size = page_size - 8;
    std::uint64_t copied = 0;

    while (copied < count) {
        const std::uint64_t logical_offset = offset + copied;
        std::uint64_t page_index = 0;
        std::uint64_t page_offset = logical_offset;
        if (logical_offset >= page_size) {
            const std::uint64_t continuation_offset = logical_offset - page_size;
            page_index = 1 + continuation_offset / continuation_size;
            page_offset = 8 + continuation_offset % continuation_size;
        }

        if (page_index > continuation_pages.size()) {
            corrupted_page(file, first_page_number, "Table definition field is missing continuation data");
        }
        const std::uint64_t page_number =
            page_index == 0 ? first_page_number : continuation_pages[page_index - 1];
        const std::vector<std::uint8_t> page = file.read_page(page_number);
        const std::uint64_t bytes_available = page.size() - page_offset;
        const std::uint64_t bytes_to_copy = std::min(count - copied, bytes_available);
        std::copy_n(page.begin() + static_cast<std::ptrdiff_t>(page_offset),
                    static_cast<std::size_t>(bytes_to_copy),
                    result.begin() + static_cast<std::ptrdiff_t>(copied));
        copied += bytes_to_copy;
    }

    return result;
}

struct NamedItems {
    std::vector<std::string> names;
    std::uint64_t end_offset;
};

NamedItems read_names(const PageFile& file,
                      std::uint64_t first_page_number,
                      const std::vector<std::uint64_t>& continuation_pages,
                      std::uint64_t definition_size,
                      std::uint64_t names_offset,
                      std::uint32_t count,
                      std::uint16_t maximum_name_bytes) {
    std::vector<std::string> names;
    names.reserve(count);

    for (std::uint32_t index = 0; index < count; ++index) {
        if (names_offset > definition_size || 2 > definition_size - names_offset) {
            corrupted_page(file, first_page_number, "Name length extends beyond the table definition");
        }
        const auto encoded_length =
            read_definition_span(file, first_page_number, continuation_pages, names_offset, 2);
        const std::uint16_t name_length = read_u16(file, first_page_number, encoded_length, 0);
        names_offset += 2;
        if (name_length == 0 || name_length > maximum_name_bytes || name_length % 2 != 0) {
            corrupted_page(file, first_page_number, "Name has an invalid UTF-16LE byte length");
        }
        if (names_offset > definition_size || name_length > definition_size - names_offset) {
            corrupted_page(file, first_page_number, "Name extends beyond the table definition");
        }

        const auto encoded_name =
            read_definition_span(file, first_page_number, continuation_pages, names_offset, name_length);
        names.push_back(decode_utf16le(encoded_name));
        names_offset += name_length;
    }

    return {std::move(names), names_offset};
}

std::int32_t signed_u32(std::uint32_t value) {
    if (value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(value);
    }
    return static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(~value));
}

}

TableDefinition read_table_definition(const PageFile& file, std::uint64_t page_number) {
    if (!has_jet4_page_layout(file.format().version)) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "Jet4/ACE table definition layout requested for an unsupported engine version",
                            file.path(),
                            page_number);
    }

    const std::vector<std::uint8_t> first_page = file.read_page(page_number);
    if (first_page[page_type_offset] != static_cast<std::uint8_t>(PageType::table_definition)) {
        corrupted_page(file, page_number, "Expected a table definition page");
    }

    const std::uint32_t definition_size = read_u32(file, page_number, first_page, definition_size_offset);
    const std::uint32_t row_count = read_u32(file, page_number, first_page, row_count_offset);
    const std::uint8_t table_type = first_page[table_type_offset];
    const std::uint16_t maximum_column_count =
        read_u16(file, page_number, first_page, maximum_column_count_offset);
    const std::uint16_t variable_column_count =
        read_u16(file, page_number, first_page, variable_column_count_offset);
    const std::uint16_t column_count = read_u16(file, page_number, first_page, column_count_offset);
    const std::uint32_t logical_index_count =
        read_u32(file, page_number, first_page, logical_index_count_offset);
    const std::uint32_t physical_index_count =
        read_u32(file, page_number, first_page, physical_index_count_offset);
    const UsageMapReference owned_pages{
        first_page[owned_pages_offset],
        read_u24(file, page_number, first_page, owned_pages_offset + 1),
    };
    const UsageMapReference free_space_pages{
        first_page[free_space_pages_offset],
        read_u24(file, page_number, first_page, free_space_pages_offset + 1),
    };

    if (definition_size < header_size) {
        corrupted_page(file, page_number, "Table definition is shorter than its fixed header");
    }
    if (maximum_column_count > maximum_columns_per_table || column_count > maximum_column_count ||
        variable_column_count > maximum_column_count) {
        corrupted_page(file, page_number, "Table definition has inconsistent column counts");
    }
    if (logical_index_count > maximum_indexes_per_table ||
        physical_index_count > logical_index_count) {
        corrupted_page(file, page_number, "Table definition has inconsistent index counts");
    }

    const std::uint64_t minimum_size = index_definition_offset +
                                       static_cast<std::uint64_t>(physical_index_count) * index_definition_size +
                                       static_cast<std::uint64_t>(column_count) * column_definition_size +
                                       static_cast<std::uint64_t>(column_count) * 2 +
                                       static_cast<std::uint64_t>(physical_index_count) * physical_index_definition_size +
                                       static_cast<std::uint64_t>(logical_index_count) * logical_index_definition_size +
                                       static_cast<std::uint64_t>(logical_index_count) * 2 +
                                       trailer_size;
    if (minimum_size > definition_size) {
        corrupted_page(file, page_number, "Table definition is too short for its declared columns and indexes");
    }

    std::uint64_t available_size = file.format().page_size;
    std::uint64_t next_page = read_u32(file, page_number, first_page, next_page_offset);
    std::vector<std::uint64_t> continuation_pages;
    std::unordered_set<std::uint64_t> visited_pages{page_number};
    while (next_page != 0) {
        if (next_page >= file.page_count()) {
            corrupted_page(file, page_number, "Table definition references a page outside the database");
        }
        if (!visited_pages.insert(next_page).second) {
            corrupted_page(file, next_page, "Table definition continuation contains a page cycle");
        }

        const std::vector<std::uint8_t> continuation = file.read_page(next_page);
        if (continuation[page_type_offset] != static_cast<std::uint8_t>(PageType::table_definition)) {
            corrupted_page(file, next_page, "Table definition continuation has an invalid page type");
        }
        continuation_pages.push_back(next_page);
        next_page = read_u32(file, next_page, continuation, next_page_offset);
        available_size += file.format().page_size - 8;
    }

    if (definition_size > available_size) {
        corrupted_page(file, page_number, "Table definition continuation chain is incomplete");
    }

    const std::uint64_t column_definition_offset =
        index_definition_offset + static_cast<std::uint64_t>(physical_index_count) * index_definition_size;
    const std::uint64_t column_name_offset =
        column_definition_offset + static_cast<std::uint64_t>(column_count) * column_definition_size;
    const std::uint64_t column_header_bytes =
        static_cast<std::uint64_t>(column_count) * column_definition_size;
    if (column_name_offset > definition_size || column_header_bytes > definition_size - column_definition_offset) {
        corrupted_page(file, page_number, "Column definitions extend beyond the table definition");
    }

    const auto column_headers = read_definition_span(file,
                                                     page_number,
                                                     continuation_pages,
                                                     column_definition_offset,
                                                     column_header_bytes);
    const auto named_columns = read_names(file,
                                          page_number,
                                          continuation_pages,
                                          definition_size,
                                          column_name_offset,
                                          column_count,
                                          maximum_column_name_bytes);

    std::vector<ColumnDefinition> columns;
    columns.reserve(column_count);
    for (std::uint16_t index = 0; index < column_count; ++index) {
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * column_definition_size;
        columns.push_back({
            named_columns.names[index],
            column_headers[offset],
            read_u16(file, page_number, column_headers, offset + 5),
            read_u16(file, page_number, column_headers, offset + 7),
            read_u16(file, page_number, column_headers, offset + 9),
            column_headers[offset + 11],
            column_headers[offset + 12],
            column_headers[offset + 15],
            column_headers[offset + 16],
            read_u16(file, page_number, column_headers, offset + 21),
            read_u16(file, page_number, column_headers, offset + 23),
        });
        auto& column = columns.back();
        column.calculated = (column.extended_flags & calculated_column_flags) != 0;
        column.complex_value_foreign_key =
            (column.extended_flags & complex_value_foreign_key_flag) != 0;
        if (column.type == complex_type && has_ace_complex_columns(file.format().version)) {
            column.complex_type_id =
                read_u32(file, page_number, column_headers, offset + ace_complex_id_offset);
        }
        if (column.type == text_type || column.type == memo_type) {
            const auto lcid = read_u16(file, page_number, column_headers, offset + 11);
            // Jet4 treats an absent LCID as General Legacy, even if the version byte is set.
            column.text_sort_order = lcid == 0
                ? Jet4TextSortOrder{1033, 0}
                : Jet4TextSortOrder{lcid, column_headers[offset + 14]};
        }
    }
    std::stable_sort(columns.begin(), columns.end(), [](const ColumnDefinition& left, const ColumnDefinition& right) {
        return left.column_number < right.column_number;
    });

    std::vector<PhysicalIndexDefinition> physical_indexes;
    physical_indexes.reserve(physical_index_count);
    std::uint64_t index_data_offset = named_columns.end_offset;
    for (std::uint32_t index = 0; index < physical_index_count; ++index) {
        if (index_data_offset > definition_size ||
            physical_index_definition_size > definition_size - index_data_offset) {
            corrupted_page(file, page_number, "Physical index definition extends beyond the table definition");
        }
        const auto encoded = read_definition_span(file,
                                                  page_number,
                                                  continuation_pages,
                                                  index_data_offset,
                                                  physical_index_definition_size);
        PhysicalIndexDefinition physical;
        physical.unique_entry_count = read_u32(file,
                                               page_number,
                                               first_page,
                                               index_definition_offset +
                                                   static_cast<std::uint64_t>(index) * index_definition_size + 4);
        physical.columns.reserve(maximum_index_columns);
        for (std::uint16_t column = 0; column < maximum_index_columns; ++column) {
            const std::uint64_t offset = 4 + static_cast<std::uint64_t>(column) * 3;
            const std::uint16_t column_number = read_u16(file, page_number, encoded, offset);
            const std::uint8_t column_flags = encoded[offset + 2];
            if (column_number == unused_index_column) {
                continue;
            }
            const auto defined_column = std::find_if(columns.begin(), columns.end(), [&](const auto& candidate) {
                return candidate.column_number == column_number;
            });
            if (defined_column == columns.end()) {
                corrupted_page(file, page_number, "Physical index references an undefined column number");
            }
            physical.columns.push_back({column_number, (column_flags & ascending_column_flag) != 0});
        }
        physical.owned_pages = {encoded[34], read_u24(file, page_number, encoded, 35)};
        physical.root_page_number = read_u32(file, page_number, encoded, 38);
        physical.flags = encoded[46];
        physical.extended_flags = encoded[47];
        physical_indexes.push_back(std::move(physical));
        index_data_offset += physical_index_definition_size;
    }

    const std::uint64_t logical_index_bytes =
        static_cast<std::uint64_t>(logical_index_count) * logical_index_definition_size;
    if (index_data_offset > definition_size || logical_index_bytes > definition_size - index_data_offset) {
        corrupted_page(file, page_number, "Logical index definitions extend beyond the table definition");
    }
    const auto encoded_logical_indexes = read_definition_span(file,
                                                              page_number,
                                                              continuation_pages,
                                                              index_data_offset,
                                                              logical_index_bytes);
    std::vector<LogicalIndexDefinition> indexes;
    indexes.reserve(logical_index_count);
    for (std::uint32_t index = 0; index < logical_index_count; ++index) {
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * logical_index_definition_size;
        const std::uint32_t index_number = read_u32(file, page_number, encoded_logical_indexes, offset + 4);
        const std::uint32_t index_data_number = read_u32(file, page_number, encoded_logical_indexes, offset + 8);
        if (index_data_number >= physical_index_count) {
            corrupted_page(file, page_number, "Logical index references an undefined physical index");
        }
        indexes.push_back({
            index_number,
            index_data_number,
            encoded_logical_indexes[offset + 12],
            signed_u32(read_u32(file, page_number, encoded_logical_indexes, offset + 13)),
            read_u32(file, page_number, encoded_logical_indexes, offset + 17),
            encoded_logical_indexes[offset + 21],
            encoded_logical_indexes[offset + 22],
            encoded_logical_indexes[offset + 23],
            {},
        });
    }
    index_data_offset += logical_index_bytes;

    const auto named_indexes = read_names(file,
                                          page_number,
                                          continuation_pages,
                                          definition_size,
                                          index_data_offset,
                                          logical_index_count,
                                          maximum_index_name_bytes);
    for (std::size_t index = 0; index < indexes.size(); ++index) {
        indexes[index].name = named_indexes.names[index];
    }

    const auto long_value_column_count = static_cast<std::uint64_t>(std::count_if(
        columns.begin(), columns.end(), [](const auto& column) {
            return column.type == ole_type || column.type == memo_type;
        }));
    std::uint64_t column_usage_map_offset = named_indexes.end_offset;
    bool column_usage_maps_valid = true;
    std::vector<std::pair<ColumnDefinition*, std::pair<UsageMapReference, UsageMapReference>>> parsed_column_maps;
    parsed_column_maps.reserve(static_cast<std::size_t>(long_value_column_count));
    for (std::uint64_t index = 0; index < long_value_column_count; ++index) {
        if (column_usage_map_offset > definition_size || 10 > definition_size - column_usage_map_offset) {
            column_usage_maps_valid = false;
            break;
        }
        const auto encoded = read_definition_span(file,
                                                  page_number,
                                                  continuation_pages,
                                                  column_usage_map_offset,
                                                  10);
        const std::uint16_t column_number = read_u16(file, page_number, encoded, 0);
        const auto column = std::find_if(columns.begin(), columns.end(), [&](const auto& candidate) {
            return candidate.column_number == column_number &&
                   (candidate.type == ole_type || candidate.type == memo_type);
        });
        if (column == columns.end() ||
            std::any_of(parsed_column_maps.begin(), parsed_column_maps.end(), [&](const auto& entry) {
                return entry.first == &*column;
            })) {
            column_usage_maps_valid = false;
            break;
        }
        parsed_column_maps.push_back({
            &*column,
            {UsageMapReference{encoded[2], read_u24(file, page_number, encoded, 3)},
             UsageMapReference{encoded[6], read_u24(file, page_number, encoded, 7)}}});
        column_usage_map_offset += 10;
    }
    if (column_usage_maps_valid && parsed_column_maps.size() == long_value_column_count) {
        for (const auto& entry : parsed_column_maps) {
            entry.first->long_value_owned_pages = entry.second.first;
            entry.first->long_value_free_space_pages = entry.second.second;
        }
    }

    return {
        definition_size,
        row_count,
        table_type,
        maximum_column_count,
        variable_column_count,
        column_count,
        logical_index_count,
        physical_index_count,
        continuation_pages.size(),
        owned_pages,
        free_space_pages,
        std::move(columns),
        std::move(physical_indexes),
        std::move(indexes),
    };
}

}

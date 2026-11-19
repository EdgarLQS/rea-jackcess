#include "table_definition.hpp"

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
constexpr std::uint64_t column_definition_size = 25;
constexpr std::uint64_t trailer_size = 2;
constexpr std::uint64_t index_definition_offset = 63;
constexpr std::uint16_t maximum_columns_per_table = 255;
constexpr std::uint32_t maximum_indexes_per_table = 32;
constexpr std::uint16_t maximum_column_name_bytes = 128;

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

std::vector<std::string> read_column_names(const PageFile& file,
                                           std::uint64_t first_page_number,
                                           const std::vector<std::uint64_t>& continuation_pages,
                                           std::uint64_t definition_size,
                                           std::uint64_t names_offset,
                                           std::uint16_t column_count) {
    std::vector<std::string> names;
    names.reserve(column_count);

    for (std::uint16_t index = 0; index < column_count; ++index) {
        if (names_offset > definition_size || 2 > definition_size - names_offset) {
            corrupted_page(file, first_page_number, "Column name length extends beyond the table definition");
        }
        const auto encoded_length =
            read_definition_span(file, first_page_number, continuation_pages, names_offset, 2);
        const std::uint16_t name_length = read_u16(file, first_page_number, encoded_length, 0);
        names_offset += 2;
        if (name_length == 0 || name_length > maximum_column_name_bytes || name_length % 2 != 0) {
            corrupted_page(file, first_page_number, "Column name has an invalid UTF-16LE byte length");
        }
        if (names_offset > definition_size || name_length > definition_size - names_offset) {
            corrupted_page(file, first_page_number, "Column name extends beyond the table definition");
        }

        const auto encoded_name =
            read_definition_span(file, first_page_number, continuation_pages, names_offset, name_length);
        names.push_back(decode_utf16le(encoded_name));
        names_offset += name_length;
    }

    return names;
}

}

TableDefinition read_table_definition(const PageFile& file, std::uint64_t page_number) {
    if (file.format().version != EngineVersion::jet4) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "Jet4 table definition layout requested for a different engine version",
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
    const auto column_names = read_column_names(file,
                                                page_number,
                                                continuation_pages,
                                                definition_size,
                                                column_name_offset,
                                                column_count);

    std::vector<ColumnDefinition> columns;
    columns.reserve(column_count);
    for (std::uint16_t index = 0; index < column_count; ++index) {
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * column_definition_size;
        columns.push_back({
            column_names[index],
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
    }
    std::stable_sort(columns.begin(), columns.end(), [](const ColumnDefinition& left, const ColumnDefinition& right) {
        return left.column_number < right.column_number;
    });

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
    };
}

}

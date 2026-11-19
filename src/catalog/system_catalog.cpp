#include "system_catalog.hpp"

#include "../page/data_page.hpp"
#include "../page/page_types.hpp"
#include "../value/text_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <set>
#include <string_view>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t row_column_count_size = 2;
constexpr std::uint64_t fixed_data_row_offset = 2;
constexpr std::uint8_t fixed_length_flag = 0x01;
constexpr std::uint8_t short_type = 0x03;
constexpr std::uint8_t long_type = 0x04;
constexpr std::uint8_t text_type = 0x0a;

[[noreturn]] void corrupted_row(const PageFile& file,
                                std::uint64_t page_number,
                                const char* reason) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, reason, file.path(), page_number);
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t read_u24(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16);
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

const ColumnDefinition& find_column(const PageFile& file,
                                   std::uint64_t page_number,
                                   const TableDefinition& definition,
                                   std::string_view name,
                                   std::uint8_t type) {
    const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
        return equals_ignore_case(candidate.name, name);
    });
    if (column == definition.columns.end() || column->type != type) {
        corrupted_row(file, page_number, "System catalog is missing a required column or has an unsupported type");
    }
    return *column;
}

bool row_has_non_null_value(const PageFile& file,
                            std::uint64_t page_number,
                            const DataPageRow& row,
                            const ColumnDefinition& column) {
    const auto& bytes = row.bytes;
    if (bytes.size() < row_column_count_size) {
        corrupted_row(file, page_number, "System catalog row is shorter than its column count");
    }

    const std::uint16_t row_column_count = read_u16(bytes, 0);
    const std::uint64_t null_mask_size = (static_cast<std::uint64_t>(row_column_count) + 7) / 8;
    if (null_mask_size > bytes.size() - row_column_count_size) {
        corrupted_row(file, page_number, "System catalog row has a truncated NULL mask");
    }
    const std::uint64_t null_mask_offset = bytes.size() - null_mask_size;
    if (column.column_number >= row_column_count) {
        return false;
    }
    const std::uint64_t mask_byte = null_mask_offset + column.column_number / 8;
    const std::uint8_t mask_bit = static_cast<std::uint8_t>(1u << (column.column_number % 8));
    if ((bytes[mask_byte] & mask_bit) == 0) {
        return false;
    }

    return true;
}

std::optional<std::vector<std::uint8_t>> read_column_bytes(const PageFile& file,
                                                          std::uint64_t page_number,
                                                          const DataPageRow& row,
                                                          const ColumnDefinition& column,
                                                          std::uint16_t variable_column_count) {
    if (!row_has_non_null_value(file, page_number, row, column)) {
        return std::nullopt;
    }
    const auto& bytes = row.bytes;
    const std::uint16_t row_column_count = read_u16(bytes, 0);
    const std::uint64_t null_mask_size = (static_cast<std::uint64_t>(row_column_count) + 7) / 8;
    const std::uint64_t null_mask_offset = bytes.size() - null_mask_size;

    std::uint64_t start = 0;
    std::uint64_t end = 0;
    if ((column.flags & fixed_length_flag) != 0) {
        start = fixed_data_row_offset + column.fixed_data_offset;
        end = start + column.length;
        if (end < start || end > null_mask_offset) {
            corrupted_row(file, page_number, "System catalog fixed column extends beyond row data");
        }
    } else {
        const std::uint64_t variable_index = column.variable_table_index;
        if (variable_index >= variable_column_count) {
            corrupted_row(file, page_number, "System catalog variable column index is outside the table schema");
        }
        if (null_mask_offset < 6 || variable_index > (null_mask_offset - 6) / 2) {
            corrupted_row(file, page_number, "System catalog variable column offsets are truncated");
        }
        const std::uint64_t offset_position = null_mask_offset - 4 - variable_index * 2;
        if (offset_position < 2) {
            corrupted_row(file, page_number, "System catalog variable column offsets are truncated");
        }
        start = read_u16(bytes, offset_position);
        end = read_u16(bytes, offset_position - 2);
        if (start > end || end > offset_position - 2 || start < row_column_count_size) {
            corrupted_row(file, page_number, "System catalog variable column offsets are invalid");
        }
    }

    return std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(end));
}

std::optional<std::int32_t> read_int32(const PageFile& file,
                                       std::uint64_t page_number,
                                       const DataPageRow& row,
                                       const ColumnDefinition& column,
                                       std::uint16_t variable_column_count) {
    auto bytes = read_column_bytes(file, page_number, row, column, variable_column_count);
    if (!bytes) {
        return std::nullopt;
    }
    if (bytes->size() != 4) {
        corrupted_row(file, page_number, "System catalog 32-bit integer has an invalid length");
    }
    const std::uint32_t value = read_u32(*bytes, 0);
    if (value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(value);
    }
    return static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(~value));
}

std::optional<std::int16_t> read_int16(const PageFile& file,
                                       std::uint64_t page_number,
                                       const DataPageRow& row,
                                       const ColumnDefinition& column,
                                       std::uint16_t variable_column_count) {
    auto bytes = read_column_bytes(file, page_number, row, column, variable_column_count);
    if (!bytes) {
        return std::nullopt;
    }
    if (bytes->size() != 2) {
        corrupted_row(file, page_number, "System catalog 16-bit integer has an invalid length");
    }
    const std::uint16_t value = read_u16(*bytes, 0);
    if (value <= static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())) {
        return static_cast<std::int16_t>(value);
    }
    return static_cast<std::int16_t>(-1 - static_cast<std::int32_t>(static_cast<std::uint16_t>(~value)));
}

std::optional<std::string> read_text(const PageFile& file,
                                     std::uint64_t page_number,
                                     const DataPageRow& row,
                                     const ColumnDefinition& column,
                                     std::uint16_t variable_column_count) {
    auto bytes = read_column_bytes(file, page_number, row, column, variable_column_count);
    if (!bytes) {
        return std::nullopt;
    }
    return decode_jet4_text(*bytes);
}

std::vector<std::uint8_t> read_complete_row(const PageFile& file,
                                           std::uint64_t start_page,
                                           const DataPageRow& start_row) {
    std::uint64_t page_number = start_page;
    std::uint16_t row_number = start_row.row_number;
    const DataPageRow* current_row = &start_row;
    DataPage current_page{start_page, {}};
    std::set<std::uint64_t> visited;

    while (true) {
        const std::uint64_t row_key = page_number * 256 + row_number;
        if (!visited.insert(row_key).second) {
            corrupted_row(file, page_number, "System catalog overflow row chain contains a cycle");
        }
        if (!current_row->overflow) {
            return current_row->bytes;
        }
        if (current_row->bytes.size() < 4) {
            corrupted_row(file, page_number, "System catalog overflow row pointer is truncated");
        }

        row_number = current_row->bytes[0];
        page_number = read_u24(current_row->bytes, 1);
        if (page_number == 0 || page_number >= file.page_count()) {
            corrupted_row(file, page_number, "System catalog overflow row points outside the database");
        }
        current_page = read_data_page(file, page_number);
        if (row_number >= current_page.rows.size()) {
            corrupted_row(file, page_number, "System catalog overflow row points outside its data page");
        }
        current_row = &current_page.rows[row_number];
    }
}

}

std::vector<SystemCatalogObject> read_system_catalog_objects(const PageFile& file,
                                                              const TableDefinition& definition) {
    if (file.format().version != EngineVersion::jet4) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "Jet4 system catalog layout requested for a different engine version",
                            file.path());
    }

    const std::uint64_t context_page = definition.owned_pages.page_number;
    const auto& id_column = find_column(file, context_page, definition, "Id", long_type);
    const auto& parent_id_column = find_column(file, context_page, definition, "ParentId", long_type);
    const auto& name_column = find_column(file, context_page, definition, "Name", text_type);
    const auto& type_column = find_column(file, context_page, definition, "Type", short_type);
    const auto& flags_column = find_column(file, context_page, definition, "Flags", long_type);
    const auto data_pages = read_usage_map_pages(file, definition.owned_pages);

    std::vector<SystemCatalogObject> objects;
    for (const std::uint64_t page_number : data_pages) {
        const auto page_bytes = file.read_page(page_number);
        if (page_bytes.empty() || page_bytes[0] != static_cast<std::uint8_t>(PageType::data)) {
            corrupted_row(file, page_number, "System catalog usage map references a non-data page");
        }
        const DataPage data_page = parse_data_page(file, page_number, page_bytes);
        for (const auto& row : data_page.rows) {
            if (row.deleted) {
                continue;
            }
            const auto complete_bytes = read_complete_row(file, page_number, row);
            const DataPageRow complete_row{row.row_number, 0, 0, false, false, complete_bytes};
            objects.push_back({
                read_int32(file, page_number, complete_row, id_column, definition.variable_column_count),
                read_int32(file, page_number, complete_row, parent_id_column, definition.variable_column_count),
                read_text(file, page_number, complete_row, name_column, definition.variable_column_count),
                read_int16(file, page_number, complete_row, type_column, definition.variable_column_count),
                read_int32(file, page_number, complete_row, flags_column, definition.variable_column_count),
            });
        }
    }
    return objects;
}

}

#include "row_reader.hpp"

#include "../page/data_page.hpp"
#include "usage_map.hpp"

#include <cstddef>
#include <optional>
#include <set>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t row_column_count_size = 2;
constexpr std::uint64_t fixed_data_row_offset = 2;

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u24(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16);
}

[[noreturn]] void corrupted_row(const PageFile& file,
                                const std::string& table_name,
                                std::uint64_t page_number,
                                const char* reason,
                                std::optional<std::uint16_t> row_number = std::nullopt,
                                const ColumnDefinition* column = nullptr) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    context.row_number = row_number;
    if (column != nullptr) {
        context.column_number = column->column_number;
        context.column_name = column->name;
    }
    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                        reason,
                        file.path(),
                        page_number,
                        std::move(context));
}

[[noreturn]] void rethrow_with_table_context(const PageFile& file,
                                             const std::string& table_name,
                                             const DatabaseError& error,
                                             std::optional<std::uint16_t> row_number = std::nullopt) {
    auto context = error.context();
    context.table_name = table_name;
    if (row_number && !context.row_number) {
        context.row_number = *row_number;
    }
    throw DatabaseError(error.code(), error.what(), file.path(), error.page_number(), std::move(context));
}

std::vector<std::uint8_t> read_complete_row(const PageFile& file,
                                           const std::string& table_name,
                                           std::uint64_t start_page,
                                           const DataPageRow& start_row) {
    std::uint64_t page_number = start_page;
    std::uint16_t row_number = start_row.row_number;
    DataPageRow current_row = start_row;
    std::set<std::pair<std::uint64_t, std::uint16_t>> visited;

    while (true) {
        if (!visited.emplace(page_number, row_number).second) {
            corrupted_row(file, table_name, page_number, "Table row overflow chain contains a cycle", row_number);
        }
        if (!current_row.overflow) {
            return current_row.bytes;
        }
        if (current_row.bytes.size() < 4) {
            corrupted_row(file, table_name, page_number, "Table row overflow pointer is truncated", row_number);
        }

        row_number = current_row.bytes[0];
        page_number = read_u24(current_row.bytes, 1);
        if (page_number == 0 || page_number >= file.page_count()) {
            corrupted_row(file, table_name, page_number, "Table row overflow pointer is outside the database", row_number);
        }
        const DataPage next_page = read_data_page(file, page_number);
        if (row_number >= next_page.rows.size()) {
            corrupted_row(file, table_name, page_number, "Table row overflow pointer is outside its data page", row_number);
        }
        current_row = next_page.rows[row_number];
    }
}

EncodedTableRow decode_row_layout(const PageFile& file,
                                  const std::string& table_name,
                                  std::uint64_t page_number,
                                  std::uint16_t row_number,
                                  const std::vector<std::uint8_t>& bytes,
                                  const TableDefinition& definition) {
    if (bytes.size() < row_column_count_size) {
        corrupted_row(file, table_name, page_number, "Table row is shorter than its column count", row_number);
    }

    const std::uint16_t row_column_count = read_u16(bytes, 0);
    if (row_column_count > definition.maximum_column_count) {
        corrupted_row(file, table_name, page_number, "Table row column count exceeds its table definition", row_number);
    }
    const std::uint64_t null_mask_size = (static_cast<std::uint64_t>(row_column_count) + 7) / 8;
    if (null_mask_size > bytes.size() - row_column_count_size) {
        corrupted_row(file, table_name, page_number, "Table row NULL mask is truncated", row_number);
    }
    const std::uint64_t null_mask_offset = bytes.size() - null_mask_size;

    std::vector<EncodedColumnValue> values;
    std::uint16_t highest_column_number = 0;
    for (const auto& column : definition.columns) {
        if (column.column_number > highest_column_number) {
            highest_column_number = column.column_number;
        }
    }
    if (!definition.columns.empty()) {
        values.resize(static_cast<std::size_t>(highest_column_number) + 1);
    }

    for (const auto& column : definition.columns) {
        if (column.column_number >= row_column_count) {
            continue;
        }
        const std::uint64_t mask_byte = null_mask_offset + column.column_number / 8;
        const std::uint8_t mask_bit = static_cast<std::uint8_t>(1u << (column.column_number % 8));
        const bool mask_value = (bytes[mask_byte] & mask_bit) != 0;
        auto& value = values[column.column_number];
        if (column.type == 0x01) {
            value.boolean_value = mask_value;
            continue;
        }
        if (!mask_value) {
            continue;
        }

        std::uint64_t start = 0;
        std::uint64_t end = 0;
        if ((column.flags & 0x01) != 0) {
            start = fixed_data_row_offset + column.fixed_data_offset;
            end = start + column.length;
            if (end < start || end > null_mask_offset) {
                corrupted_row(file, table_name, page_number,
                              "Table row fixed column extends beyond row data", row_number, &column);
            }
        } else {
            const std::uint64_t variable_index = column.variable_table_index;
            if (variable_index >= definition.variable_column_count) {
                corrupted_row(file, table_name, page_number,
                              "Table row variable column index is outside the table schema", row_number, &column);
            }
            if (null_mask_offset < 6 || variable_index > (null_mask_offset - 6) / 2) {
                corrupted_row(file, table_name, page_number,
                              "Table row variable column offsets are truncated", row_number, &column);
            }
            const std::uint64_t offset_position = null_mask_offset - 4 - variable_index * 2;
            if (offset_position < 2) {
                corrupted_row(file, table_name, page_number,
                              "Table row variable column offsets are truncated", row_number, &column);
            }
            start = read_u16(bytes, offset_position);
            end = read_u16(bytes, offset_position - 2);
            if (start > end || end > offset_position - 2 || start < row_column_count_size) {
                corrupted_row(file, table_name, page_number,
                              "Table row variable column offsets are invalid", row_number, &column);
            }
        }
        value.bytes = std::vector<std::uint8_t>(
            bytes.begin() + static_cast<std::ptrdiff_t>(start),
            bytes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    return {page_number, row_number, std::move(values)};
}

}

struct TableRowReader::Impl {
    Impl(const PageFile& source_file,
         std::string source_table_name,
         const TableDefinition& source_definition)
        : file(source_file),
          table_name(std::move(source_table_name)),
          definition(source_definition) {
        if (file.format().version != EngineVersion::jet4) {
            DatabaseErrorContext context;
            context.table_name = table_name;
            throw DatabaseError(DatabaseErrorCode::unsupported_version,
                                "Jet4 table row layout requested for a different engine version",
                                file.path(),
                                std::nullopt,
                                std::move(context));
        }
        if (definition.owned_pages.page_number == 0 && definition.row_count == 0) {
            return;
        }
        try {
            data_pages = read_usage_map_pages(file, definition.owned_pages);
        } catch (const DatabaseError& error) {
            rethrow_with_table_context(file, table_name, error);
        }
    }

    std::optional<EncodedTableRow> next() {
        while (true) {
            if (!current_page || next_row_index >= current_page->rows.size()) {
                if (next_page_index >= data_pages.size()) {
                    return std::nullopt;
                }
                const std::uint64_t page_number = data_pages[next_page_index++];
                try {
                    current_page = read_data_page(file, page_number);
                } catch (const DatabaseError& error) {
                    rethrow_with_table_context(file, table_name, error);
                }
                next_row_index = 0;
            }

            const DataPageRow& row = current_page->rows[next_row_index++];
            if (row.deleted) {
                continue;
            }
            std::vector<std::uint8_t> complete_bytes;
            try {
                complete_bytes = read_complete_row(file, table_name, current_page->page_number, row);
            } catch (const DatabaseError& error) {
                rethrow_with_table_context(file, table_name, error, row.row_number);
            }
            return decode_row_layout(file,
                                     table_name,
                                     current_page->page_number,
                                     row.row_number,
                                     complete_bytes,
                                     definition);
        }
    }

    EncodedTableRow read_at(std::uint64_t page_number, std::uint16_t row_number) const {
        DataPage page;
        try {
            page = read_data_page(file, page_number);
        } catch (const DatabaseError& error) {
            rethrow_with_table_context(file, table_name, error, row_number);
        }
        if (row_number >= page.rows.size()) {
            corrupted_row(file,
                          table_name,
                          page_number,
                          "Index row reference is outside its data page",
                          row_number);
        }

        const DataPageRow& row = page.rows[row_number];
        if (row.deleted) {
            corrupted_row(file,
                          table_name,
                          page_number,
                          "Index references a deleted table row",
                          row_number);
        }

        std::vector<std::uint8_t> complete_bytes;
        try {
            complete_bytes = read_complete_row(file, table_name, page_number, row);
        } catch (const DatabaseError& error) {
            rethrow_with_table_context(file, table_name, error, row_number);
        }
        return decode_row_layout(file,
                                 table_name,
                                 page_number,
                                 row_number,
                                 complete_bytes,
                                 definition);
    }

    const PageFile& file;
    std::string table_name;
    const TableDefinition& definition;
    std::vector<std::uint64_t> data_pages;
    std::size_t next_page_index = 0;
    std::optional<DataPage> current_page;
    std::size_t next_row_index = 0;
};

TableRowReader::TableRowReader(const PageFile& file,
                               std::string table_name,
                               const TableDefinition& definition)
    : impl_(std::make_unique<Impl>(file, std::move(table_name), definition)) {}

TableRowReader::~TableRowReader() = default;

TableRowReader::TableRowReader(TableRowReader&& other) noexcept = default;

TableRowReader& TableRowReader::operator=(TableRowReader&& other) noexcept = default;

std::optional<EncodedTableRow> TableRowReader::next() {
    if (!impl_) {
        return std::nullopt;
    }
    return impl_->next();
}

EncodedTableRow TableRowReader::read_at(std::uint64_t page_number, std::uint16_t row_number) const {
    if (!impl_) {
        throw std::logic_error("Table row reader has no state; it may have been moved from");
    }
    return impl_->read_at(page_number, row_number);
}

std::vector<EncodedTableRow> read_table_rows(const PageFile& file,
                                             const std::string& table_name,
                                             const TableDefinition& definition) {
    TableRowReader reader(file, table_name, definition);
    std::vector<EncodedTableRow> rows;
    while (auto row = reader.next()) {
        rows.push_back(std::move(*row));
    }
    return rows;
}

}

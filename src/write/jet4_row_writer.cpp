#include "jet4_row_writer.hpp"

#include "jet4_byte_order.hpp"
#include "jet4_global_usage_map.hpp"

#include "../catalog/table_definition.hpp"
#include "../catalog/usage_map.hpp"
#include "../page/data_page.hpp"
#include "../page/page_types.hpp"
#include "../page/page_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t data_page_row_count_offset = 12;
constexpr std::uint64_t data_page_row_directory_offset = 14;
constexpr std::uint64_t data_page_table_definition_offset = 4;
constexpr std::uint16_t data_page_row_offset_mask = 0x1fff;
constexpr std::uint8_t long_type = 0x04;

[[noreturn]] void throw_table_error(DatabaseErrorCode code,
                                   const PageFile& file,
                                   const std::string& table_name,
                                   const char* message,
                                   std::optional<std::uint64_t> page_number = std::nullopt) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    throw DatabaseError(code, message, file.path(), page_number, std::move(context));
}

[[noreturn]] void rethrow_with_table_context(const PageFile& file,
                                             const std::string& table_name,
                                             const DatabaseError& error) {
    auto context = error.context();
    if (!context.table_name) {
        context.table_name = table_name;
    }
    throw DatabaseError(error.code(), error.what(), file.path(), error.page_number(), std::move(context));
}

[[noreturn]] void unsupported_table_write(const PageFile& file,
                                          const std::string& table_name,
                                          const char* message) {
    throw_table_error(DatabaseErrorCode::unsupported_operation, file, table_name, message);
}

std::uint64_t fixed_row_data_size(const PageFile& file,
                                  const ReadableTableSchema& table,
                                  const TableDefinition& definition) {
    if (definition.maximum_column_count == 0 ||
        definition.maximum_column_count != definition.columns.size()) {
        unsupported_table_write(file,
                                table.name,
                                "Row insertion currently requires a dense table schema without deleted columns");
    }
    std::uint64_t data_size = 0;
    for (std::size_t index = 0; index < definition.columns.size(); ++index) {
        const auto& column = definition.columns[index];
        if (column.column_number != index) {
            unsupported_table_write(file,
                                    table.name,
                                    "Row insertion currently requires contiguous column numbers");
        }
        if (column.type != long_type || (column.flags & 0x01) == 0 || column.length != 4) {
            DatabaseErrorContext context;
            context.table_name = table.name;
            context.column_number = column.column_number;
            context.column_name = column.name;
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "Row insertion currently supports fixed LONG columns only",
                                file.path(),
                                table.definition_page_number,
                                std::move(context));
        }
        const std::uint64_t end = static_cast<std::uint64_t>(column.fixed_data_offset) + column.length;
        data_size = std::max(data_size, end);
    }
    return data_size;
}

std::vector<std::uint8_t> encode_row(const PageFile& file,
                                     const ReadableTableSchema& table,
                                     const TableDefinition& definition,
                                     const std::vector<Value>& values,
                                     std::uint64_t fixed_data_size) {
    if (values.size() != definition.columns.size()) {
        throw std::invalid_argument("Row value count does not match the table column count");
    }

    const std::uint64_t null_mask_size =
        (static_cast<std::uint64_t>(definition.maximum_column_count) + 7) / 8;
    const std::uint64_t row_size = 2 + fixed_data_size + null_mask_size;
    if (row_size > std::numeric_limits<std::uint16_t>::max()) {
        unsupported_table_write(file, table.name, "Encoded row exceeds Jet4 row offset limits");
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(row_size), 0);
    write_u16(row, 0, definition.maximum_column_count);

    for (std::size_t index = 0; index < definition.columns.size(); ++index) {
        const auto& column = definition.columns[index];
        const auto& value = values[index];
        if (std::holds_alternative<std::monostate>(value)) {
            if (column.nullable != std::optional<bool>(true)) {
                DatabaseErrorContext context;
                context.table_name = table.name;
                context.column_number = column.column_number;
                context.column_name = column.name;
                throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                    "Cannot insert NULL into a column that is not known to be nullable",
                                    file.path(),
                                    table.definition_page_number,
                                    std::move(context));
            }
            continue;
        }

        const auto* int32_value = std::get_if<std::int32_t>(&value);
        if (int32_value == nullptr) {
            DatabaseErrorContext context;
            context.table_name = table.name;
            context.column_number = column.column_number;
            context.column_name = column.name;
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "Row insertion currently accepts LONG values or NULL only",
                                file.path(),
                                table.definition_page_number,
                                std::move(context));
        }

        const std::uint64_t value_offset = 2 + column.fixed_data_offset;
        if (column.fixed_data_offset > fixed_data_size ||
            column.length > fixed_data_size - column.fixed_data_offset) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table.name,
                              "LONG column extends beyond its fixed row data",
                              table.definition_page_number);
        }
        write_u32(row, value_offset, static_cast<std::uint32_t>(*int32_value));
        const std::uint64_t null_mask_offset = row.size() - null_mask_size;
        row[null_mask_offset + column.column_number / 8] |=
            static_cast<std::uint8_t>(1u << (column.column_number % 8));
    }
    return row;
}

bool row_fits_on_data_page(const PageFile& file,
                           const std::string& table_name,
                           const std::vector<std::uint8_t>& page,
                           std::uint64_t row_size) {
    const std::uint16_t row_count = read_u16(page, data_page_row_count_offset);
    const std::uint16_t free_space = read_u16(page, 2);
    if (row_count > 255 || free_space > page.size() - data_page_row_directory_offset) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Data page row count or free space is invalid");
    }
    if (row_count == 255 || row_size + 2 > free_space) {
        return false;
    }

    const std::uint64_t directory_end =
        data_page_row_directory_offset + static_cast<std::uint64_t>(row_count + 1) * 2;
    const std::uint16_t previous_start = row_count == 0
                                             ? static_cast<std::uint16_t>(page.size())
                                             : static_cast<std::uint16_t>(
                                                   read_u16(page,
                                                            data_page_row_directory_offset +
                                                                static_cast<std::uint64_t>(row_count - 1) * 2) &
                                                   data_page_row_offset_mask);
    return row_size <= previous_start && previous_start - row_size >= directory_end;
}

void validate_data_page(const PageFile& file,
                        const std::string& table_name,
                        std::uint64_t page_number,
                        std::uint64_t definition_page_number,
                        const std::vector<std::uint8_t>& page) {
    (void)parse_data_page(file, page_number, page);
    if (read_u32(page, data_page_table_definition_offset) != definition_page_number) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Data page references a different table definition",
                          page_number);
    }
}

std::vector<std::uint8_t> new_data_page(std::uint32_t page_size,
                                        std::uint64_t definition_page_number) {
    std::vector<std::uint8_t> page(page_size, 0);
    page[0] = static_cast<std::uint8_t>(PageType::data);
    page[1] = 0x01;
    write_u16(page, 2, static_cast<std::uint16_t>(page_size - data_page_row_directory_offset));
    write_u32(page, data_page_table_definition_offset, static_cast<std::uint32_t>(definition_page_number));
    write_u16(page, data_page_row_count_offset, 0);
    return page;
}

std::uint16_t append_row_to_page(std::vector<std::uint8_t>& page,
                                const std::vector<std::uint8_t>& row) {
    const std::uint16_t row_count = read_u16(page, data_page_row_count_offset);
    const std::uint16_t previous_start = row_count == 0
                                             ? static_cast<std::uint16_t>(page.size())
                                             : static_cast<std::uint16_t>(
                                                   read_u16(page,
                                                            data_page_row_directory_offset +
                                                                static_cast<std::uint64_t>(row_count - 1) * 2) &
                                                   data_page_row_offset_mask);
    const std::uint16_t start = static_cast<std::uint16_t>(previous_start - row.size());
    std::copy(row.begin(), row.end(), page.begin() + start);
    write_u16(page, data_page_row_directory_offset + static_cast<std::uint64_t>(row_count) * 2, start);
    write_u16(page, data_page_row_count_offset, static_cast<std::uint16_t>(row_count + 1));
    const std::uint16_t free_space = read_u16(page, 2);
    write_u16(page, 2, static_cast<std::uint16_t>(free_space - row.size() - 2));
    return row_count;
}

void update_inline_usage_map_row(const PageFile& file,
                                 const std::string& table_name,
                                 std::vector<std::uint8_t>& usage_page,
                                 const UsageMapReference& reference,
                                 const std::set<std::uint64_t>& add_pages,
                                 const std::set<std::uint64_t>& remove_pages,
                                 bool convert_empty_reference_map) {
    const DataPage parsed = parse_data_page(file, reference.page_number, usage_page);
    if (reference.row_number >= parsed.rows.size()) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Table usage map references a missing declaration row",
                          reference.page_number);
    }
    const auto& declaration = parsed.rows[reference.row_number];
    if (declaration.deleted || declaration.overflow || declaration.bytes.size() < 5) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Table usage map declaration is invalid",
                          reference.page_number);
    }

    std::vector<std::uint8_t> bytes = declaration.bytes;
    if (bytes[0] == 1) {
        if (!convert_empty_reference_map ||
            std::any_of(bytes.begin() + 1, bytes.end(), [](std::uint8_t value) { return value != 0; })) {
            unsupported_table_write(file,
                                    table_name,
                                    "Writing to reference usage maps with allocated map pages is not yet supported");
        }
        std::fill(bytes.begin(), bytes.end(), 0);
        bytes[0] = 0;
        write_u32(bytes, 1, 0);
    } else if (bytes[0] != 0) {
        unsupported_table_write(file, table_name, "Unsupported table usage map type for row insertion");
    }

    const std::uint64_t bitmap_size = bytes.size() - 5;
    std::uint64_t first_page = read_u32(bytes, 1);
    std::set<std::uint64_t> pages;
    for (std::uint64_t byte_index = 0; byte_index < bitmap_size; ++byte_index) {
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((bytes[5 + byte_index] & static_cast<std::uint8_t>(1u << bit)) != 0) {
                pages.insert(first_page + byte_index * 8 + bit);
            }
        }
    }
    for (const std::uint64_t page_number : remove_pages) {
        pages.erase(page_number);
    }
    pages.insert(add_pages.begin(), add_pages.end());

    if (!pages.empty()) {
        const std::uint64_t new_first_page = (*pages.begin() / 8) * 8;
        const std::uint64_t last_page = *pages.rbegin();
        if (last_page - new_first_page >= bitmap_size * 8) {
            unsupported_table_write(file, table_name, "Table usage map expansion is not yet supported");
        }
        first_page = new_first_page;
    }
    write_u32(bytes, 1, static_cast<std::uint32_t>(first_page));
    std::fill(bytes.begin() + 5, bytes.end(), 0);
    for (const std::uint64_t page_number : pages) {
        const std::uint64_t bit_index = page_number - first_page;
        if (bit_index >= bitmap_size * 8) {
            unsupported_table_write(file, table_name, "Table usage map expansion is not yet supported");
        }
        bytes[5 + bit_index / 8] |= static_cast<std::uint8_t>(1u << (bit_index % 8));
    }
    std::copy(bytes.begin(), bytes.end(), usage_page.begin() + declaration.start_offset);
}

}

std::vector<RowId> append_jet4_rows(Jet4WriteContext& context,
                                    std::size_t table_index,
                                    const std::vector<std::vector<Value>>& rows) {
    if (rows.empty()) {
        return {};
    }
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to insert rows",
                            context.file.path());
    }
    std::lock_guard<std::mutex> write_lock(context.write_mutex);
    if (!context.write_state_valid) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database write state is invalid after a previous write failure; reopen and inspect the file",
                            context.file.path());
    }
    if (table_index >= context.schema.tables.size()) {
        throw std::out_of_range("Table schema index is outside the database catalog");
    }
    const auto& table = context.schema.tables[table_index];
    try {
        const auto& cached_definition = table.definition;
        const std::filesystem::path path = context.file.path();
        const PageFile file = PageFile::open(path);
        if (file.page_count() != context.file.page_count()) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Database file size changed outside this writable handle");
        }
        TableDefinition definition = read_table_definition(file, table.definition_page_number);
        if (definition.row_count != cached_definition.row_count) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Table row count changed outside this writable handle",
                              table.definition_page_number);
        }
        if (definition.columns.size() != cached_definition.columns.size()) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Table schema changed outside this writable handle",
                              table.definition_page_number);
        }
        for (std::size_t index = 0; index < definition.columns.size(); ++index) {
            auto& column = definition.columns[index];
            const auto& cached_column = cached_definition.columns[index];
            if (column.name != cached_column.name || column.column_number != cached_column.column_number ||
                column.type != cached_column.type || column.fixed_data_offset != cached_column.fixed_data_offset ||
                column.length != cached_column.length) {
                throw_table_error(DatabaseErrorCode::io_error,
                                  file,
                                  table.name,
                                  "Table schema changed outside this writable handle",
                                  table.definition_page_number);
            }
            column.nullable = cached_column.nullable;
        }
        if (!table.is_user_table || table.is_linked) {
            unsupported_table_write(file, table.name, "Rows can only be inserted into local user tables");
        }
        if (!definition.indexes.empty() || !definition.physical_indexes.empty()) {
            unsupported_table_write(file,
                                    table.name,
                                    "Row insertion currently does not maintain user indexes");
        }
        if (definition.owned_pages.page_number == 0 || definition.free_space_pages.page_number == 0 ||
            definition.owned_pages.page_number != definition.free_space_pages.page_number) {
            unsupported_table_write(file,
                                    table.name,
                                    "Row insertion currently requires the table usage maps to share one declaration page");
        }
        if (rows.size() > std::numeric_limits<std::uint32_t>::max() - definition.row_count) {
            unsupported_table_write(file, table.name, "Table row count would exceed Jet4 limits");
        }

        const std::uint64_t fixed_data_size = fixed_row_data_size(file, table, definition);
        std::vector<std::vector<std::uint8_t>> encoded_rows;
        encoded_rows.reserve(rows.size());
        for (const auto& row : rows) {
            encoded_rows.push_back(encode_row(file, table, definition, row, fixed_data_size));
        }
        const std::uint64_t row_size = encoded_rows.front().size();
        if (row_size + 2 > file.format().page_size - data_page_row_directory_offset) {
            unsupported_table_write(file, table.name, "Encoded row does not fit on a Jet4 data page");
        }

        const auto owned_page_list = read_usage_map_pages(file, definition.owned_pages);
        const auto free_page_list = read_usage_map_pages(file, definition.free_space_pages);
        const std::set<std::uint64_t> owned_pages(owned_page_list.begin(), owned_page_list.end());
        std::set<std::uint64_t> free_pages(free_page_list.begin(), free_page_list.end());
        std::set<std::uint64_t> removed_free_pages;
        std::map<std::uint64_t, std::vector<std::uint8_t>> data_pages;
        std::vector<std::uint64_t> appended_page_numbers;
        std::uint64_t next_page_number = file.page_count();
        std::optional<std::uint64_t> active_page_number;

        for (auto iterator = free_page_list.rbegin(); iterator != free_page_list.rend(); ++iterator) {
            const std::uint64_t page_number = *iterator;
            if (owned_pages.find(page_number) == owned_pages.end()) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Free-space map references a page not owned by the table",
                                  page_number);
            }
            auto page = file.read_page(page_number);
            validate_data_page(file, table.name, page_number, table.definition_page_number, page);
            if (!row_fits_on_data_page(file, table.name, page, row_size)) {
                free_pages.erase(page_number);
                removed_free_pages.insert(page_number);
                continue;
            }
            data_pages.emplace(page_number, std::move(page));
            active_page_number = page_number;
            break;
        }

        std::vector<RowId> row_ids;
        row_ids.reserve(encoded_rows.size());
        for (const auto& row : encoded_rows) {
            if (!active_page_number ||
                !row_fits_on_data_page(file, table.name, data_pages.at(*active_page_number), row.size())) {
                if (active_page_number) {
                    free_pages.erase(*active_page_number);
                    removed_free_pages.insert(*active_page_number);
                }
                if (next_page_number >= 0x1000000) {
                    unsupported_table_write(file, table.name, "Jet4 data page exceeds the three-byte page reference limit");
                }
                const std::uint64_t page_number = next_page_number++;
                data_pages.emplace(page_number,
                                   new_data_page(file.format().page_size, table.definition_page_number));
                appended_page_numbers.push_back(page_number);
                active_page_number = page_number;
            }

            auto& page = data_pages.at(*active_page_number);
            const std::uint16_t row_number = append_row_to_page(page, row);
            row_ids.push_back({*active_page_number, row_number});
            if (row_fits_on_data_page(file, table.name, page, row.size())) {
                free_pages.insert(*active_page_number);
                removed_free_pages.erase(*active_page_number);
            } else {
                free_pages.erase(*active_page_number);
                removed_free_pages.insert(*active_page_number);
            }
        }

        std::vector<std::uint64_t> newly_free_pages;
        for (const std::uint64_t page_number : appended_page_numbers) {
            if (free_pages.find(page_number) != free_pages.end()) {
                newly_free_pages.push_back(page_number);
            }
        }
        std::set<std::uint64_t> new_owned_pages(appended_page_numbers.begin(), appended_page_numbers.end());
        std::set<std::uint64_t> add_free_pages(newly_free_pages.begin(), newly_free_pages.end());

        auto updated_usage_page = file.read_page(definition.owned_pages.page_number);
        update_inline_usage_map_row(file,
                                    table.name,
                                    updated_usage_page,
                                    definition.owned_pages,
                                    new_owned_pages,
                                    {},
                                    true);
        update_inline_usage_map_row(file,
                                    table.name,
                                    updated_usage_page,
                                    definition.free_space_pages,
                                    add_free_pages,
                                    removed_free_pages,
                                    false);

        auto updated_definition_page = file.read_page(table.definition_page_number);
        write_u32(updated_definition_page,
                  16,
                  definition.row_count + static_cast<std::uint32_t>(rows.size()));
        std::vector<std::uint8_t> updated_global_usage_page;
        if (!appended_page_numbers.empty()) {
            updated_global_usage_page = mark_global_pages_allocated(file, appended_page_numbers);
        }

        bool writes_started = false;
        try {
            PageFileWriter writer = PageFileWriter::open(path);
            if (writer.page_count() != file.page_count()) {
                throw_table_error(DatabaseErrorCode::io_error,
                                  file,
                                  table.name,
                                  "Database page count changed before row pages were written");
            }
            writes_started = true;
            for (const std::uint64_t page_number : appended_page_numbers) {
                if (writer.append_page(data_pages.at(page_number)) != page_number) {
                    throw_table_error(DatabaseErrorCode::io_error,
                                      file,
                                      table.name,
                                      "Row data page was appended at an unexpected page number",
                                      page_number);
                }
            }
            for (const auto& entry : data_pages) {
                if (entry.first < file.page_count()) {
                    writer.write_page(entry.first, entry.second);
                }
            }
            writer.write_page(definition.owned_pages.page_number, updated_usage_page);
            if (!updated_global_usage_page.empty()) {
                writer.write_page(1, updated_global_usage_page);
            }
            writer.write_page(table.definition_page_number, updated_definition_page);
            writer.flush();
        } catch (const DatabaseError& error) {
            if (writes_started) {
                context.write_state_valid = false;
            }
            rethrow_with_table_context(file, table.name, error);
        } catch (...) {
            if (writes_started) {
                context.write_state_valid = false;
            }
            throw;
        }

        try {
            context.file = PageFile::open(path);
        } catch (const DatabaseError& error) {
            context.write_state_valid = false;
            rethrow_with_table_context(file, table.name, error);
        } catch (...) {
            context.write_state_valid = false;
            throw;
        }
        context.schema.tables[table_index].definition.row_count =
            definition.row_count + static_cast<std::uint32_t>(rows.size());
        return row_ids;
    } catch (const DatabaseError& error) {
        rethrow_with_table_context(context.file, table.name, error);
    }
}

}

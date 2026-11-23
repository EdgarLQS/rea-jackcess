#include "jet4_relationship_creator.hpp"

#include "jet4_global_usage_map.hpp"
#include "jet4_index_entry.hpp"

#include "../catalog/row_reader.hpp"
#include "../index/index_cursor.hpp"
#include "../page/page_types.hpp"
#include "../value/jet4_value.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t table_definition_header_size = 63;
constexpr std::uint64_t index_row_count_block_size = 12;
constexpr std::uint64_t column_definition_size = 25;
constexpr std::uint64_t physical_index_block_size = 52;
constexpr std::uint64_t logical_index_block_size = 28;
constexpr std::uint64_t table_definition_trailer_size = 2;
constexpr std::uint64_t definition_size_offset = 8;
constexpr std::uint64_t free_space_offset = 2;
constexpr std::uint64_t logical_index_count_offset = 47;
constexpr std::uint64_t physical_index_count_offset = 51;
constexpr std::uint64_t next_page_offset = 4;
constexpr std::uint32_t table_definition_magic = 0x659;
constexpr std::uint32_t physical_index_magic = 1923;
constexpr std::uint32_t maximum_indexes_per_table = 32;
constexpr std::size_t maximum_index_columns = 10;
constexpr std::size_t maximum_index_name_length = 64;
constexpr std::uint8_t foreign_index_type = 0x02;
constexpr std::uint8_t foreign_parent_type = 0x01;
constexpr std::uint8_t foreign_child_type = 0x02;
constexpr std::uint8_t cascade_update_flag = 0x01;
constexpr std::uint8_t cascade_delete_flag = 0x01;
constexpr std::uint8_t cascade_null_flag = 0x02;
constexpr std::uint32_t relation_one_to_one_flag = 0x0001;
constexpr std::uint32_t relation_dont_enforce_flag = 0x0002;
constexpr std::uint32_t relation_update_cascade_flag = 0x0100;
constexpr std::uint32_t relation_delete_cascade_flag = 0x1000;
constexpr std::uint32_t relation_delete_null_flag = 0x2000;
constexpr std::uint32_t relation_left_outer_flag = 0x01000000;
constexpr std::uint32_t relation_right_outer_flag = 0x02000000;

[[noreturn]] void throw_table_error(DatabaseErrorCode code,
                                    const PageFile& file,
                                    const std::string& table_name,
                                    const char* message) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    throw DatabaseError(code, message, file.path(), std::nullopt, std::move(context));
}

[[noreturn]] void unsupported_write(const PageFile& file,
                                    const std::string& table_name,
                                    const char* message) {
    throw_table_error(DatabaseErrorCode::unsupported_operation, file, table_name, message);
}

std::string identifier_key(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name;
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

void write_u16(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

bool is_ascii_name(const std::string& name);

struct IndexAddition {
    LogicalIndexDefinition logical;
    std::optional<PhysicalIndexDefinition> physical;
};

// Append the new blocks while retaining all existing descriptors, names and long-value maps.
// The definition is handled as one logical byte stream (the first page followed by every
// continuation page from offset 8), so a table whose definition already spans pages can grow
// again. Extra continuation pages are taken from `next_free_page` and reported in `new_pages`
// so the caller can register them in the global usage map.
DefinitionWrite definition_with_added_indexes(const PageFile& file,
                                               const std::string& table_name,
                                               const ReadableTableSchema& table,
                                               const TableDefinition& definition,
                                               const std::vector<IndexAddition>& additions,
                                               std::uint64_t next_free_page) {
    if (additions.empty() || definition.logical_index_count >= maximum_indexes_per_table ||
        additions.size() > maximum_indexes_per_table - definition.logical_index_count) {
        unsupported_write(file, table_name, "Table already declares the maximum number of indexes");
    }
    const std::uint64_t page_size = file.format().page_size;
    std::vector<std::uint8_t> stream = file.read_page(table.definition_page_number);
    std::vector<std::uint64_t> continuation_pages;
    std::uint32_t next_page = read_u32(stream, next_page_offset);
    while (next_page != 0) {
        const auto continuation = file.read_page(next_page);
        continuation_pages.push_back(next_page);
        stream.insert(stream.end(), continuation.begin() + 8, continuation.end());
        next_page = read_u32(continuation, next_page_offset);
    }

    const std::uint32_t definition_size = read_u32(stream, definition_size_offset);
    if (definition_size < table_definition_header_size ||
        definition_size > stream.size() ||
        definition.physical_indexes.size() != read_u32(stream, physical_index_count_offset) ||
        definition.logical_index_count != read_u32(stream, logical_index_count_offset)) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Table definition is inconsistent with its index block");
    }

    const auto physical_count = static_cast<std::uint32_t>(definition.physical_indexes.size());
    std::uint32_t physical_additions = 0;
    for (const auto& addition : additions) {
        const auto& index = addition.logical;
        if (addition.physical) {
            if (index.index_data_number != physical_count + physical_additions) {
                unsupported_write(file, table_name,
                                  "Logical index references an invalid physical index number");
            }
            ++physical_additions;
            const auto& physical = *addition.physical;
            if (physical.columns.empty() || physical.columns.size() > maximum_index_columns ||
                physical.owned_pages.page_number > 0xffffff) {
                unsupported_write(file, table_name,
                                  "Physical index cannot be represented in a Jet4 table definition");
            }
        } else if (index.index_data_number >= physical_count + physical_additions) {
            unsupported_write(file, table_name, "Logical index references an invalid physical index number");
        }
        if (index.name.empty() || index.name.size() > maximum_index_name_length ||
            !is_ascii_name(index.name)) {
            unsupported_write(file, table_name, "Index names must contain between one and 64 ASCII characters");
        }
    }
    const auto new_physical_count = physical_count + physical_additions;

    const std::uint64_t row_counts_end =
        table_definition_header_size + physical_count * index_row_count_block_size;
    std::uint64_t offset = row_counts_end + definition.columns.size() * column_definition_size;
    for (std::size_t column = 0; column < definition.columns.size(); ++column) {
        if (offset + 2 > definition_size) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Column name extends beyond the table definition");
        }
        const std::uint16_t length = read_u16(stream, offset);
        offset += 2 + length;
    }
    const std::uint64_t logical_start = offset + physical_count * physical_index_block_size;
    const std::uint64_t names_start =
        logical_start + static_cast<std::uint64_t>(definition.logical_index_count) * logical_index_block_size;
    if (names_start > definition_size) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Index block extends beyond the table definition");
    }
    std::uint64_t names_end = names_start;
    for (std::uint32_t index_position = 0; index_position < definition.logical_index_count; ++index_position) {
        if (names_end + 2 > definition_size) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Index name extends beyond the table definition");
        }
        const std::uint16_t length = read_u16(stream, names_end);
        if (length == 0 || length % 2 != 0) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Index name has an invalid UTF-16LE byte length");
        }
        names_end += 2 + length;
    }
    if (names_end + table_definition_trailer_size > definition_size ||
        stream[definition_size - 2] != 0xff || stream[definition_size - 1] != 0xff) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Table definition does not end with its expected trailer");
    }

    std::vector<std::vector<std::uint8_t>> logical_blocks;
    std::vector<std::vector<std::uint8_t>> name_entries;
    std::uint64_t name_entries_size = 0;
    for (const auto& addition : additions) {
        const auto& index = addition.logical;
        std::vector<std::uint8_t> block(logical_index_block_size, 0);
        write_u32(block, 0, table_definition_magic);
        write_u32(block, 4, index.index_number);
        write_u32(block, 8, index.index_data_number);
        block[12] = index.referenced_index_type;
        write_u32(block, 13, static_cast<std::uint32_t>(index.referenced_index_number));
        write_u32(block, 17, index.referenced_table_page_number);
        block[21] = index.cascade_updates_flags;
        block[22] = index.cascade_deletes_flags;
        block[23] = index.type;
        write_u32(block, 24, 0);
        logical_blocks.push_back(std::move(block));

        std::vector<std::uint8_t> name_entry;
        name_entry.reserve(2 + index.name.size() * 2);
        const auto encoded_length = static_cast<std::uint16_t>(index.name.size() * 2);
        name_entry.push_back(static_cast<std::uint8_t>(encoded_length & 0xff));
        name_entry.push_back(static_cast<std::uint8_t>((encoded_length >> 8) & 0xff));
        for (unsigned char character : index.name) {
            name_entry.push_back(character);
            name_entry.push_back(0);
        }
        name_entries_size += name_entry.size();
        name_entries.push_back(std::move(name_entry));
    }

    const std::uint64_t physical_size =
        static_cast<std::uint64_t>(physical_additions) *
        (index_row_count_block_size + physical_index_block_size);
    const auto logical_size = static_cast<std::uint64_t>(additions.size()) * logical_index_block_size;
    const auto new_definition_size = static_cast<std::uint32_t>(
        definition_size + physical_size + logical_size + name_entries_size);

    std::vector<std::uint8_t> rebuilt;
    rebuilt.reserve(stream.size() + physical_size + logical_size + name_entries_size);
    rebuilt.insert(rebuilt.end(), stream.begin(), stream.begin() + row_counts_end);
    for (const auto& addition : additions) {
        if (!addition.physical) {
            continue;
        }
        const auto count_offset = rebuilt.size();
        rebuilt.resize(count_offset + index_row_count_block_size, 0);
        write_u32(rebuilt, count_offset + 4, addition.physical->unique_entry_count);
    }
    rebuilt.insert(rebuilt.end(), stream.begin() + row_counts_end, stream.begin() + logical_start);
    for (const auto& addition : additions) {
        if (!addition.physical) {
            continue;
        }
        const auto& physical = *addition.physical;
        const auto physical_offset = rebuilt.size();
        rebuilt.resize(physical_offset + physical_index_block_size, 0);
        write_u32(rebuilt, physical_offset, physical_index_magic);
        for (std::size_t column = 0; column < maximum_index_columns; ++column) {
            const auto column_offset = physical_offset + 4 + column * 3;
            if (column >= physical.columns.size()) {
                write_u16(rebuilt, column_offset, 0xffff);
                continue;
            }
            const auto& indexed_column = physical.columns[column];
            write_u16(rebuilt, column_offset, indexed_column.column_number);
            rebuilt[column_offset + 2] = indexed_column.ascending ? 0x01 : 0;
        }
        rebuilt[physical_offset + 34] = physical.owned_pages.row_number;
        const auto map_page = physical.owned_pages.page_number;
        rebuilt[physical_offset + 35] = static_cast<std::uint8_t>(map_page);
        rebuilt[physical_offset + 36] = static_cast<std::uint8_t>(map_page >> 8);
        rebuilt[physical_offset + 37] = static_cast<std::uint8_t>(map_page >> 16);
        write_u32(rebuilt, physical_offset + 38, physical.root_page_number);
        rebuilt[physical_offset + 46] = physical.flags;
        rebuilt[physical_offset + 47] = physical.extended_flags;
    }
    rebuilt.insert(rebuilt.end(), stream.begin() + logical_start, stream.begin() + names_start);
    for (const auto& block : logical_blocks) {
        rebuilt.insert(rebuilt.end(), block.begin(), block.end());
    }
    rebuilt.insert(rebuilt.end(), stream.begin() + names_start, stream.begin() + names_end);
    for (const auto& name_entry : name_entries) {
        rebuilt.insert(rebuilt.end(), name_entry.begin(), name_entry.end());
    }
    rebuilt.insert(rebuilt.end(), stream.begin() + names_end, stream.begin() + definition_size);
    rebuilt.insert(rebuilt.end(), stream.begin() + new_definition_size, stream.end());

    write_u32(rebuilt, definition_size_offset, new_definition_size);
    write_u32(rebuilt, logical_index_count_offset,
              definition.logical_index_count + static_cast<std::uint32_t>(additions.size()));
    if (physical_additions != 0) {
        write_u32(rebuilt, physical_index_count_offset, new_physical_count);
    }

    // The first page carries the whole header and the beginning of the stream; each continuation
    // page contributes page_size - 8 bytes of stream after its own 8-byte header.
    const std::uint64_t continuation_capacity = page_size - 8;
    std::uint64_t required_continuations = 0;
    if (rebuilt.size() > page_size) {
        required_continuations = (rebuilt.size() - page_size + continuation_capacity - 1) /
                                 continuation_capacity;
    }

    DefinitionWrite write;
    write.next_free_page = next_free_page;
    std::vector<std::uint64_t> continuation_numbers = continuation_pages;
    while (continuation_numbers.size() < required_continuations) {
        continuation_numbers.push_back(write.next_free_page++);
        write.new_pages.push_back(continuation_numbers.back());
    }
    if (continuation_numbers.size() > required_continuations) {
        unsupported_write(file, table_name, "Table definition shrank below its existing continuation pages");
    }

    std::vector<std::uint8_t> first_page(page_size, 0);
    std::copy(rebuilt.begin(), rebuilt.begin() + static_cast<std::ptrdiff_t>(page_size),
              first_page.begin());
    write_u32(first_page, next_page_offset,
              continuation_numbers.empty() ? 0u : static_cast<std::uint32_t>(continuation_numbers.front()));
    const auto first_free_space = page_size - new_definition_size;
    write_u16(first_page, free_space_offset,
              static_cast<std::uint16_t>(first_free_space > 8 ? first_free_space - 8 : 0));
    write.pages.emplace_back(table.definition_page_number, std::move(first_page));

    for (std::size_t position = 0; position < continuation_numbers.size(); ++position) {
        const std::uint64_t begin = page_size + position * continuation_capacity;
        const std::uint64_t end = std::min<std::uint64_t>(begin + continuation_capacity, rebuilt.size());
        const std::uint64_t used = end - begin;
        std::vector<std::uint8_t> page(page_size, 0);
        page[0] = static_cast<std::uint8_t>(PageType::table_definition);
        page[1] = 0x01;
        const auto free_space = continuation_capacity - used;
        write_u16(page, free_space_offset,
                  static_cast<std::uint16_t>(free_space > 8 ? free_space - 8 : 0));
        write_u32(page, next_page_offset,
                  position + 1 < continuation_numbers.size()
                      ? static_cast<std::uint32_t>(continuation_numbers[position + 1])
                      : 0u);
        std::copy(rebuilt.begin() + static_cast<std::ptrdiff_t>(begin),
                  rebuilt.begin() + static_cast<std::ptrdiff_t>(end),
                  page.begin() + 8);
        write.pages.emplace_back(continuation_numbers[position], std::move(page));
    }
    return write;
}

const ColumnDefinition* find_column(const TableDefinition& definition, const std::string& name) {
    const auto key = identifier_key(name);
    const auto found = std::find_if(definition.columns.begin(), definition.columns.end(),
                                    [&](const auto& column) {
        return identifier_key(column.name) == key;
    });
    return found == definition.columns.end() ? nullptr : &*found;
}

bool index_matches(const PhysicalIndexDefinition& index,
                   const std::vector<const ColumnDefinition*>& columns) {
    if (index.columns.size() != columns.size()) {
        return false;
    }
    for (std::size_t position = 0; position < columns.size(); ++position) {
        if (index.columns[position].column_number != columns[position]->column_number) {
            return false;
        }
    }
    return true;
}

const LogicalIndexDefinition* unique_index_for(
    const TableDefinition& definition, const std::vector<const ColumnDefinition*>& columns) {
    const auto match = std::find_if(definition.indexes.begin(), definition.indexes.end(),
                                    [&](const auto& logical) {
        const auto& physical = definition.physical_indexes.at(logical.index_data_number);
        return (logical.type == 0x01 || (physical.flags & 0x01) != 0) &&
               index_matches(physical, columns);
    });
    return match == definition.indexes.end() ? nullptr : &*match;
}

void validate_existing_child_rows(const PageFile& file,
                                  const ReadableTableSchema& parent,
                                  const ReadableTableSchema& child,
                                  const TableDefinition& child_definition,
                                  const PhysicalIndexDefinition& parent_index,
                                  const std::vector<const ColumnDefinition*>& parent_columns,
                                  const std::vector<const ColumnDefinition*>& child_columns) {
    TableRowReader reader(file, child.name, child_definition);
    while (const auto row = reader.next()) {
        std::vector<Value> values;
        values.reserve(child_columns.size());
        for (const auto* column : child_columns) {
            values.push_back(decode_jet4_value(file, child.name, *row, *column));
        }
        if (std::all_of(values.begin(), values.end(), [](const auto& value) {
                return std::holds_alternative<std::monostate>(value);
            })) {
            continue;
        }
        std::vector<std::uint8_t> key;
        for (std::size_t position = 0; position < values.size(); ++position) {
            try {
                const auto part = encode_jet4_index_key(*parent_columns[position], values[position],
                                                        !parent_index.columns[position].ascending);
                key.insert(key.end(), part.begin(), part.end());
            } catch (const std::invalid_argument& error) {
                unsupported_write(file, child.name, error.what());
            }
        }
        key = truncate_jet4_index_key(std::move(key));
        IndexEntryCursor cursor(file, parent.name, parent.definition_page_number,
                                parent_index.root_page_number, key);
        const auto entry = cursor.next_entry();
        if (!entry || entry->key != key) {
            DatabaseErrorContext error_context;
            error_context.table_name = child.name;
            error_context.row_number = row->row_number;
            throw DatabaseError(DatabaseErrorCode::constraint_violation,
                                "Existing child row has no matching parent key",
                                file.path(), row->page_number, error_context);
        }
    }
}

const LogicalIndexDefinition* logical_for(const TableDefinition& definition, std::uint32_t index_data_number,
                                          std::uint8_t type) {
    const auto found = std::find_if(definition.indexes.begin(), definition.indexes.end(),
                                    [&](const auto& index) {
        return index.index_data_number == index_data_number && index.type == type;
    });
    return found == definition.indexes.end() ? nullptr : &*found;
}

std::string alias_name_for(const TableDefinition& definition,
                           std::string_view reserved_name = {}) {
    const auto reserved_key = identifier_key(std::string(reserved_name));
    for (char suffix = 'B'; suffix <= 'Z'; ++suffix) {
        std::string candidate = std::string(".r") + suffix;
        const auto key = identifier_key(candidate);
        const auto taken = key == reserved_key ||
                           std::any_of(definition.indexes.begin(), definition.indexes.end(),
                                       [&](const auto& index) {
                               return identifier_key(index.name) == key;
                           });
        if (!taken) {
            return candidate;
        }
    }
    return {};
}

bool is_ascii_name(const std::string& name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character < 0x80;
    });
}

}

DefinitionWrite definition_with_added_jet4_index(const PageFile& file,
                                                 const ReadableTableSchema& table,
                                                 const TableDefinition& definition,
                                                 const LogicalIndexDefinition& index,
                                                 const PhysicalIndexDefinition* physical,
                                                 std::uint64_t next_free_page) {
    std::vector<IndexAddition> additions;
    additions.push_back({index, physical ? std::optional<PhysicalIndexDefinition>(*physical)
                                         : std::nullopt});
    return definition_with_added_indexes(file, table.name, table, definition, additions,
                                         next_free_page);
}

// Merges a definition write into the staged pages and registers any appended continuation pages
// in the global usage map, so the write path only has to flush what is staged.
void stage_definition_write(const PageFile& file, Jet4StagedPages& staged, DefinitionWrite write) {
    for (auto& page : write.pages) {
        staged.pages[page.first] = std::move(page.second);
    }
    if (!write.new_pages.empty()) {
        // Appended pages are written in list order, so the continuation pages come before any
        // bitmap page the allocation itself has to create.
        staged.appended.insert(staged.appended.end(), write.new_pages.begin(), write.new_pages.end());
        const auto bitmap_pages = mark_global_pages_allocated(file, staged.pages, write.new_pages);
        staged.appended.insert(staged.appended.end(), bitmap_pages.begin(), bitmap_pages.end());
    }
}

std::vector<std::size_t> create_jet4_relationship(Jet4WriteContext& context, const RelationshipDefinition& definition) {
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to create relationships",
                            context.file.path());
    }
    std::lock_guard<std::recursive_mutex> write_lock(context.write_mutex);
    if (!context.write_state_valid) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database write state is invalid after a previous write failure; reopen and inspect the file",
                            context.file.path());
    }

    const auto find_table = [&](const std::string& name) -> std::optional<std::size_t> {
        const auto key = identifier_key(name);
        for (std::size_t position = 0; position < context.schema.tables.size(); ++position) {
            const auto& table = context.schema.tables[position];
            if (identifier_key(table.name) == key && !table.is_linked) {
                return position;
            }
        }
        return std::nullopt;
    };

    const PageFile file = PageFile::open(context.file.path());
    if (file.page_count() != context.file.page_count()) {
        throw_table_error(DatabaseErrorCode::io_error,
                          file,
                          definition.from_table,
                          "Database file size changed outside this writable handle");
    }
    const auto parent_position = find_table(definition.from_table);
    const auto child_position = find_table(definition.to_table);
    if (!parent_position || !child_position) {
        const auto references_linked_table = [&](const std::string& name) {
            const auto key = identifier_key(name);
            return std::any_of(context.schema.tables.begin(), context.schema.tables.end(), [&](const auto& table) {
                return table.is_linked && identifier_key(table.name) == key;
            });
        };
        if (references_linked_table(definition.from_table) || references_linked_table(definition.to_table)) {
            unsupported_write(file, definition.from_table, "Both relationship endpoints must be local tables");
        }
        throw std::invalid_argument("Two valid tables are required in relationship");
    }
    const bool self_reference = *parent_position == *child_position;
    const auto& parent = context.schema.tables[*parent_position];
    const auto& child = context.schema.tables[*child_position];
    if (!parent.is_user_table || !child.is_user_table) {
        unsupported_write(file, definition.from_table, "Relationships can only be created between user tables");
    }
    if (!is_ascii_name(definition.name) || definition.name.empty()) {
        throw std::invalid_argument("Relationship names must be non-empty ASCII text");
    }
    if (definition.columns.empty() ||
        definition.columns.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument("A relationship needs at least one column pair");
    }
    if (definition.referential_integrity && definition.columns.size() > maximum_index_columns) {
        throw std::invalid_argument("An integrity-enforced relationship cannot contain more than ten column pairs");
    }
    if (!definition.referential_integrity &&
        (definition.cascade_updates || definition.cascade_deletes || definition.cascade_null_on_delete)) {
        throw std::invalid_argument("Cascade flags cannot be enabled if referential integrity is not enforced");
    }

    const TableDefinition parent_definition = read_table_definition(file, parent.definition_page_number);
    const TableDefinition child_definition = read_table_definition(file, child.definition_page_number);
    const auto child_name_key = identifier_key(definition.name);
    if (definition.referential_integrity &&
        std::any_of(child_definition.indexes.begin(), child_definition.indexes.end(),
                    [&](const auto& index) { return identifier_key(index.name) == child_name_key; })) {
        unsupported_write(file, child.name, "The child table already has an index with that name");
    }

    std::vector<const ColumnDefinition*> parent_columns;
    std::vector<const ColumnDefinition*> child_columns;
    for (const auto& pair : definition.columns) {
        const auto* parent_column = find_column(parent_definition, pair.first);
        const auto* child_column = find_column(child_definition, pair.second);
        if (parent_column == nullptr || child_column == nullptr) {
            throw std::invalid_argument("Relationship references an unknown column");
        }
        if (parent_column->type != child_column->type) {
            throw std::invalid_argument("Matched relationship columns must have the same data type");
        }
        parent_columns.push_back(parent_column);
        child_columns.push_back(child_column);
    }

    // Referential integrity needs a unique index on the parent side and an index over the
    // foreign key columns on the child side. A relationship without it only adds catalog rows.
    const auto* parent_index = unique_index_for(parent_definition, parent_columns);
    const auto new_child_index_data = child_definition.physical_index_count;
    auto child_index_data = new_child_index_data;
    if (definition.referential_integrity) {
        if (parent_index == nullptr) {
            throw std::invalid_argument("Missing unique index on primary table required to enforce integrity");
        }
        const auto has_duplicate_column = [](const std::vector<const ColumnDefinition*>& columns) {
            std::set<std::uint32_t> seen;
            return std::any_of(columns.begin(), columns.end(), [&](const ColumnDefinition* column) {
                return !seen.insert(column->column_number).second;
            });
        };
        if (has_duplicate_column(parent_columns) || has_duplicate_column(child_columns)) {
            throw std::invalid_argument("Cannot have duplicate columns in an integrity enforced relationship");
        }
        for (std::uint32_t number = 0; number < new_child_index_data; ++number) {
            const auto& physical = child_definition.physical_indexes.at(number);
            if (index_matches(physical, child_columns) &&
                std::all_of(physical.columns.begin(), physical.columns.end(),
                            [](const auto& column) { return column.ascending; }) &&
                !logical_for(child_definition, number, foreign_index_type)) {
                child_index_data = number;
                break;
            }
        }
    }

    if (definition.referential_integrity) {
        validate_existing_child_rows(file, parent, child, child_definition,
                                     parent_definition.physical_indexes.at(parent_index->index_data_number),
                                     parent_columns, child_columns);
    }

    std::uint32_t flags = definition.referential_integrity ? 0 : relation_dont_enforce_flag;
    if (parent_index && unique_index_for(child_definition, child_columns)) {
        flags |= relation_one_to_one_flag;
    }
    if (definition.cascade_updates) {
        flags |= relation_update_cascade_flag;
    }
    if (definition.cascade_deletes) {
        flags |= relation_delete_cascade_flag;
    }
    if (definition.cascade_null_on_delete) {
        flags |= relation_delete_null_flag;
    }
    switch (definition.join_type) {
        case RelationshipJoinType::inner:
            break;
        case RelationshipJoinType::left_outer:
            flags |= relation_left_outer_flag;
            break;
        case RelationshipJoinType::right_outer:
            flags |= relation_right_outer_flag;
            break;
        default:
            throw std::invalid_argument("Unknown relationship join type");
    }
    std::vector<std::vector<Value>> rows;
    rows.reserve(definition.columns.size());
    for (std::size_t position = 0; position < definition.columns.size(); ++position) {
        rows.push_back({definition.name,
                        static_cast<std::int32_t>(flags),
                        static_cast<std::int32_t>(definition.columns.size()),
                        static_cast<std::int32_t>(position),
                        child.name,
                        definition.columns[position].second,
                        parent.name,
                        definition.columns[position].first});
    }

    std::optional<std::size_t> catalog_position;
    for (std::size_t position = 0; position < context.schema.tables.size(); ++position) {
        const auto& table = context.schema.tables[position];
        if (!table.is_user_table && identifier_key(table.name) == "msysrelationships") {
            catalog_position = position;
            break;
        }
    }
    if (!catalog_position) {
        unsupported_write(file, "MSysRelationships", "The relationship catalog table is unavailable");
    }

    if (definition.referential_integrity) {
        const auto alias_name = alias_name_for(parent_definition,
                                               self_reference ? definition.name : std::string_view{});
        if (alias_name.empty()) {
            unsupported_write(file, parent.name, "No relationship alias name is available for the parent table");
        }
        if ((self_reference &&
             parent_definition.logical_index_count + 2 > maximum_indexes_per_table) ||
            (!self_reference &&
             (parent_definition.logical_index_count >= maximum_indexes_per_table ||
              child_definition.logical_index_count >= maximum_indexes_per_table))) {
            unsupported_write(file, parent.name, "Table already declares the maximum number of indexes");
        }
        const auto next_index_number = [](const TableDefinition& table_definition) {
            std::uint32_t next = 0;
            for (const auto& index : table_definition.indexes) {
                next = std::max(next, index.index_number + 1);
            }
            return next;
        };
        const std::uint32_t parent_alias_number = next_index_number(parent_definition);
        const std::uint32_t child_index_number = self_reference
                                                     ? parent_alias_number + 1
                                                     : next_index_number(child_definition);
        const std::uint8_t update_flags = definition.cascade_updates ? cascade_update_flag : 0;
        const std::uint8_t delete_flags =
            static_cast<std::uint8_t>((definition.cascade_deletes ? cascade_delete_flag : 0) |
                                      (definition.cascade_null_on_delete ? cascade_null_flag : 0));

        Jet4StagedPages staged;
        PhysicalIndexDefinition child_physical{};
        const bool needs_child_index = child_index_data == new_child_index_data;
        if (needs_child_index) {
            for (const auto* column : child_columns) {
                child_physical.columns.push_back({column->column_number, true});
            }
            staged = stage_jet4_index(file, child, child_definition, child_physical, false);
        }
        if (self_reference) {
            std::vector<IndexAddition> additions;
            additions.push_back({
                {parent_alias_number, parent_index->index_data_number, foreign_parent_type,
                 static_cast<std::int32_t>(child_index_number),
                 static_cast<std::uint32_t>(child.definition_page_number), update_flags, delete_flags,
                 foreign_index_type, alias_name},
                std::nullopt});
            additions.push_back({
                {child_index_number, child_index_data, foreign_child_type,
                 static_cast<std::int32_t>(parent_alias_number),
                 static_cast<std::uint32_t>(parent.definition_page_number), update_flags, delete_flags,
                 foreign_index_type, definition.name},
                needs_child_index ? std::optional<PhysicalIndexDefinition>(child_physical)
                                  : std::nullopt});
            auto table_write = definition_with_added_indexes(
                file, parent.name, parent, parent_definition, additions,
                file.page_count() + staged.appended.size());
            stage_definition_write(file, staged, std::move(table_write));
            write_jet4_schema_pages(context, file, parent.name, staged, {});
            append_jet4_catalog_rows(context, *catalog_position, rows);
            return {*parent_position};
        }
        // Both descriptors are written together: a one-sided descriptor would fail the
        // reciprocal check that the row writer performs before every write. Each definition is
        // staged before the next one is laid out, because staging can append usage-map bitmap
        // pages and those take the page numbers that follow the definition's own pages.
        auto parent_write = definition_with_added_jet4_index(
            file, parent, parent_definition,
            {parent_alias_number, parent_index->index_data_number, foreign_parent_type,
             static_cast<std::int32_t>(child_index_number),
             static_cast<std::uint32_t>(child.definition_page_number), update_flags, delete_flags,
             foreign_index_type, alias_name}, nullptr,
            file.page_count() + staged.appended.size());
        stage_definition_write(file, staged, std::move(parent_write));
        auto child_write = definition_with_added_jet4_index(
            file, child, child_definition,
            {child_index_number, child_index_data, foreign_child_type,
             static_cast<std::int32_t>(parent_alias_number),
             static_cast<std::uint32_t>(parent.definition_page_number), update_flags, delete_flags,
             foreign_index_type, definition.name}, needs_child_index ? &child_physical : nullptr,
            file.page_count() + staged.appended.size());
        stage_definition_write(file, staged, std::move(child_write));
        write_jet4_schema_pages(context, file, parent.name, staged, {});
        append_jet4_catalog_rows(context, *catalog_position, rows);
        return {*parent_position, *child_position};
    }

    append_jet4_catalog_rows(context, *catalog_position, rows);
    return {};
}

}

#include "jet3_table_definition.hpp"

#include "../page/page_types.hpp"
#include "../util/code_page.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace rea_jackcess::detail {
namespace {

// Jet3 table-definition layout, taken from the fixed Jackcess 5.2.0 JetFormat.Jet3Format.
constexpr std::uint64_t header_size = 43;
constexpr std::uint64_t next_page_offset = 4;
constexpr std::uint64_t row_count_offset = 12;
constexpr std::uint64_t table_type_offset = 20;
constexpr std::uint64_t maximum_column_count_offset = 21;
constexpr std::uint64_t variable_column_count_offset = 23;
constexpr std::uint64_t column_count_offset = 25;
constexpr std::uint64_t logical_index_count_offset = 27;
constexpr std::uint64_t index_count_offset = 31;
constexpr std::uint64_t owned_pages_offset = 35;
constexpr std::uint64_t free_space_pages_offset = 39;
constexpr std::uint64_t index_definition_offset = 43;
constexpr std::uint64_t index_definition_size = 8;
constexpr std::uint64_t column_header_size = 18;
constexpr std::uint64_t physical_index_block_size = 39;
constexpr std::uint64_t logical_index_block_size = 20;
constexpr std::uint16_t maximum_index_columns = 10;
constexpr std::uint16_t unused_index_column = 0xffff;
constexpr std::uint8_t ascending_column_flag = 0x01;
constexpr std::uint64_t trailer_size = 2;
constexpr std::uint16_t maximum_columns_per_table = 255;
constexpr std::uint32_t maximum_indexes_per_table = 32;
constexpr std::uint16_t maximum_name_bytes = 64;
constexpr std::vector<std::uint8_t>::size_type continuation_header_size = 8;

// Column fields inside an 18-byte Jet3 column header.
constexpr std::uint64_t column_type_offset = 0;
constexpr std::uint64_t column_number_offset = 1;
constexpr std::uint64_t column_variable_index_offset = 3;
constexpr std::uint64_t column_id_offset = 5;
constexpr std::uint64_t column_sort_order_offset = 9;
constexpr std::uint64_t column_code_page_offset = 11;
constexpr std::uint64_t column_precision_offset = 11;
constexpr std::uint64_t column_scale_offset = 12;
constexpr std::uint64_t column_flags_offset = 13;
constexpr std::uint64_t column_fixed_offset_offset = 14;
constexpr std::uint64_t column_length_offset = 16;

[[noreturn]] void corrupted(const PageFile& file, std::uint64_t page_number, const char* reason) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, reason, file.path(), page_number);
}

void require(const PageFile& file,
             std::uint64_t page_number,
             std::uint64_t size,
             std::uint64_t offset,
             std::uint64_t count) {
    if (offset > size || count > size - offset) {
        corrupted(file, page_number, "Jet3 table definition field extends beyond its data");
    }
}

std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

// Collects the definition bytes of the first page and its continuation pages. Jet3 continuation
// pages skip an eight-byte header; the chain lives in the first four bytes of each page.
class DefinitionBuffer {
public:
    DefinitionBuffer(const PageFile& file, std::uint64_t first_page)
        : file_(file), first_page_(first_page) {
        std::uint64_t page_number = first_page;
        std::set<std::uint64_t> visited;
        while (page_number != 0) {
            if (page_number >= file_.page_count()) {
                corrupted(file_, first_page_, "Jet3 table definition continuation is outside the file");
            }
            if (!visited.insert(page_number).second) {
                corrupted(file_, first_page_, "Jet3 table definition continuation contains a cycle");
            }
            const auto page = file_.read_page(page_number);
            if (page.size() < continuation_header_size) {
                corrupted(file_, first_page_, "Jet3 table definition page is truncated");
            }
            const std::size_t begin = page_count_ == 0 ? 0 : continuation_header_size;
            pages_.insert(pages_.end(), page.begin() + static_cast<std::ptrdiff_t>(begin), page.end());
            ++page_count_;
            page_number = u32(page, next_page_offset);
        }
    }

    std::uint64_t size() const { return pages_.size(); }
    std::uint64_t continuation_pages() const { return page_count_ == 0 ? 0 : page_count_ - 1; }

    std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t count) const {
        require(file_, first_page_, pages_.size(), offset, count);
        return {pages_.begin() + static_cast<std::ptrdiff_t>(offset),
                pages_.begin() + static_cast<std::ptrdiff_t>(offset + count)};
    }

private:
    const PageFile& file_;
    std::uint64_t first_page_;
    std::vector<std::uint8_t> pages_;
    std::uint64_t page_count_ = 0;
};

}  // namespace

TableDefinition read_jet3_table_definition(const PageFile& file, std::uint64_t page_number) {
    if (file.format().version != EngineVersion::jet3) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "Jet3 table definition requested for a non-Jet3 file",
                            file.path(),
                            page_number);
    }

    const std::uint16_t code_page = file.code_page_override().value_or(1252);
    DefinitionBuffer buffer(file, page_number);
    const auto header = buffer.read(0, header_size);
    const std::vector<std::uint8_t>& first_page = header;
    if (first_page[0] != static_cast<std::uint8_t>(PageType::table_definition)) {
        corrupted(file, page_number, "Expected a Jet3 table definition page");
    }

    TableDefinition definition;
    definition.definition_size = static_cast<std::uint32_t>(buffer.size());
    definition.row_count = u32(first_page, row_count_offset);
    definition.table_type = first_page[table_type_offset];
    definition.maximum_column_count = u16(first_page, maximum_column_count_offset);
    definition.variable_column_count = u16(first_page, variable_column_count_offset);
    definition.column_count = u16(first_page, column_count_offset);
    definition.logical_index_count = u32(first_page, logical_index_count_offset);
    definition.physical_index_count = u32(first_page, index_count_offset);
    definition.continuation_page_count = buffer.continuation_pages();
    definition.owned_pages = {first_page[owned_pages_offset],
                              static_cast<std::uint32_t>(first_page[owned_pages_offset + 1]) |
                                  (static_cast<std::uint32_t>(first_page[owned_pages_offset + 2]) << 8) |
                                  (static_cast<std::uint32_t>(first_page[owned_pages_offset + 3]) << 16)};
    definition.free_space_pages = {first_page[free_space_pages_offset],
                                   static_cast<std::uint32_t>(first_page[free_space_pages_offset + 1]) |
                                       (static_cast<std::uint32_t>(first_page[free_space_pages_offset + 2]) << 8) |
                                       (static_cast<std::uint32_t>(first_page[free_space_pages_offset + 3]) << 16)};

    const std::uint32_t declared_indexes = u32(first_page, index_count_offset);
    if (definition.maximum_column_count > maximum_columns_per_table ||
        definition.column_count > definition.maximum_column_count ||
        definition.variable_column_count > definition.maximum_column_count) {
        corrupted(file, page_number, "Jet3 table definition has inconsistent column counts");
    }
    if (declared_indexes > maximum_indexes_per_table) {
        corrupted(file, page_number, "Jet3 table definition has an invalid index count");
    }

    const std::uint64_t column_offset =
        index_definition_offset + static_cast<std::uint64_t>(declared_indexes) * index_definition_size;
    const std::uint64_t names_offset =
        column_offset + static_cast<std::uint64_t>(definition.column_count) * column_header_size;

    std::uint64_t name_cursor = names_offset;
    definition.columns.reserve(definition.column_count);
    for (std::uint16_t index = 0; index < definition.column_count; ++index) {
        const auto block = buffer.read(column_offset + static_cast<std::uint64_t>(index) * column_header_size,
                                       column_header_size);
        const auto name_length_bytes = buffer.read(name_cursor, 1);
        const std::uint8_t name_length = name_length_bytes[0];
        ++name_cursor;
        if (name_length == 0 || name_length > maximum_name_bytes) {
            corrupted(file, page_number, "Jet3 column name has an invalid length");
        }
        const auto name_bytes = buffer.read(name_cursor, name_length);
        name_cursor += name_length;

        ColumnDefinition column;
        column.name = decode_code_page_text(name_bytes, code_page);
        column.type = block[column_type_offset];
        column.column_number = u16(block, column_number_offset);
        column.variable_table_index = u16(block, column_variable_index_offset);
        column.column_id = static_cast<std::uint16_t>(u32(block, column_id_offset));
        if (column.type == 0x0a || column.type == 0x04 || column.type == 0x03) {
            column.precision = block[column_precision_offset];
            column.scale = block[column_scale_offset];
        }
        column.flags = block[column_flags_offset];
        column.extended_flags = 0;
        column.fixed_data_offset = u16(block, column_fixed_offset_offset);
        column.length = u16(block, column_length_offset);
        (void)column_sort_order_offset;
        (void)column_code_page_offset;
        definition.columns.push_back(std::move(column));
    }

    if (definition.logical_index_count > maximum_indexes_per_table ||
        definition.physical_index_count > definition.logical_index_count) {
        corrupted(file, page_number, "Jet3 table definition has inconsistent index counts");
    }

    // Physical index blocks: 10 column triples, a four-byte usage map, the root page, and flags.
    std::uint64_t index_cursor = name_cursor;
    definition.physical_indexes.reserve(definition.physical_index_count);
    for (std::uint32_t index = 0; index < definition.physical_index_count; ++index) {
        const auto block = buffer.read(index_cursor, physical_index_block_size);
        PhysicalIndexDefinition physical;
        for (std::uint16_t slot = 0; slot < maximum_index_columns; ++slot) {
            const std::uint64_t offset = static_cast<std::uint64_t>(slot) * 3;
            const std::uint16_t column_number = u16(block, offset);
            const std::uint8_t column_flags = block[offset + 2];
            if (column_number == unused_index_column) {
                continue;
            }
            const bool known = std::any_of(definition.columns.begin(), definition.columns.end(),
                                           [&](const auto& column) {
                                               return column.column_number == column_number;
                                           });
            if (!known) {
                corrupted(file, page_number, "Jet3 index references an undefined column number");
            }
            physical.columns.push_back({column_number, (column_flags & ascending_column_flag) != 0});
        }
        const std::uint64_t usage_map_offset = static_cast<std::uint64_t>(maximum_index_columns) * 3;
        physical.owned_pages = {block[usage_map_offset],
                                static_cast<std::uint32_t>(block[usage_map_offset + 1]) |
                                    (static_cast<std::uint32_t>(block[usage_map_offset + 2]) << 8) |
                                    (static_cast<std::uint32_t>(block[usage_map_offset + 3]) << 16)};
        physical.root_page_number = u32(block, usage_map_offset + 4);
        physical.flags = block[usage_map_offset + 8];
        physical.extended_flags = 0;
        definition.physical_indexes.push_back(std::move(physical));
        index_cursor += physical_index_block_size;
    }

    // Logical index blocks: two numbers, the foreign-key reference, and cascade flags.
    std::vector<std::string> index_names;
    index_names.reserve(definition.logical_index_count);
    for (std::uint32_t index = 0; index < definition.logical_index_count; ++index) {
        const auto block = buffer.read(index_cursor, logical_index_block_size);
        LogicalIndexDefinition logical;
        logical.index_number = u32(block, 0);
        logical.index_data_number = u32(block, 4);
        logical.referenced_index_type = block[8];
        logical.referenced_index_number = static_cast<std::int32_t>(u32(block, 9));
        logical.referenced_table_page_number = u32(block, 13);
        logical.cascade_updates_flags = block[17];
        logical.cascade_deletes_flags = block[18];
        logical.type = block[19];
        definition.indexes.push_back(std::move(logical));
        index_cursor += logical_index_block_size;
    }
    // Names follow all logical blocks, mirroring TableImpl.readIndexDefinitions.
    for (auto& logical : definition.indexes) {
        const auto length_bytes = buffer.read(index_cursor, 1);
        const std::uint8_t name_length = length_bytes[0];
        ++index_cursor;
        if (name_length == 0 || name_length > maximum_name_bytes) {
            corrupted(file, page_number, "Jet3 index name has an invalid length");
        }
        const auto name_bytes = buffer.read(index_cursor, name_length);
        index_cursor += name_length;
        logical.name = decode_code_page_text(name_bytes, code_page);
    }

    if (index_cursor + trailer_size > buffer.size()) {
        corrupted(file, page_number, "Jet3 table definition trailer is truncated");
    }
    return definition;
}

}  // namespace rea_jackcess::detail

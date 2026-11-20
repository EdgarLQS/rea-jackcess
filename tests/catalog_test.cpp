#include "catalog/system_catalog.hpp"
#include "catalog/property_map.hpp"
#include "catalog/table_catalog.hpp"
#include "catalog/table_definition.hpp"
#include "catalog/usage_map.hpp"
#include "canonical_json.hpp"
#include "format/jet_format.hpp"
#include "page/page_file.hpp"
#include "rea_jackcess/database.hpp"
#include "util/sha256.hpp"
#include "value/jet4_value.hpp"
#include "value/text_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t page_size = 4096;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write_u24(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void write_u24_be(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value);
}

void write_u32_be(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 4);
    write_u32(bytes, offset, value);
}

std::vector<std::uint8_t> utf16le(const std::u16string& text) {
    std::vector<std::uint8_t> bytes;
    for (const char16_t character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
        bytes.push_back(static_cast<std::uint8_t>(character >> 8));
    }
    return bytes;
}

void append_text_property_map(std::vector<std::uint8_t>& bytes,
                              std::uint16_t map_type,
                              const std::u16string& map_name,
                              const std::string& value) {
    const std::size_t block_start = bytes.size();
    append_u32(bytes, 0);
    append_u16(bytes, map_type);

    const std::size_t map_name_start = bytes.size();
    append_u32(bytes, 0);
    const auto encoded_map_name = utf16le(map_name);
    append_u16(bytes, static_cast<std::uint16_t>(encoded_map_name.size()));
    bytes.insert(bytes.end(), encoded_map_name.begin(), encoded_map_name.end());
    write_u32(bytes,
              map_name_start,
              static_cast<std::uint32_t>(bytes.size() - map_name_start));

    const auto encoded_value = utf16le(std::u16string(value.begin(), value.end()));
    append_u16(bytes, static_cast<std::uint16_t>(8 + encoded_value.size()));
    bytes.push_back(0);
    bytes.push_back(0x0a);
    append_u16(bytes, 0);
    append_u16(bytes, static_cast<std::uint16_t>(encoded_value.size()));
    bytes.insert(bytes.end(), encoded_value.begin(), encoded_value.end());
    write_u32(bytes, block_start, static_cast<std::uint32_t>(bytes.size() - block_start));
}

struct TestColumn {
    std::u16string name;
    std::uint8_t type;
    std::uint8_t flags;
    std::uint16_t variable_index;
    std::uint16_t fixed_offset;
    std::uint16_t length;
};

void write_data_page(std::vector<std::uint8_t>& file_bytes,
                     std::uint32_t page_number,
                     const std::vector<std::vector<std::uint8_t>>& rows,
                     const std::vector<std::uint16_t>& row_flags) {
    std::vector<std::uint8_t> page(page_size, 0);
    page[0] = 0x01;
    write_u16(page, 12, static_cast<std::uint16_t>(rows.size()));
    std::size_t row_end = page_size;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        check(rows[index].size() <= row_end, "fixture row is larger than its page");
        const std::size_t row_start = row_end - rows[index].size();
        check(row_start >= 14 + rows.size() * 2, "fixture rows overlap the row directory");
        std::copy(rows[index].begin(), rows[index].end(), page.begin() + static_cast<std::ptrdiff_t>(row_start));
        write_u16(page, 14 + index * 2,
                  static_cast<std::uint16_t>(row_start) | row_flags[index]);
        row_end = row_start;
    }
    const std::size_t offset = static_cast<std::size_t>(page_number) * page_size;
    std::copy(page.begin(), page.end(), file_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<std::uint8_t> make_catalog_row(std::uint32_t id,
                                           std::uint32_t parent_id,
                                           std::uint16_t type,
                                           std::uint32_t flags,
                                           const std::vector<std::uint8_t>& name) {
    std::vector<std::uint8_t> row(16, 0);
    write_u16(row, 0, 5);
    write_u32(row, 2, id);
    write_u32(row, 6, parent_id);
    write_u16(row, 10, type);
    write_u32(row, 12, flags);
    row.insert(row.end(), name.begin(), name.end());
    const auto end_of_data = static_cast<std::uint16_t>(row.size());
    append_u16(row, end_of_data);
    append_u16(row, 16);
    append_u16(row, 1);
    row.push_back(0x1f);
    return row;
}

std::filesystem::path make_database_fixture() {
    static std::uint64_t sequence = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("rea-jackcess-catalog-" + std::to_string(++sequence) + ".mdb");
    std::vector<std::uint8_t> file_bytes(page_size * 6, 0);
    const std::string engine_name = "Standard Jet DB";
    std::copy(engine_name.begin(), engine_name.end(), file_bytes.begin() + 4);
    file_bytes[20] = 1;

    const std::array<TestColumn, 5> columns{{
        {u"Id", 0x04, 0x01, 0, 0, 4},
        {u"ParentId", 0x04, 0x01, 0, 4, 4},
        {u"Name", 0x0a, 0x12, 0, 0, 255},
        {u"Type", 0x03, 0x01, 0, 8, 2},
        {u"Flags", 0x04, 0x01, 0, 10, 4},
    }};
    constexpr std::size_t row_count_definitions_size = 12;
    constexpr std::size_t physical_index_definition_size = 52;
    constexpr std::size_t logical_index_definition_size = 28;
    const auto index_name = utf16le(u"Id");
    std::size_t definition_size = 63 + row_count_definitions_size + columns.size() * 25 +
                                  physical_index_definition_size + logical_index_definition_size +
                                  2 + index_name.size() + 2;
    for (const auto& column : columns) {
        definition_size += 2 + column.name.size() * 2;
    }
    std::vector<std::uint8_t> definition(page_size, 0);
    definition[0] = 0x02;
    write_u32(definition, 8, static_cast<std::uint32_t>(definition_size));
    write_u32(definition, 16, 2);
    definition[40] = 83;
    write_u16(definition, 41, static_cast<std::uint16_t>(columns.size()));
    write_u16(definition, 43, 1);
    write_u16(definition, 45, static_cast<std::uint16_t>(columns.size()));
    write_u32(definition, 47, 1);
    write_u32(definition, 51, 1);
    definition[55] = 0;
    write_u24(definition, 56, 3);
    definition[59] = 0;
    write_u24(definition, 60, 3);

    write_u32(definition, 67, 2);
    std::size_t names_offset = 63 + row_count_definitions_size + columns.size() * 25;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const std::size_t header_offset = 63 + row_count_definitions_size + index * 25;
        const auto encoded_name = utf16le(columns[index].name);
        definition[header_offset] = columns[index].type;
        write_u16(definition, header_offset + 5, static_cast<std::uint16_t>(index));
        write_u16(definition, header_offset + 7, columns[index].variable_index);
        write_u16(definition, header_offset + 9, static_cast<std::uint16_t>(index + 1));
        definition[header_offset + 15] = columns[index].flags;
        write_u16(definition, header_offset + 21, columns[index].fixed_offset);
        write_u16(definition, header_offset + 23, columns[index].length);
        write_u16(definition, names_offset, static_cast<std::uint16_t>(encoded_name.size()));
        names_offset += 2;
        std::copy(encoded_name.begin(), encoded_name.end(), definition.begin() + static_cast<std::ptrdiff_t>(names_offset));
        names_offset += encoded_name.size();
    }
    const std::size_t physical_index_offset = names_offset;
    write_u16(definition, physical_index_offset + 4, 0);
    definition[physical_index_offset + 6] = 0x01;
    for (std::size_t index = 1; index < 10; ++index) {
        write_u16(definition, physical_index_offset + 4 + index * 3, 0xffff);
    }
    write_u24(definition, physical_index_offset + 35, 3);
    definition[physical_index_offset + 46] = 0x01;

    const std::size_t logical_index_offset = physical_index_offset + physical_index_definition_size;
    write_u32(definition, logical_index_offset + 4, 0);
    write_u32(definition, logical_index_offset + 8, 0);
    write_u32(definition, logical_index_offset + 13, 0xffffffffu);
    write_u32(definition, logical_index_offset + 17, 0xffffffffu);
    definition[logical_index_offset + 23] = 0x01;
    const std::size_t index_name_offset = logical_index_offset + logical_index_definition_size;
    write_u16(definition, index_name_offset, static_cast<std::uint16_t>(index_name.size()));
    std::copy(index_name.begin(), index_name.end(),
              definition.begin() + static_cast<std::ptrdiff_t>(index_name_offset + 2));
    std::vector<std::uint8_t> user_definition(page_size, 0);
    user_definition[0] = 0x02;
    write_u32(user_definition, 8, 65);
    user_definition[40] = 0x4e;
    std::copy(user_definition.begin(), user_definition.end(), file_bytes.begin() + static_cast<std::ptrdiff_t>(page_size));
    std::copy(definition.begin(), definition.end(), file_bytes.begin() + static_cast<std::ptrdiff_t>(2 * page_size));

    const std::vector<std::uint8_t> usage_map{0x00, 0, 0, 0, 0, 0x30};
    write_data_page(file_bytes, 3, {usage_map}, {0});
    const std::vector<std::uint8_t> overflow_pointer{0, 5, 0, 0};
    const auto second_row = make_catalog_row(
        2, 0, 1, 0x02u, {0xff, 0xfe, 'A', 0, 0x2d, 0x4e, 0, 'B'});
    write_data_page(file_bytes, 4, {overflow_pointer, second_row}, {0x4000, 0});
    const auto overflow_row = make_catalog_row(1, 0, 1, 0, utf16le(u"用户表"));
    write_data_page(file_bytes, 5, {overflow_row}, {0x8000});

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file_bytes.data()), static_cast<std::streamsize>(file_bytes.size()));
    check(static_cast<bool>(output), "failed to write the catalog fixture");
    return path;
}

std::filesystem::path make_indexed_database_fixture() {
    const auto path = make_database_fixture();
    std::vector<std::uint8_t> file_bytes(page_size * 9, 0);
    {
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char*>(file_bytes.data()),
                   static_cast<std::streamsize>(page_size * 6));
        check(input.gcount() == static_cast<std::streamsize>(page_size * 6),
              "failed to read the base index fixture");
    }

    constexpr std::size_t table_definition_page = 2;
    constexpr std::size_t physical_index_definition_offset = 256;
    constexpr std::size_t index_entry_data_offset = 480;
    write_u32(file_bytes, table_definition_page * page_size + 16, 3);
    file_bytes[table_definition_page * page_size + physical_index_definition_offset + 34] = 1;
    write_u24(file_bytes,
              table_definition_page * page_size + physical_index_definition_offset + 35,
              3);
    write_u32(file_bytes,
              table_definition_page * page_size + physical_index_definition_offset + 38,
              6);

    write_data_page(file_bytes,
                    3,
                    {{0x00, 0, 0, 0, 0, 0x30},
                     {0x00, 0, 0, 0, 0, 0xc0, 0x01}},
                    {0, 0});
    const std::vector<std::uint8_t> overflow_pointer{0, 5, 0, 0};
    const auto second_row = make_catalog_row(
        2, 0, 1, 0x02u, {0xff, 0xfe, 'A', 0, 0x2d, 0x4e, 0, 'B'});
    const auto ignored_query = make_catalog_row(3, 0, 5, 0, utf16le(u"IgnoredQuery"));
    write_data_page(file_bytes,
                    4,
                    {overflow_pointer, second_row, ignored_query},
                    {0x4000, 0, 0});

    std::vector<std::uint8_t> root_page(page_size, 0);
    root_page[0] = 0x03;
    write_u16(root_page, 2, static_cast<std::uint16_t>(page_size - index_entry_data_offset - 9));
    write_u32(root_page, 4, static_cast<std::uint32_t>(table_definition_page));
    write_u32(root_page, 20, 8);
    write_u16(root_page, 24, 0);
    root_page[26] = 1;
    root_page[27 + 1] |= 1u << 1;
    root_page[index_entry_data_offset] = 0x18;
    write_u24_be(root_page, index_entry_data_offset + 1, 4);
    root_page[index_entry_data_offset + 4] = 0;
    write_u32_be(root_page, index_entry_data_offset + 5, 7);
    std::copy(root_page.begin(), root_page.end(),
              file_bytes.begin() + static_cast<std::ptrdiff_t>(6 * page_size));

    std::vector<std::uint8_t> first_leaf(page_size, 0);
    first_leaf[0] = 0x04;
    write_u16(first_leaf, 2, static_cast<std::uint16_t>(page_size - index_entry_data_offset - 11));
    write_u32(first_leaf, 4, static_cast<std::uint32_t>(table_definition_page));
    write_u32(first_leaf, 16, 8);
    write_u16(first_leaf, 24, 1);
    first_leaf[26] = 0;
    first_leaf[27] |= 1u << 6;
    first_leaf[27 + 1] |= 1u << 3;
    const std::array<std::uint8_t, 11> first_leaf_entries{
        0x10, 0x01, 0x00, 0x00, 0x04, 0x00,
        0x02, 0x00, 0x00, 0x04, 0x01,
    };
    std::copy(first_leaf_entries.begin(), first_leaf_entries.end(),
              first_leaf.begin() + static_cast<std::ptrdiff_t>(index_entry_data_offset));
    std::copy(first_leaf.begin(), first_leaf.end(),
              file_bytes.begin() + static_cast<std::ptrdiff_t>(7 * page_size));

    std::vector<std::uint8_t> last_leaf(page_size, 0);
    last_leaf[0] = 0x04;
    write_u16(last_leaf, 2, static_cast<std::uint16_t>(page_size - index_entry_data_offset - 6));
    write_u32(last_leaf, 4, static_cast<std::uint32_t>(table_definition_page));
    write_u32(last_leaf, 12, 7);
    write_u16(last_leaf, 24, 0);
    last_leaf[26] = 0;
    last_leaf[27] |= 1u << 6;
    const std::array<std::uint8_t, 6> last_leaf_entry{
        0x20, 0x01, 0x00, 0x00, 0x04, 0x02,
    };
    std::copy(last_leaf_entry.begin(), last_leaf_entry.end(),
              last_leaf.begin() + static_cast<std::ptrdiff_t>(index_entry_data_offset));
    std::copy(last_leaf.begin(), last_leaf.end(),
              file_bytes.begin() + static_cast<std::ptrdiff_t>(8 * page_size));

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(file_bytes.data()),
                 static_cast<std::streamsize>(file_bytes.size()));
    check(static_cast<bool>(output), "failed to write the index fixture");
    return path;
}

std::filesystem::path make_canonical_row_error_fixture() {
    const auto path = make_database_fixture();
    std::vector<std::uint8_t> file_bytes(page_size * 8, 0);
    {
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char*>(file_bytes.data()),
                   static_cast<std::streamsize>(page_size * 6));
        check(input.gcount() == static_cast<std::streamsize>(page_size * 6),
              "failed to read the base canonical fixture");
    }

    const std::size_t user_definition = page_size;
    const std::uint32_t definition_size = 63 + 25 + 2 + 2 + 10;
    write_u32(file_bytes, user_definition + 8, definition_size);
    write_u32(file_bytes, user_definition + 16, 1);
    file_bytes[user_definition + 40] = 0x4e;
    write_u16(file_bytes, user_definition + 41, 1);
    write_u16(file_bytes, user_definition + 43, 0);
    write_u16(file_bytes, user_definition + 45, 1);
    write_u24(file_bytes, user_definition + 56, 6);
    file_bytes[user_definition + 63] = 0x04;
    write_u16(file_bytes, user_definition + 68, 0);
    write_u16(file_bytes, user_definition + 70, 0);
    write_u16(file_bytes, user_definition + 72, 1);
    file_bytes[user_definition + 78] = 0x01;
    write_u16(file_bytes, user_definition + 84, 0xffff);
    write_u16(file_bytes, user_definition + 86, 4);
    write_u16(file_bytes, user_definition + 88, 10);
    const auto encoded_name = utf16le(u"Value");
    std::copy(encoded_name.begin(), encoded_name.end(),
              file_bytes.begin() + static_cast<std::ptrdiff_t>(user_definition + 90));

    write_data_page(file_bytes, 6, {{0, 0, 0, 0, 0, 0x80}}, {0});
    write_data_page(file_bytes, 7, {{1, 0, 42, 0, 0, 0, 1}}, {0});
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(file_bytes.data()),
                     static_cast<std::streamsize>(file_bytes.size()));
        check(static_cast<bool>(output), "failed to write the canonical error fixture");
    }
    return path;
}

void test_text_codec() {
    const std::vector<std::uint8_t> mixed{0xff, 0xfe, 'A', 0, 0x2d, 0x4e, 0, 'B'};
    check(rea_jackcess::detail::decode_jet4_text(mixed) == "A中B", "compressed Jet4 text was not decoded");
    check(rea_jackcess::detail::decode_utf16le(utf16le(u"用户表")) == "用户表",
          "UTF-16LE text was not decoded");
}

void test_jet4_value_decoding() {
    const auto path = make_database_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto decode = [&](std::uint8_t type,
                                std::vector<std::uint8_t> bytes,
                                std::uint8_t scale = 0) {
            rea_jackcess::detail::EncodedTableRow row{
                4,
                0,
                {rea_jackcess::detail::EncodedColumnValue{false, std::move(bytes)}},
            };
            rea_jackcess::detail::ColumnDefinition column{
                "value", type, 0, 0, 0, 18, scale, 0, 0, 0, 0,
            };
            return rea_jackcess::detail::decode_jet4_value(file, "Values", row, column);
        };

        const auto byte_value = decode(0x02, {0xff});
        check(std::get<std::int8_t>(byte_value) == -1,
              "BYTE did not preserve Jackcess signed-byte read behavior");

        const auto money_value = decode(0x05, {0xc7, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
        check(std::get<rea_jackcess::detail::DecimalValue>(money_value).text == "-1.2345",
              "MONEY value was not decoded as an exact scaled decimal");

        const auto numeric_value = decode(0x10,
                                          {0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0xd2, 0x04, 0, 0},
                                          2);
        check(std::get<rea_jackcess::detail::DecimalValue>(numeric_value).text == "12.34",
              "NUMERIC value was not decoded using its scale");

        const auto date_value = decode(0x08, {0, 0, 0, 0, 0, 0, 0, 0});
        check(std::get<rea_jackcess::detail::DateTimeValue>(date_value).iso8601 == "1899-12-30T00:00:00",
              "SHORT_DATE_TIME zero serial was not decoded from the Access epoch");

        const auto out_of_range_date = decode(0x08, {0, 0, 0, 0, 0, 0, 0xf0, 0xbf});
        check(std::get<rea_jackcess::detail::DateOutOfRangeValue>(out_of_range_date).serial == -1.0,
              "negative SHORT_DATE_TIME serial was not preserved as out of range");

        std::vector<std::uint8_t> extended_date;
        extended_date.insert(extended_date.end(), 19, '0');
        extended_date.push_back(':');
        extended_date.insert(extended_date.end(), 12, '0');
        extended_date.insert(extended_date.end(), 7, '0');
        extended_date.push_back(':');
        extended_date.push_back('7');
        extended_date.push_back(0);
        const auto extended_date_value = decode(0x14, std::move(extended_date));
        check(std::get<rea_jackcess::detail::DateTimeValue>(extended_date_value).iso8601 ==
                  "0001-01-01T00:00:00",
              "EXT_DATE_TIME was not decoded from the year-one epoch");

        std::vector<std::uint8_t> inline_memo(16, 0);
        inline_memo[0] = 4;
        inline_memo[3] = 0x80;
        inline_memo[12] = 0xff;
        inline_memo[13] = 0xfe;
        inline_memo[14] = 'A';
        inline_memo[15] = 0;
        const auto memo_value = decode(0x0c, std::move(inline_memo));
        check(std::get<rea_jackcess::detail::MemoValue>(memo_value).text == "A" &&
                  std::get<rea_jackcess::detail::MemoValue>(memo_value).raw_bytes ==
                      std::vector<std::uint8_t>({0xff, 0xfe, 'A', 0}),
              "inline MEMO long value was not decoded through the text codec");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_sha256_and_jet4_code_page() {
    const std::vector<std::uint8_t> abc{'a', 'b', 'c'};
    check(rea_jackcess::detail::sha256_hex(abc) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 did not match the standard abc vector");

    std::vector<std::uint8_t> root_page(page_size, 0);
    constexpr std::uint16_t code_page = 1252;
    root_page[60] = static_cast<std::uint8_t>(code_page) ^ 0x7b;
    root_page[61] = static_cast<std::uint8_t>(code_page >> 8) ^ 0x42;
    check(rea_jackcess::detail::decode_jet4_code_page(root_page) == code_page,
          "Jet4 code page was not decoded from its obfuscated root header");
}

void test_usage_map_and_system_catalog() {
    const auto path = make_database_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto definition = rea_jackcess::detail::read_table_definition(file, 2);
        check(definition.owned_pages.page_number == 3 && definition.owned_pages.row_number == 0,
              "table definition owned-page map reference was not decoded");
        const auto pages = rea_jackcess::detail::read_usage_map_pages(file, definition.owned_pages);
        check(pages == std::vector<std::uint64_t>({4, 5}), "inline Usage Map returned the wrong data pages");

        rea_jackcess::detail::TableRowReader moved_from_reader(file, "MSysObjects", definition);
        auto row_reader = std::move(moved_from_reader);
        check(!moved_from_reader.next(), "moved-from table row reader was not safe to query");
        check(row_reader.next().has_value(), "moved table row reader lost its first row");

        const auto objects = rea_jackcess::detail::read_system_catalog_objects(file, definition);
        check(objects.size() == 2, "system catalog row count is incorrect");
        check(objects[0].id == 1 && objects[0].parent_id == 0 && objects[0].name == "用户表" &&
                  objects[0].type == 1 && objects[0].flags == 0,
              "overflow system catalog row was not decoded");
        check(objects[1].id == 2 && objects[1].parent_id == 0 && objects[1].name == "A中B" &&
                  objects[1].type == 1 && objects[1].flags == 2,
              "compressed-text system catalog row was not decoded");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_table_schema_catalog() {
    const auto path = make_database_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
        check(catalog.errors.empty(), "valid local table schemas produced catalog errors");
        check(catalog.tables.size() == 2, "table schema catalog did not discover both local tables");
        check(catalog.tables[0].name == "用户表" && catalog.tables[0].is_user_table &&
                  catalog.tables[0].object_type == "TABLE" && catalog.tables[0].definition.columns.empty(),
              "user table definition was not classified and loaded");
        check(catalog.tables[1].name == "A中B" && !catalog.tables[1].is_user_table &&
                  catalog.tables[1].object_type == "SYSTEM_TABLE" &&
                  catalog.tables[1].definition.columns.size() == 5 &&
                  catalog.tables[1].definition.columns[2].name == "Name",
              "system table schema was not classified and loaded");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_column_property_map_nullability() {
    const auto path = make_database_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        std::vector<std::uint8_t> property_bytes{'M', 'R', '2', 0};

        const auto names_block = property_bytes.size();
        append_u32(property_bytes, 0);
        append_u16(property_bytes, 0x80);
        const auto required_name = utf16le(u"Required");
        append_u16(property_bytes, static_cast<std::uint16_t>(required_name.size()));
        property_bytes.insert(property_bytes.end(), required_name.begin(), required_name.end());
        write_u32(property_bytes, names_block, static_cast<std::uint32_t>(property_bytes.size() - names_block));

        const auto values_block = property_bytes.size();
        append_u32(property_bytes, 0);
        append_u16(property_bytes, 0x01);
        const auto column_map = property_bytes.size();
        append_u32(property_bytes, 0);
        const auto column_name = utf16le(u"FieldA");
        append_u16(property_bytes, static_cast<std::uint16_t>(column_name.size()));
        property_bytes.insert(property_bytes.end(), column_name.begin(), column_name.end());
        write_u32(property_bytes, column_map, static_cast<std::uint32_t>(property_bytes.size() - column_map));
        append_u16(property_bytes, 9);
        property_bytes.push_back(0);
        property_bytes.push_back(0x01);
        append_u16(property_bytes, 0);
        append_u16(property_bytes, 1);
        property_bytes.push_back(1);
        write_u32(property_bytes, values_block, static_cast<std::uint32_t>(property_bytes.size() - values_block));

        const auto nullable = rea_jackcess::detail::read_column_nullability(
            file, "TestTable", 7, 3, {"FieldA", "FieldB"}, property_bytes);
        check(nullable == std::vector<bool>({false, true}),
              "Required column property was not converted into nullability");

        try {
            rea_jackcess::detail::read_column_nullability(
                file, "TestTable", 7, 3, {"FieldA"}, {'X', 'X', 'X', 0});
            throw std::runtime_error("unrecognized property map header was accepted");
        } catch (const rea_jackcess::DatabaseError& error) {
            check(error.context().table_name == "TestTable" && error.context().row_number == 3 &&
                      error.context().column_name == "LvProp",
                  "property map error omitted system catalog context");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_text_property_reads_only_the_default_map() {
    const auto path = make_database_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        std::vector<std::uint8_t> property_bytes{'M', 'R', '2', 0};

        const std::size_t names_block = property_bytes.size();
        append_u32(property_bytes, 0);
        append_u16(property_bytes, 0x80);
        const auto property_name = utf16le(u"AccessVersion");
        append_u16(property_bytes, static_cast<std::uint16_t>(property_name.size()));
        property_bytes.insert(property_bytes.end(), property_name.begin(), property_name.end());
        write_u32(property_bytes,
                  names_block,
                  static_cast<std::uint32_t>(property_bytes.size() - names_block));

        append_text_property_map(property_bytes, 0, u"NamedMap", "08.50");
        append_text_property_map(property_bytes, 2, u"", "08.50");
        const auto missing_default = rea_jackcess::detail::read_text_property(
            file, "MSysDb", 7, 3, property_bytes, "AccessVersion");
        check(!missing_default,
              "AccessVersion from a named or non-default property map was accepted");

        append_text_property_map(property_bytes, 0, u"", "09.50");
        const auto default_value = rea_jackcess::detail::read_text_property(
            file, "MSysDb", 7, 3, property_bytes, "AccessVersion");
        check(default_value == std::optional<std::string>("09.50"),
              "AccessVersion was not read from the unnamed type-zero property map");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_table_context_is_preserved_for_usage_map_errors() {
    const auto path = make_database_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        auto definition = rea_jackcess::detail::read_table_definition(file, 2);
        definition.owned_pages.page_number = file.page_count();
        try {
            rea_jackcess::detail::read_table_rows(file, "NamedTable", definition);
            throw std::runtime_error("invalid Usage Map page was accepted");
        } catch (const rea_jackcess::DatabaseError& error) {
            check(error.context().table_name == "NamedTable",
                  "Usage Map error omitted the table name");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_public_database_table_and_row_cursor() {
    const auto path = make_database_fixture();
    try {
        auto database = rea_jackcess::Database::open(path);
        check(database.format().version == rea_jackcess::EngineVersion::jet4,
              "public database API returned the wrong engine version");
        check(database.diagnostics().empty(), "valid database emitted schema diagnostics");
        const auto tables = database.tables();
        check(tables.size() == 2 && tables[1].name() == "A中B" && !tables[1].is_user_table(),
              "public database API returned the wrong table list");
        auto empty_rows = tables[0].rows();
        check(!empty_rows.next(), "empty table cursor returned a row");
        check(tables[1].columns().size() == 5 &&
                  tables[1].columns()[0].type == rea_jackcess::DataType::int32 &&
                  tables[1].columns()[2].name == "Name",
              "public table API returned the wrong schema");
        check(tables[1].indexes().size() == 1 && tables[1].indexes()[0].name == "Id" &&
                  tables[1].indexes()[0].unique && tables[1].indexes()[0].primary_key &&
                  tables[1].indexes()[0].columns.size() == 1 &&
                  tables[1].indexes()[0].columns[0].name == "Id" &&
                  tables[1].indexes()[0].columns[0].ascending,
              "public table API returned the wrong logical index definition");

        auto rows = tables[1].rows();
        const auto first = rows.next();
        check(first && first->size() == 5 && std::get<std::int32_t>(first->at(0)) == 1 &&
                  std::get<std::string>(first->at(2)) == "用户表",
              "public row cursor did not decode the first row");
        check(first->id() == rea_jackcess::RowId{4, 0},
              "sequential row cursor returned the wrong physical row identifier");
        const auto second = rows.next();
        check(second && std::get<std::int32_t>(second->at(0)) == 2 &&
                  std::get<std::string>(second->at(2)) == "A中B",
              "public row cursor did not decode the second row");
        check(!rows.next(), "public row cursor returned more rows than the table contains");

        auto moved_from_cursor = tables[1].rows();
        auto moved_cursor = std::move(moved_from_cursor);
        check(!moved_from_cursor.next(), "moved-from row cursor did not remain safe to query");
        check(moved_cursor.next().has_value(), "moved row cursor lost its first row");

        auto moved_from_table = tables[0];
        auto moved_table = std::move(moved_from_table);
        bool table_move_was_reported = false;
        try {
            moved_from_table.name();
        } catch (const std::logic_error&) {
            table_move_was_reported = true;
        }
        check(table_move_was_reported && moved_table.name() == "用户表",
              "moved-from table was unsafe or moved table lost its state");

        auto moved_from_database = rea_jackcess::Database::open(path);
        auto moved_database = std::move(moved_from_database);
        bool database_move_was_reported = false;
        try {
            moved_from_database.tables();
        } catch (const std::logic_error&) {
            database_move_was_reported = true;
        }
        check(database_move_was_reported && moved_database.tables().size() == 2,
              "moved-from database was unsafe or moved database lost its state");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_public_index_row_cursor() {
    const auto path = make_indexed_database_fixture();
    try {
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        auto rows = tables[1].rows_by_index("Id");
        std::vector<std::int32_t> ids;
        std::vector<rea_jackcess::RowId> row_ids;
        while (auto row = rows.next()) {
            ids.push_back(std::get<std::int32_t>(row->at(0)));
            row_ids.push_back(row->id());
        }
        check(ids == std::vector<std::int32_t>({1, 2, 3}),
              "index row cursor did not follow the B-tree leaf order");
        check(row_ids == std::vector<rea_jackcess::RowId>({{4, 0}, {4, 1}, {4, 2}}),
              "index row cursor returned incorrect physical row identifiers");
        check(row_ids[0] < row_ids[1] && !(row_ids[1] < row_ids[0]),
              "row identifiers did not compare by page and row number");

        bool missing_index_was_reported = false;
        try {
            tables[1].rows_by_index("Missing");
        } catch (const std::invalid_argument&) {
            missing_index_was_reported = true;
        }
        check(missing_index_was_reported, "unknown index name was accepted");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_canonical_row_error_includes_row_number() {
    const auto path = make_canonical_row_error_fixture();
    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        std::ostringstream output;
        rea_jackcess::detail::write_canonical_json(output, file, path);
        check(output.str().find("\"stage\":\"row-data\",\"table\":\"用户表\",\"column\":\"Value\"") !=
                  std::string::npos &&
                  output.str().find("\"row\":0") != std::string::npos,
              "canonical row-data error omitted its row number");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_error_contains_row_and_column_context() {
    const auto path = make_database_fixture();
    {
        std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
        constexpr std::streamoff first_column_header =
            static_cast<std::streamoff>(2 * page_size + 63 + 12 + 21);
        output.seekp(first_column_header, std::ios::beg);
        const std::array<std::uint8_t, 2> invalid_offset{0xff, 0xff};
        output.write(reinterpret_cast<const char*>(invalid_offset.data()),
                     static_cast<std::streamsize>(invalid_offset.size()));
        check(static_cast<bool>(output), "failed to corrupt the fixed column offset");
    }

    try {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto definition = rea_jackcess::detail::read_table_definition(file, 2);
        rea_jackcess::detail::read_system_catalog_objects(file, definition);
        throw std::runtime_error("invalid fixed column offset was accepted");
    } catch (const rea_jackcess::DatabaseError& error) {
        check(error.code() == rea_jackcess::DatabaseErrorCode::corrupted_file,
              "invalid fixed column offset returned the wrong error code");
        check(error.page_number() == 4, "corrupt row error did not preserve its page number");
        check(error.context().table_name == "MSysObjects", "corrupt row error omitted its table name");
        check(error.context().row_number == 0, "corrupt row error omitted its row number");
        check(error.context().column_number == 0, "corrupt row error omitted its column number");
        check(error.context().column_name == "Id", "corrupt row error omitted its column name");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}

int main() {
    try {
        test_text_codec();
        test_jet4_value_decoding();
        test_sha256_and_jet4_code_page();
        test_usage_map_and_system_catalog();
        test_table_schema_catalog();
        test_column_property_map_nullability();
        test_text_property_reads_only_the_default_map();
        test_table_context_is_preserved_for_usage_map_errors();
        test_public_database_table_and_row_cursor();
        test_public_index_row_cursor();
        test_canonical_row_error_includes_row_number();
        test_error_contains_row_and_column_context();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

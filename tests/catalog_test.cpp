#include "catalog/system_catalog.hpp"
#include "catalog/table_definition.hpp"
#include "catalog/usage_map.hpp"
#include "page/page_file.hpp"
#include "value/text_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

std::vector<std::uint8_t> utf16le(const std::u16string& text) {
    std::vector<std::uint8_t> bytes;
    for (const char16_t character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
        bytes.push_back(static_cast<std::uint8_t>(character >> 8));
    }
    return bytes;
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
    std::size_t definition_size = 63 + columns.size() * 25 + 2;
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
    definition[55] = 0;
    write_u24(definition, 56, 3);
    definition[59] = 0;
    write_u24(definition, 60, 3);

    std::size_t names_offset = 63 + columns.size() * 25;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const std::size_t header_offset = 63 + index * 25;
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
    std::copy(definition.begin(), definition.end(), file_bytes.begin() + static_cast<std::ptrdiff_t>(2 * page_size));

    const std::vector<std::uint8_t> usage_map{0x00, 0, 0, 0, 0, 0x30};
    write_data_page(file_bytes, 3, {usage_map}, {0});
    const std::vector<std::uint8_t> overflow_pointer{0, 5, 0, 0};
    const auto second_row = make_catalog_row(2, 0, 1, 0, {0xff, 0xfe, 'A', 0, 0x2d, 0x4e, 0, 'B'});
    write_data_page(file_bytes, 4, {overflow_pointer, second_row}, {0x4000, 0});
    const auto overflow_row = make_catalog_row(1, 0, 1, 0x80000000u, utf16le(u"用户表"));
    write_data_page(file_bytes, 5, {overflow_row}, {0x8000});

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file_bytes.data()), static_cast<std::streamsize>(file_bytes.size()));
    check(static_cast<bool>(output), "failed to write the catalog fixture");
    return path;
}

void test_text_codec() {
    const std::vector<std::uint8_t> mixed{0xff, 0xfe, 'A', 0, 0x2d, 0x4e, 0, 'B'};
    check(rea_jackcess::detail::decode_jet4_text(mixed) == "A中B", "compressed Jet4 text was not decoded");
    check(rea_jackcess::detail::decode_utf16le(utf16le(u"用户表")) == "用户表",
          "UTF-16LE text was not decoded");
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

        const auto objects = rea_jackcess::detail::read_system_catalog_objects(file, definition);
        check(objects.size() == 2, "system catalog row count is incorrect");
        check(objects[0].id == 1 && objects[0].parent_id == 0 && objects[0].name == "用户表" &&
                  objects[0].type == 1 && objects[0].flags == -2147483647 - 1,
              "overflow system catalog row was not decoded");
        check(objects[1].id == 2 && objects[1].parent_id == 0 && objects[1].name == "A中B" &&
                  objects[1].type == 1 && objects[1].flags == 0,
              "compressed-text system catalog row was not decoded");
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
            static_cast<std::streamoff>(2 * page_size + 63 + 21);
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
        test_usage_map_and_system_catalog();
        test_error_contains_row_and_column_context();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

#include "rea_jackcess/database_file.hpp"
#include "catalog/table_definition.hpp"
#include "page/page_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::uint64_t next_id = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-test-" + std::to_string(++next_id));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

void write_database(const std::filesystem::path& path,
                    std::uint8_t version,
                    std::uint32_t page_size,
                    const std::string& engine_name = "Standard Jet DB",
                    std::uint32_t page_count = 2) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(page_size) * page_count, 0x5a);
    for (std::size_t index = 0; index < engine_name.size(); ++index) {
        bytes[4 + index] = static_cast<std::uint8_t>(engine_name[index]);
    }
    bytes[20] = version;

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "failed to write a database fixture");
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

std::u16string column_name(std::uint16_t index) {
    if (index == 0) {
        return u"Id";
    }
    if (index == 1) {
        return u"名称";
    }

    std::u16string name = u"Column";
    for (const char digit : std::to_string(index)) {
        name.push_back(static_cast<char16_t>(digit));
    }
    return name;
}

void write_table_definition(const std::filesystem::path& path,
                            std::uint32_t next_page,
                            std::uint32_t definition_size = 1050,
                            std::uint32_t page_count = 3,
                            std::uint16_t column_count = 17) {
    write_database(path, 1, 4096, "Standard Jet DB", page_count);
    std::vector<std::uint8_t> definition(definition_size, 0);
    definition[0] = 0x02;
    write_u32(definition, 4, next_page);
    write_u32(definition, 8, definition_size);
    write_u32(definition, 16, 51);
    definition[40] = 83;
    write_u16(definition, 41, column_count);
    write_u16(definition, 43, std::min<std::uint16_t>(11, column_count));
    write_u16(definition, 45, column_count);
    write_u32(definition, 47, 2);
    write_u32(definition, 51, 2);

    constexpr std::size_t column_definition_offset = 63 + 2 * 12;
    constexpr std::size_t column_definition_size = 25;
    std::size_t names_offset = column_definition_offset + column_count * column_definition_size;
    for (std::uint16_t index = 0; index < column_count; ++index) {
        const std::size_t offset = column_definition_offset + index * column_definition_size;
        definition[offset] = index == 0 ? 0x04 : 0x0c;
        write_u16(definition, offset + 5, index);
        write_u16(definition, offset + 7, index);
        write_u16(definition, offset + 9, static_cast<std::uint16_t>(index + 1));
        definition[offset + 15] = 0x01;
        definition[offset + 16] = 0x02;
        write_u16(definition, offset + 21, static_cast<std::uint16_t>(index * 4));
        write_u16(definition, offset + 23, 4);

        const std::u16string name = column_name(index);
        write_u16(definition, names_offset, static_cast<std::uint16_t>(name.size() * 2));
        names_offset += 2;
        for (const char16_t character : name) {
            write_u16(definition, names_offset, static_cast<std::uint16_t>(character));
            names_offset += 2;
        }
    }
    check(names_offset + 2 <= definition.size(), "table fixture definition is too short for its column names");
    constexpr std::size_t physical_index_definition_size = 52;
    constexpr std::size_t logical_index_definition_size = 28;
    for (std::uint32_t index = 0; index < 2; ++index) {
        const std::size_t base = names_offset + index * physical_index_definition_size;
        for (std::uint16_t column = 0; column < 10; ++column) {
            write_u16(definition, base + 4 + column * 3, 0xffff);
        }
        write_u16(definition, base + 4, static_cast<std::uint16_t>(index));
        definition[base + 6] = index == 0 ? 0x01 : 0x00;
        write_u24(definition, base + 35, 2);
        write_u32(definition, base + 38, 0);
        definition[base + 46] = index == 0 ? 0x01 : 0x00;
    }
    const std::size_t logical_index_offset = names_offset + 2 * physical_index_definition_size;
    for (std::uint32_t index = 0; index < 2; ++index) {
        const std::size_t base = logical_index_offset + index * logical_index_definition_size;
        write_u32(definition, base + 4, index);
        write_u32(definition, base + 8, index);
        write_u32(definition, base + 13, 0xffffffffu);
        write_u32(definition, base + 17, 0xffffffffu);
        definition[base + 23] = index == 0 ? 0x01 : 0x00;
    }
    std::size_t index_names_offset = logical_index_offset + 2 * logical_index_definition_size;
    const std::array<std::u16string, 2> index_names{{u"Index0", u"Index1"}};
    for (const std::u16string& name : index_names) {
        write_u16(definition, index_names_offset, static_cast<std::uint16_t>(name.size() * 2));
        index_names_offset += 2;
        for (const char16_t character : name) {
            write_u16(definition, index_names_offset, static_cast<std::uint16_t>(character));
            index_names_offset += 2;
        }
    }
    check(index_names_offset + 2 <= definition.size(),
          "table fixture definition is too short for its index metadata");

    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    const std::size_t continuation_capacity = 4096 - 8;
    const std::size_t continuation_count = definition.size() <= 4096
                                               ? 0
                                               : (definition.size() - 4096 + continuation_capacity - 1) /
                                                     continuation_capacity;
    for (std::size_t index = 0; index < continuation_count; ++index) {
        const std::size_t page_number = next_page + index;
        if (page_number >= page_count) {
            break;
        }
        std::vector<std::uint8_t> page(4096, 0);
        page[0] = 0x02;
        write_u32(page, 4, index + 1 < continuation_count ? static_cast<std::uint32_t>(page_number + 1) : 0);
        output.seekp(static_cast<std::streamoff>(page_number * 4096), std::ios::beg);
        output.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size()));
        check(static_cast<bool>(output), "failed to write table definition continuation page");
    }

    std::size_t logical_offset = 0;
    while (logical_offset < definition.size()) {
        const std::size_t page_index = logical_offset < 4096
                                           ? 0
                                           : 1 + (logical_offset - 4096) / continuation_capacity;
        const std::size_t page_offset = logical_offset < 4096
                                            ? logical_offset
                                            : 8 + (logical_offset - 4096) % continuation_capacity;
        const std::size_t page_number = page_index == 0 ? 2 : next_page + page_index - 1;
        if (page_number >= page_count) {
            break;
        }
        const std::size_t bytes_available = 4096 - page_offset;
        const std::size_t bytes_to_copy = std::min(definition.size() - logical_offset, bytes_available);
        output.seekp(static_cast<std::streamoff>(page_number * 4096 + page_offset), std::ios::beg);
        output.write(reinterpret_cast<const char*>(definition.data() + logical_offset),
                     static_cast<std::streamsize>(bytes_to_copy));
        check(static_cast<bool>(output), "failed to write table definition data");
        logical_offset += bytes_to_copy;
    }
}

void expect_error(rea_jackcess::DatabaseErrorCode code,
                  const std::function<void()>& operation,
                  const std::filesystem::path& expected_path = {},
                  std::optional<std::uint64_t> expected_page = std::nullopt) {
    try {
        operation();
    } catch (const rea_jackcess::DatabaseError& error) {
        check(error.code() == code, "database error used the wrong error code");
        if (!expected_path.empty()) {
            check(error.path() == expected_path, "database error did not identify its file");
        }
        if (expected_page) {
            check(error.page_number() == expected_page, "database error did not identify its page");
        }
        return;
    }
    throw std::runtime_error("expected database operation to fail");
}

void test_jet3_pages(const TemporaryDirectory& directory) {
    const auto path = directory.file("jet3.mdb");
    write_database(path, 0, 2048);

    auto database = rea_jackcess::DatabaseFile::open(path);
    check(database.format().version == rea_jackcess::EngineVersion::jet3, "Jet3 header was not recognized");
    check(database.format().page_size == 2048, "Jet3 page size is incorrect");
    check(database.format().read_only, "Jet3 must be identified as read-only");
    check(database.page_count() == 2, "Jet3 page count is incorrect");

    const auto page = database.read_page(1);
    check(page.size() == 2048, "Jet3 page has an incorrect length");
    check(page.front() == 0x5a && page.back() == 0x5a, "Jet3 page contents were not read correctly");
}

void test_jet4_and_ace_headers(const TemporaryDirectory& directory) {
    const std::array<std::pair<std::uint8_t, rea_jackcess::EngineVersion>, 6> versions{{
        {1, rea_jackcess::EngineVersion::jet4},
        {2, rea_jackcess::EngineVersion::ace2007},
        {3, rea_jackcess::EngineVersion::ace2010},
        {5, rea_jackcess::EngineVersion::ace2016},
        {6, rea_jackcess::EngineVersion::ace2019},
        {1, rea_jackcess::EngineVersion::msisam},
    }};

    for (std::size_t index = 0; index < versions.size(); ++index) {
        const auto [header_version, expected_version] = versions[index];
        const auto path = directory.file("format-" + std::to_string(index) + ".mdb");
        const std::string engine_name = expected_version == rea_jackcess::EngineVersion::msisam
                                            ? "MSISAM Database"
                                            : "Standard Jet DB";
        write_database(path, header_version, 4096, engine_name);

        const auto database = rea_jackcess::DatabaseFile::open(path);
        check(database.format().version == expected_version, "database engine version was not recognized");
        check(database.format().page_size == 4096, "Jet4/ACE page size is incorrect");
        check(database.page_count() == 2, "Jet4/ACE page count is incorrect");
    }
}

void test_rejects_malformed_files(const TemporaryDirectory& directory) {
    const auto short_header = directory.file("short-header.mdb");
    {
        std::ofstream output(short_header, std::ios::binary);
        const std::array<char, 8> bytes{};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    expect_error(rea_jackcess::DatabaseErrorCode::truncated_file,
                 [&] { rea_jackcess::DatabaseFile::open(short_header); });

    const auto unsupported_version = directory.file("unsupported.mdb");
    write_database(unsupported_version, 4, 4096);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_version,
                 [&] { rea_jackcess::DatabaseFile::open(unsupported_version); }, unsupported_version);

    const auto partial_page = directory.file("partial-page.mdb");
    write_database(partial_page, 1, 4096);
    {
        std::ofstream output(partial_page, std::ios::binary | std::ios::app);
        output.put('\0');
    }
    expect_error(rea_jackcess::DatabaseErrorCode::corrupted_file,
                 [&] { rea_jackcess::DatabaseFile::open(partial_page); });
}

void test_page_bounds_and_move(const TemporaryDirectory& directory) {
    const auto path = directory.file("bounds.mdb");
    write_database(path, 1, 4096);

    auto original = rea_jackcess::DatabaseFile::open(path);
    auto moved = std::move(original);
    check(moved.path() == path, "moving a database handle lost its path");
    expect_error(rea_jackcess::DatabaseErrorCode::page_out_of_range,
                 [&] { moved.read_page(2); }, path, 2);

    const auto page = moved.read_page(1);
    check(page.size() == 4096 && page[2048] == 0x5a, "Jet4 page read returned invalid data");
}

void test_table_definition_header(const TemporaryDirectory& directory) {
    const auto path = directory.file("table-definition.mdb");
    write_table_definition(path, 0);

    const auto file = rea_jackcess::detail::PageFile::open(path);
    const auto definition = rea_jackcess::detail::read_table_definition(file, 2);
    check(definition.definition_size == 1050, "table definition size was not read correctly");
    check(definition.row_count == 51, "table row count was not read correctly");
    check(definition.table_type == 83, "table type was not read correctly");
    check(definition.maximum_column_count == 17, "table maximum column count was not read correctly");
    check(definition.variable_column_count == 11, "table variable column count was not read correctly");
    check(definition.column_count == 17, "table column count was not read correctly");
    check(definition.logical_index_count == 2, "logical index count was not read correctly");
    check(definition.physical_index_count == 2, "physical index count was not read correctly");
    check(definition.continuation_page_count == 0, "unexpected table definition continuation page");
    check(definition.columns.size() == 17, "column definitions were not read correctly");
    check(definition.columns[0].name == "Id", "ASCII column name was not decoded correctly");
    check(definition.columns[1].name == "名称", "UTF-16LE column name was not decoded correctly");
    check(definition.columns[0].type == 0x04, "column type was not read correctly");
    check(definition.columns[0].column_number == 0, "column number was not read correctly");
    check(definition.columns[0].column_id == 1, "column id was not read correctly");
    check(definition.columns[0].fixed_data_offset == 0, "fixed data offset was not read correctly");
    check(definition.columns[0].length == 4, "column length was not read correctly");
}

void test_table_definition_continuations(const TemporaryDirectory& directory) {
    const auto path = directory.file("continued-table-definition.mdb");
    write_table_definition(path, 3, 5000, 4);
    const auto file = rea_jackcess::detail::PageFile::open(path);
    const auto definition = rea_jackcess::detail::read_table_definition(file, 2);
    check(definition.continuation_page_count == 1, "table definition continuation was not followed");

    const auto cycle_path = directory.file("cyclic-table-definition.mdb");
    write_table_definition(cycle_path, 2);
    const auto cycle_file = rea_jackcess::detail::PageFile::open(cycle_path);
    expect_error(rea_jackcess::DatabaseErrorCode::corrupted_file,
                 [&] { rea_jackcess::detail::read_table_definition(cycle_file, 2); },
                 cycle_path, 2);

    const auto out_of_range_path = directory.file("out-of-range-table-definition.mdb");
    write_table_definition(out_of_range_path, 3);
    const auto out_of_range_file = rea_jackcess::detail::PageFile::open(out_of_range_path);
    expect_error(rea_jackcess::DatabaseErrorCode::corrupted_file,
                 [&] { rea_jackcess::detail::read_table_definition(out_of_range_file, 2); },
                 out_of_range_path, 2);
}

void test_column_names_cross_continuation(const TemporaryDirectory& directory) {
    const auto path = directory.file("continued-column-names.mdb");
    write_table_definition(path, 3, 8000, 4, 170);

    const auto file = rea_jackcess::detail::PageFile::open(path);
    const auto definition = rea_jackcess::detail::read_table_definition(file, 2);
    check(definition.continuation_page_count == 1, "column metadata continuation was not followed");
    check(definition.columns.size() == 170, "continued column metadata was truncated");
    check(definition.columns[0].name == "Id", "first continued column name changed");
    check(definition.columns[1].name == "名称", "continued UTF-16LE name was not decoded");
    check(definition.columns.back().name == "Column169", "last continued column name changed");
}

void test_rejects_invalid_column_name(const TemporaryDirectory& directory) {
    const auto path = directory.file("invalid-column-name.mdb");
    write_table_definition(path, 0);
    {
        std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(2 * 4096 + 63 + 2 * 12 + 17 * 25, std::ios::beg);
        const std::array<std::uint8_t, 2> invalid_length{0xff, 0xff};
        output.write(reinterpret_cast<const char*>(invalid_length.data()),
                     static_cast<std::streamsize>(invalid_length.size()));
        check(static_cast<bool>(output), "failed to corrupt a column name fixture");
    }

    const auto file = rea_jackcess::detail::PageFile::open(path);
    expect_error(rea_jackcess::DatabaseErrorCode::corrupted_file,
                 [&] { rea_jackcess::detail::read_table_definition(file, 2); },
                 path, 2);
}

void test_columns_are_sorted_by_number(const TemporaryDirectory& directory) {
    const auto path = directory.file("unsorted-columns.mdb");
    write_table_definition(path, 0);
    {
        std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
        constexpr std::streamoff first_column_offset = 2 * 4096 + 63 + 2 * 12;
        constexpr std::streamoff column_header_size = 25;
        for (std::uint16_t index = 0; index < 17; ++index) {
            const std::uint16_t column_number = static_cast<std::uint16_t>(16 - index);
            const std::array<std::uint8_t, 2> encoded{
                static_cast<std::uint8_t>(column_number),
                static_cast<std::uint8_t>(column_number >> 8),
            };
            output.seekp(first_column_offset + index * column_header_size + 5, std::ios::beg);
            output.write(reinterpret_cast<const char*>(encoded.data()),
                         static_cast<std::streamsize>(encoded.size()));
        }
        check(static_cast<bool>(output), "failed to reorder column numbers in the fixture");
    }

    const auto file = rea_jackcess::detail::PageFile::open(path);
    const auto definition = rea_jackcess::detail::read_table_definition(file, 2);
    check(definition.columns.front().column_number == 0, "columns were not sorted by column number");
    check(definition.columns.front().name == "Column16", "column name detached from its column number");
    check(definition.columns.back().column_number == 16, "last column number is incorrect after sorting");
    check(definition.columns.back().name == "Id", "last column name detached from its column number");
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_jet3_pages(directory);
        test_jet4_and_ace_headers(directory);
        test_rejects_malformed_files(directory);
        test_page_bounds_and_move(directory);
        test_table_definition_header(directory);
        test_table_definition_continuations(directory);
        test_column_names_cross_continuation(directory);
        test_rejects_invalid_column_name(directory);
        test_columns_are_sorted_by_number(directory);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

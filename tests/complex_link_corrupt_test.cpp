#include "rea_jackcess/database.hpp"

#include "catalog/row_reader.hpp"
#include "catalog/table_catalog.hpp"
#include "page/data_page.hpp"
#include "page/page_file.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace detail = rea_jackcess::detail;

using rea_jackcess::Database;
using rea_jackcess::RowId;
using rea_jackcess::Table;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "could not write " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

Table find_table(const Database& database, const std::string& name) {
    for (const auto& table : database.tables()) {
        if (table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("fixture table is missing: " + name);
}

// Returns the first parent row that actually carries attachments, plus its attachment count.
std::pair<RowId, std::size_t> first_populated_row(const Table& table, const std::string& column) {
    auto cursor = table.rows();
    while (auto row = cursor.next()) {
        const auto count = table.complex_values(column, row->id()).size();
        if (count > 0) {
            return {row->id(), count};
        }
    }
    throw std::runtime_error("no fixture row carries complex values");
}

std::size_t column_index(const Table& table, const std::string& name) {
    const auto& columns = table.columns();
    const auto found = std::find_if(columns.begin(), columns.end(), [&](const auto& column) {
        return column.name == name;
    });
    check(found != columns.end(), "fixture column is missing: " + name);
    return static_cast<std::size_t>(found - columns.begin());
}

// Reads the parent row's four-byte complex-value foreign key through the catalog so the copy can
// be damaged at exactly that field.
std::vector<std::uint8_t> foreign_key_definition(const std::filesystem::path& path,
                                                 const std::string& table_name,
                                                 const std::string& column_name,
                                                 RowId row_id) {
    const auto file = detail::PageFile::open(path);
    auto catalog = detail::read_table_schema_catalog(file);
    const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(),
                                    [&](const auto& candidate) { return candidate.name == table_name; });
    check(table != catalog.tables.end(), "catalog is missing " + table_name);
    const auto column = std::find_if(table->definition.columns.begin(),
                                     table->definition.columns.end(),
                                     [&](const auto& candidate) { return candidate.name == column_name; });
    check(column != table->definition.columns.end(), "catalog is missing " + column_name);
    const auto index = static_cast<std::size_t>(column - table->definition.columns.begin());

    const auto rows = detail::read_table_rows(file, table->name, table->definition);
    for (const auto& row : rows) {
        if (row.page_number != row_id.page_number || row.row_number != row_id.row_number) {
            continue;
        }
        check(index < row.columns.size() && row.columns[index].bytes.has_value(),
              "parent row has no foreign key bytes");
        return *row.columns[index].bytes;
    }
    throw std::runtime_error("parent row is missing from the catalog scan");
}

// The row directory gives the byte range of each row. This fixture row stores the same foreign
// key value in more than one complex column, so every hit inside the row is rewritten and the
// check only requires the selected column to end up with no attachments.
std::vector<std::size_t> locate_in_row(const std::filesystem::path& path, RowId row_id,
                                       const std::vector<std::uint8_t>& pattern) {
    const auto page = detail::read_data_page(detail::PageFile::open(path), row_id.page_number);
    for (const auto& row : page.rows) {
        if (row.row_number != row_id.row_number) {
            continue;
        }
        std::vector<std::size_t> hits;
        for (std::size_t offset = 0; offset + pattern.size() <= row.bytes.size(); ++offset) {
            if (std::equal(pattern.begin(), pattern.end(),
                           row.bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
                hits.push_back(static_cast<std::size_t>(row.start_offset) + offset);
            }
        }
        check(!hits.empty(), "the foreign key does not appear inside its row");
        return hits;
    }
    throw std::runtime_error("the parent row is missing from its data page");
}

}  // namespace

// A parent row whose complex-value foreign key matches no flat-table row must yield no complex
// values, without failing the row read or touching the file.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: complex_link_corrupt <complex-data-v2007.accdb>\n";
        return 2;
    }
    const std::filesystem::path fixture(argv[1]);
    const std::string table_name = "Table1";
    const std::string column_name = "attach-data";
    try {
        const auto original = read_file(fixture);
        std::size_t expected_values = 0;
        RowId row_id{};
        {
            auto database = Database::open(fixture);
            Table table = find_table(database, table_name);
            const auto populated = first_populated_row(table, column_name);
            row_id = populated.first;
            expected_values = populated.second;
        }

        const auto definition = foreign_key_definition(fixture, table_name, column_name, row_id);
        check(definition.size() == 4, "unexpected complex foreign key width");
        const auto page_size = detail::PageFile::open(fixture).format().page_size;
        const auto page_offset = static_cast<std::size_t>(row_id.page_number * page_size);
        check(page_offset + page_size <= original.size(), "parent row page is outside the file");
        const auto hits = locate_in_row(fixture, row_id, definition);

        auto bytes = original;
        // 0x7fffffff is a valid LONG but points at no flat-table row.
        for (const auto hit : hits) {
            const auto field_offset = page_offset + hit;
            bytes[field_offset] = 0xff;
            bytes[field_offset + 1] = 0xff;
            bytes[field_offset + 2] = 0xff;
            bytes[field_offset + 3] = 0x7f;
        }

        const auto directory = std::filesystem::temp_directory_path() /
                               ("rea-jackcess-complex-link-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto path = directory / "dangling-link.accdb";
        std::filesystem::copy_file(fixture, path, std::filesystem::copy_options::overwrite_existing);
        write_file(path, bytes);

        {
            auto database = Database::open(path);
            Table table = find_table(database, table_name);
            const auto values = table.complex_values(column_name, row_id);
            check(values.empty(), "a dangling complex link returned " + std::to_string(values.size()) +
                                      " values instead of none");
            // The parent row and its ordinary columns stay readable.
            auto rows = table.rows();
            check(rows.first().has_value(), "the parent row disappeared");
        }
        check(read_file(path) == bytes, "the failed link read rewrote the copy");
        check(read_file(fixture) == original, "the source fixture changed during the checks");

        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::cout << "Complex link checks passed: " << fixture.filename().string() << " (attachments "
                  << expected_values << " -> 0)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Complex link check failed: " << error.what() << '\n';
        return 1;
    }
}

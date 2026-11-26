#include "rea_jackcess/database.hpp"

#include "catalog/row_reader.hpp"
#include "catalog/table_catalog.hpp"
#include "catalog/table_definition.hpp"
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
#include <vector>

namespace {

namespace detail = rea_jackcess::detail;

using rea_jackcess::Database;

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

// Returns the byte offset of a fixed column inside a row on its data page.
std::size_t locate_fixed_field(const detail::PageFile& file,
                               const detail::TableDefinition& definition,
                               const std::string& column_name,
                               std::uint64_t page_number,
                               std::uint16_t row_number) {
    const auto column = std::find_if(definition.columns.begin(), definition.columns.end(),
                                     [&](const auto& candidate) {
                                         return candidate.name == column_name;
                                     });
    check(column != definition.columns.end(), "column is missing: " + column_name);
    check((column->flags & 0x01) != 0, "column is not stored at a fixed offset: " + column_name);

    const auto page = detail::read_data_page(file, page_number);
    const auto row = std::find_if(page.rows.begin(), page.rows.end(), [&](const auto& candidate) {
        return candidate.row_number == row_number;
    });
    check(row != page.rows.end(), "row is missing from its data page");
    const std::uint64_t offset = page_number * file.format().page_size + row->start_offset + 2 +
                                 column->fixed_data_offset;
    check(offset + column->length <= file.page_count() * file.format().page_size,
          "column offset lies outside the file");
    return static_cast<std::size_t>(offset);
}

}  // namespace

// Access hides objects through a 0x08 flag in MSysObjects. The test flips that bit for the user
// table in a copy and requires the reader to report it without dropping the table.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: hidden_table <jet4-data-v2000.mdb>\n";
        return 2;
    }
    const std::filesystem::path fixture(argv[1]);
    if (!std::filesystem::exists(fixture)) {
        std::cout << "fixture is missing, run the generator first: " << fixture << '\n';
        return 77;
    }
    try {
        const auto original = read_file(fixture);
        std::uint64_t table_page = 0;
        std::size_t flags_offset = 0;
        {
            const auto file = detail::PageFile::open(fixture);
            auto catalog = detail::read_table_schema_catalog(file);
            const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(),
                                            [](const auto& candidate) {
                                                return candidate.name == "DataRows";
                                            });
            check(table != catalog.tables.end(), "fixture table DataRows is missing");
            table_page = table->definition_page_number;

            const auto objects = detail::read_table_definition(file, 2);
            const auto rows = detail::read_table_rows(file, "MSysObjects", objects);
            const auto id_column = std::find_if(objects.columns.begin(), objects.columns.end(),
                                               [](const auto& candidate) {
                                                   return candidate.name == "Id";
                                               });
            check(id_column != objects.columns.end(), "MSysObjects has no Id column");
            for (const auto& row : rows) {
                if (id_column->column_number >= row.columns.size() ||
                    !row.columns[id_column->column_number].bytes ||
                    row.columns[id_column->column_number].bytes->size() != 4) {
                    continue;
                }
                const auto& bytes = *row.columns[id_column->column_number].bytes;
                const std::uint32_t id = static_cast<std::uint32_t>(bytes[0]) |
                                         (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                         (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                         (static_cast<std::uint32_t>(bytes[3]) << 24);
                if (id != table_page) {
                    continue;
                }
                flags_offset = locate_fixed_field(file, objects, "Flags", row.page_number,
                                                  row.row_number);
                break;
            }
            check(flags_offset != 0, "MSysObjects row for DataRows was not found");
        }

        auto bytes = original;
        bytes[flags_offset] = static_cast<std::uint8_t>(bytes[flags_offset] | 0x08u);

        const auto directory = std::filesystem::temp_directory_path() /
                               ("rea-jackcess-hidden-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto path = directory / "hidden.mdb";
        std::filesystem::copy_file(fixture, path, std::filesystem::copy_options::overwrite_existing);
        write_file(path, bytes);

        {
            auto database = Database::open(path);
            bool found_hidden = false;
            for (const auto& table : database.tables()) {
                if (table.name() == "DataRows") {
                    found_hidden = table.is_hidden();
                    check(table.is_user_table(), "hidden table lost its user-table flag");
                }
            }
            check(found_hidden, "the hidden flag was not reported for DataRows");
        }
        check(read_file(path) == bytes, "reading the hidden table rewrote the copy");
        check(read_file(fixture) == original, "the source fixture changed during the checks");

        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::cout << "hidden table checks passed: " << fixture.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hidden table check failed: " << error.what() << '\n';
        return 1;
    }
}

#include "rea_jackcess/database.hpp"

#include "catalog/row_reader.hpp"
#include "catalog/table_catalog.hpp"
#include "page/page_file.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace detail = rea_jackcess::detail;

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;

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

std::filesystem::path temporary_copy(const std::filesystem::path& source,
                                     const std::filesystem::path& directory,
                                     const std::string& name) {
    const auto target = directory / name;
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    return target;
}

// The generated fixture stores a cross-page MEMO in row 3. Its field bytes are the external
// long-value definition: four header bytes, then the row number, then a three-byte page number.
struct MemoDefinition {
    std::uint64_t row_page = 0;
    std::uint16_t row_number = 0;
    std::vector<std::uint8_t> definition;
};

MemoDefinition find_cross_page_memo(const std::filesystem::path& path) {
    const auto file = detail::PageFile::open(path);
    auto catalog = detail::read_table_schema_catalog(file);
    const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(),
                                    [](const auto& candidate) { return candidate.name == "DataRows"; });
    check(table != catalog.tables.end(), "fixture table DataRows is missing");

    const auto memo = std::find_if(table->definition.columns.begin(),
                                   table->definition.columns.end(),
                                   [](const auto& column) { return column.name == "MemoVal"; });
    check(memo != table->definition.columns.end(), "fixture column MemoVal is missing");
    const auto memo_index =
        static_cast<std::size_t>(memo - table->definition.columns.begin());

    const auto rows = detail::read_table_rows(file, table->name, table->definition);
    for (const auto& row : rows) {
        if (row.columns.size() <= memo_index || !row.columns[0].bytes.has_value() ||
            row.columns[0].bytes->size() != 4) {
            continue;
        }
        const auto& id_bytes = *row.columns[0].bytes;
        const std::int32_t id = static_cast<std::int32_t>(static_cast<std::uint32_t>(id_bytes[0]) |
                                                         (static_cast<std::uint32_t>(id_bytes[1]) << 8) |
                                                         (static_cast<std::uint32_t>(id_bytes[2]) << 16) |
                                                         (static_cast<std::uint32_t>(id_bytes[3]) << 24));
        if (id != 3) {
            continue;
        }
        check(row.columns[memo_index].bytes.has_value(), "row 3 has no MEMO definition");
        return {row.page_number, row.row_number, *row.columns[memo_index].bytes};
    }
    throw std::runtime_error("row 3 with the cross-page MEMO is missing");
}

// The same eight bytes can only belong to one row on the page.
std::size_t locate_definition(const std::vector<std::uint8_t>& page,
                              const std::vector<std::uint8_t>& definition) {
    std::vector<std::size_t> hits;
    for (std::size_t offset = 0; offset + definition.size() <= page.size(); ++offset) {
        if (std::equal(definition.begin(), definition.end(),
                       page.begin() + static_cast<std::ptrdiff_t>(offset))) {
            hits.push_back(offset);
        }
    }
    check(hits.size() == 1, "the MEMO definition is not unique on its page");
    return hits.front();
}

template <typename Operation>
void expect_memo_error(const std::string& label, Operation operation) {
    try {
        operation();
    } catch (const DatabaseError& error) {
        check(error.code() == DatabaseErrorCode::corrupted_file,
              label + ": unexpected error code " + std::to_string(static_cast<int>(error.code())));
        check(error.context().column_name.has_value() &&
                  *error.context().column_name == "MemoVal",
              label + ": the error lost the column context");
        check(error.context().table_name.has_value() && *error.context().table_name == "DataRows",
              label + ": the error lost the table context");
        check(error.page_number().has_value(), label + ": the error lost the page context");
        std::cout << "  " << label << ": code " << static_cast<int>(error.code()) << ", page "
                  << *error.page_number() << ", column MemoVal\n";
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(label + ": wrong exception type: " + error.what());
    }
    throw std::runtime_error(label + ": the damaged long value was accepted");
}

void read_data_rows(const std::filesystem::path& path) {
    auto database = Database::open(path);
    for (const auto& table : database.tables()) {
        if (table.name() != "DataRows") {
            continue;
        }
        auto cursor = table.rows();
        while (auto row = cursor.next()) {
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: long_value_corrupt <jet4-data-v2000.mdb>\n";
        return 2;
    }
    const std::filesystem::path fixture(argv[1]);
    if (!std::filesystem::exists(fixture)) {
        std::cout << "fixture is missing, run the generator first: " << fixture << '\n';
        return 77;
    }
    try {
        const auto original = read_file(fixture);
        const auto target = find_cross_page_memo(fixture);
        check(target.definition.size() >= 8, "unexpected MEMO definition size");
        const auto page_size = detail::PageFile::open(fixture).format().page_size;
        const auto page_offset = static_cast<std::size_t>(target.row_page * page_size);
        check(page_offset + page_size <= original.size(), "row page lies outside the file");
        const std::vector<std::uint8_t> page(original.begin() + static_cast<std::ptrdiff_t>(page_offset),
                                             original.begin() +
                                                 static_cast<std::ptrdiff_t>(page_offset + page_size));
        const auto field_offset = page_offset + locate_definition(page, target.definition);

        const auto directory = std::filesystem::temp_directory_path() /
                               ("rea-jackcess-long-value-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);

        // 1. The continuation pointer names a page past the end of the file.
        {
            auto bytes = original;
            bytes[field_offset + 5] = 0xff;
            bytes[field_offset + 6] = 0xff;
            bytes[field_offset + 7] = 0x7f;
            const auto path = temporary_copy(fixture, directory, "bad-page.mdb");
            write_file(path, bytes);
            expect_memo_error("pointer past the end of the file", [&] { read_data_rows(path); });
            check(read_file(path) == bytes, "the failed read rewrote the bad-page copy");
        }

        // 2. The continuation points at the right page but a row that does not exist there.
        {
            auto bytes = original;
            bytes[field_offset + 4] = 200;
            const auto path = temporary_copy(fixture, directory, "bad-row.mdb");
            write_file(path, bytes);
            expect_memo_error("pointer to a missing row", [&] { read_data_rows(path); });
            check(read_file(path) == bytes, "the failed read rewrote the bad-row copy");
        }

        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        check(read_file(fixture) == original, "the source fixture changed during the checks");
        std::cout << "Long-value corruption checks passed: " << fixture.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Long-value corruption check failed: " << error.what() << '\n';
        return 1;
    }
}

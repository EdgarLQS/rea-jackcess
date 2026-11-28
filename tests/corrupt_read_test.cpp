#include "rea_jackcess/database.hpp"
#include "rea_jackcess/database_file.hpp"

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

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::Row;
using rea_jackcess::Table;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-corrupt-read-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

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
    check(static_cast<bool>(output), "could not write " + path.string());
}

std::filesystem::path copy_fixture(const std::filesystem::path& source,
                                   const std::filesystem::path& directory,
                                   const std::string& name) {
    const auto target = directory / name;
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    return target;
}

std::uint32_t page_size_of(const std::filesystem::path& path) {
    const auto file = rea_jackcess::DatabaseFile::open(path);
    return file.format().page_size;
}

// A damaged file must fail through DatabaseError, never through a std::bad_alloc, a hang, or a
// silently short result. The lambda is the operation under test.
template <typename Operation>
void expect_corruption_error(const std::string& label,
                             const std::filesystem::path& expected_path,
                             DatabaseErrorCode expected_code,
                             std::uint64_t expected_page,
                             std::optional<std::string> expected_table,
                             Operation operation) {
    try {
        operation();
    } catch (const DatabaseError& error) {
        check(error.code() == expected_code,
              label + ": unexpected error code " + std::to_string(static_cast<int>(error.code())) +
                  " (" + error.what() + ")");
        check(error.path() == expected_path, label + ": the error lost its file path");
        check(error.page_number() == expected_page, label + ": the error lost or changed its page context");
        if (expected_table) {
            check(error.context().table_name == expected_table,
                  label + ": the error lost its table context");
        }
        check(std::string(error.what()).size() > 0, label + ": empty error message");
        std::cout << "  " << label << ": code " << static_cast<int>(error.code());
        if (error.page_number()) {
            std::cout << ", page " << *error.page_number();
        }
        std::cout << '\n';
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(label + ": wrong exception type: " + error.what());
    }
    throw std::runtime_error(label + ": the damaged file was accepted");
}

std::vector<Row> read_all_rows(Database& database) {
    std::vector<Row> rows;
    for (const auto& table : database.tables()) {
        if (!table.is_user_table()) {
            continue;
        }
        auto cursor = table.rows();
        while (auto row = cursor.next()) {
            rows.push_back(*row);
        }
    }
    return rows;
}

// Returns a data page that currently holds at least one live user row.
struct RowPageTarget {
    std::string table_name;
    std::uint64_t page_number;
};

RowPageTarget used_row_page(const std::filesystem::path& path) {
    auto database = Database::open(path);
    for (const auto& table : database.tables()) {
        if (!table.is_user_table()) {
            continue;
        }
        auto cursor = table.rows();
        if (auto row = cursor.next()) {
            return {table.name(), row->id().page_number};
        }
    }
    throw std::runtime_error("the fixture has no user rows to locate a data page");
}

struct IndexTarget {
    std::string table_name;
    std::string index_name;
    std::uint64_t root_page;
};

// Returns an index root that already holds entries, so wiping it must break the scan.
IndexTarget used_index_root(const std::filesystem::path& path) {
    auto database = Database::open(path);
    for (const auto& table : database.tables()) {
        if (!table.is_user_table()) {
            continue;
        }
        for (const auto& index : table.indexes()) {
            if (index.unique_entry_count > 0 && index.root_page_number > 1) {
                return {table.name(), index.name, index.root_page_number};
            }
        }
    }
    throw std::runtime_error("the fixture has no populated index root");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: corrupt_read <fixture.accdb|fixture.mdb>\n";
        return 2;
    }
    const std::filesystem::path fixture(argv[1]);
    if (!std::filesystem::exists(fixture)) {
        std::cout << "fixture is missing, run the generator first: " << fixture << '\n';
        return 77;
    }
    try {
        const auto original = read_file(fixture);
        const auto page_size = page_size_of(fixture);
        const std::uint64_t page_count = original.size() / page_size;
        check(page_count > 4, "fixture is too small for the corruption checks");
        TemporaryDirectory temporary;

        // 1. A file cut inside the first data pages loses the catalog or row pages outright.
        {
            const auto path = copy_fixture(fixture, temporary.path(), "truncated.accdb");
            std::vector<std::uint8_t> bytes = original;
            bytes.resize(page_size * 2);
            write_file(path, bytes);
            expect_corruption_error("truncated file",
                                    path,
                                    DatabaseErrorCode::page_out_of_range,
                                    2,
                                    std::nullopt,
                                    [&] {
                auto database = Database::open(path);
                read_all_rows(database);
            });
            check(read_file(path) == bytes, "the failed open rewrote the truncated copy");
        }

        // 2. Wiping an index root breaks the key scan. Page 2 is deliberately not used here:
        //    several ACE fixtures keep other data there, so the test corrupts a page the catalog
        //    itself points at.
        {
            const auto target = used_index_root(fixture);
            check(target.root_page < page_count, "index root lies outside the file");
            const auto path = copy_fixture(fixture, temporary.path(), "index-root.accdb");
            std::vector<std::uint8_t> bytes = original;
            const auto begin = static_cast<std::ptrdiff_t>(target.root_page * page_size);
            std::fill(bytes.begin() + begin,
                      bytes.begin() + begin + static_cast<std::ptrdiff_t>(page_size), 0xff);
            write_file(path, bytes);
            expect_corruption_error("wiped index root",
                                    path,
                                    DatabaseErrorCode::corrupted_file,
                                    target.root_page,
                                    target.table_name,
                                    [&] {
                auto database = Database::open(path);
                for (const auto& table : database.tables()) {
                    if (table.name() != target.table_name) {
                        continue;
                    }
                    auto cursor = table.rows_by_index(target.index_name);
                    while (auto row = cursor.next()) {
                    }
                }
            });
            check(read_file(path) == bytes, "the failed index scan rewrote the copy");
        }

        // 3. A page that holds live rows is replaced with filler: the scan must stop with an error
        //    instead of returning short or wrong data.
        {
            const auto target = used_row_page(fixture);
            check(target.page_number > 1 && target.page_number < page_count,
                  "unexpected row page number");
            const auto path = copy_fixture(fixture, temporary.path(), "data-page.accdb");
            std::vector<std::uint8_t> bytes = original;
            const auto begin = static_cast<std::ptrdiff_t>(target.page_number * page_size);
            std::fill(bytes.begin() + begin, bytes.begin() + begin + static_cast<std::ptrdiff_t>(page_size),
                      0xff);
            write_file(path, bytes);
            expect_corruption_error("wiped row page",
                                    path,
                                    DatabaseErrorCode::corrupted_file,
                                    target.page_number,
                                    target.table_name,
                                    [&] {
                auto database = Database::open(path);
                read_all_rows(database);
            });
            check(read_file(path) == bytes, "the failed read rewrote the data-page copy");
        }

        // 4. The same corruption must not leak into the source fixture.
        check(read_file(fixture) == original, "the source fixture changed during the checks");
        std::cout << "Corrupt read checks passed: " << fixture.filename().string()
                  << " (pageSize " << page_size << ", pages " << page_count << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Corrupt read check failed: " << error.what() << '\n';
        return 1;
    }
}

#include "rea_jackcess/database.hpp"

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
                ("rea-jackcess-database-create-" + std::to_string(stamp));
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

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "failed to open a test database for comparison");
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

void check_empty_database(const std::filesystem::path& path,
                          rea_jackcess::DatabaseFormat database_format,
                          std::uint64_t expected_page_count) {
    {
        auto database = rea_jackcess::Database::create(path, database_format);
        check(database.format().version == rea_jackcess::EngineVersion::jet4,
              "created database was not identified as Jet4");
        check(database.path() == path, "created database did not retain the requested path");
        std::size_t user_table_count = 0;
        for (const auto& table : database.tables()) {
            user_table_count += table.is_user_table() ? 1 : 0;
        }
        check(user_table_count == 0, "new database unexpectedly contains user tables");
    }

    const auto file = rea_jackcess::DatabaseFile::open(path);
    check(file.page_count() == expected_page_count, "created database has the wrong page count");
    auto reopened = rea_jackcess::Database::open(path);
    check(reopened.format().version == rea_jackcess::EngineVersion::jet4,
          "created database did not reopen as Jet4");
}

template <typename Operation>
void expect_error(rea_jackcess::DatabaseErrorCode code, Operation operation) {
    try {
        operation();
    } catch (const rea_jackcess::DatabaseError& error) {
        check(error.code() == code, "database creation returned the wrong error code");
        return;
    }
    throw std::runtime_error("expected database creation to fail");
}

void test_create_formats_and_replace(const TemporaryDirectory& directory) {
    const auto path = directory.file("empty.mdb");
    check_empty_database(path, rea_jackcess::DatabaseFormat::v2000, 24);

    const auto original_bytes = read_bytes(path);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        auto database = rea_jackcess::Database::create(
            path, static_cast<rea_jackcess::DatabaseFormat>(0xff));
        (void)database;
    });
    check(read_bytes(path) == original_bytes,
          "unsupported creation request modified the existing destination");

    check_empty_database(path, rea_jackcess::DatabaseFormat::v2003, 50);
}

void test_missing_parent(const TemporaryDirectory& directory) {
    const auto path = directory.file("missing-parent") / "empty.mdb";
    expect_error(rea_jackcess::DatabaseErrorCode::io_error, [&] {
        auto database = rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000);
        (void)database;
    });
    check(!std::filesystem::exists(path), "creation unexpectedly made a missing parent directory");
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_create_formats_and_replace(directory);
        test_missing_parent(directory);
        std::cout << "database creation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "database creation tests failed: " << error.what() << '\n';
        return 1;
    }
}

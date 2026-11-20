#include "rea_jackcess/database.hpp"

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

void test_create_empty_tables(const TemporaryDirectory& directory) {
    const auto path = directory.file("tables.mdb");
    rea_jackcess::TableBuilder first_table("T");
    first_table.add_column("Id", rea_jackcess::DataType::int32);
    rea_jackcess::TableBuilder second_table("Table_1");
    second_table.add_column("Code", rea_jackcess::DataType::int32)
        .add_column("GroupId", rea_jackcess::DataType::int32);

    const auto database = rea_jackcess::Database::create(
        path,
        rea_jackcess::DatabaseFormat::v2000,
        std::vector<rea_jackcess::TableBuilder>{first_table, second_table});

    std::vector<rea_jackcess::Table> user_tables;
    for (const auto& table : database.tables()) {
        if (table.is_user_table()) {
            user_tables.push_back(table);
        }
    }
    check(user_tables.size() == 2, "created database did not expose both user tables");

    const auto first = std::find_if(user_tables.begin(), user_tables.end(), [](const auto& table) {
        return table.name() == "T";
    });
    check(first != user_tables.end(), "created database did not expose table T");
    check(first->columns().size() == 1 && first->columns()[0].name == "Id" &&
              first->columns()[0].type == rea_jackcess::DataType::int32,
          "table T did not preserve its LONG column definition");
    check(!first->rows().next(), "newly created table T unexpectedly contains rows");

    const auto second = std::find_if(user_tables.begin(), user_tables.end(), [](const auto& table) {
        return table.name() == "Table_1";
    });
    check(second != user_tables.end(), "created database did not expose table Table_1");
    check(second->columns().size() == 2 && second->columns()[0].name == "Code" &&
              second->columns()[1].name == "GroupId",
          "table Table_1 did not preserve its column definitions");
    check(!second->rows().next(), "newly created table Table_1 unexpectedly contains rows");

    const auto file = rea_jackcess::DatabaseFile::open(path);
    check(file.page_count() == 28, "two empty tables did not append four Jet4 pages");
}

void test_unsupported_table_schema_preserves_destination(const TemporaryDirectory& directory) {
    const auto path = directory.file("unsupported-schema.mdb");
    check_empty_database(path, rea_jackcess::DatabaseFormat::v2000, 24);
    const auto original_bytes = read_bytes(path);

    rea_jackcess::TableBuilder unsupported_table("Unsupported");
    unsupported_table.add_column("Name", rea_jackcess::DataType::text);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        auto database = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{unsupported_table});
        (void)database;
    });
    check(read_bytes(path) == original_bytes,
          "unsupported table schema modified the existing destination");

    rea_jackcess::TableBuilder supported_table("T");
    supported_table.add_column("Id", rea_jackcess::DataType::int32);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        auto database = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2003,
            std::vector<rea_jackcess::TableBuilder>{supported_table});
        (void)database;
    });
    check(read_bytes(path) == original_bytes,
          "unsupported V2003 table creation modified the existing destination");
}

rea_jackcess::TableBuilder table_with_long_column_names(std::size_t column_count) {
    rea_jackcess::TableBuilder table("Large");
    for (std::size_t index = 0; index < column_count; ++index) {
        const std::string suffix = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        std::string column_name = "C" + std::string(61, 'A') + suffix;
        table.add_column(std::move(column_name), rea_jackcess::DataType::int32);
    }
    return table;
}

void test_table_definition_page_boundary_is_supported(const TemporaryDirectory& directory) {
    const auto path = directory.file("maximum-table-definition.mdb");
    const auto table = table_with_long_column_names(26);
    const auto database = rea_jackcess::Database::create(
        path,
        rea_jackcess::DatabaseFormat::v2000,
        std::vector<rea_jackcess::TableBuilder>{table});
    const auto tables = database.tables();
    const auto created = std::find_if(tables.begin(), tables.end(), [](const auto& value) {
        return value.name() == "Large";
    });
    check(created != tables.end() && created->is_user_table(),
          "single-page table definition at the supported boundary was not created");
    check(created->columns().size() == 26,
          "single-page table definition did not preserve all columns");
    check(!created->rows().next(), "single-page table definition unexpectedly contains rows");
}

void test_oversized_table_definition_is_rejected(const TemporaryDirectory& directory) {
    const auto path = directory.file("oversized-table-definition.mdb");
    const auto table = table_with_long_column_names(27);

    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        auto database = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table});
        (void)database;
    });
    check(!std::filesystem::exists(path),
          "oversized table definition unexpectedly published a database");
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_create_formats_and_replace(directory);
        test_missing_parent(directory);
        test_create_empty_tables(directory);
        test_unsupported_table_schema_preserves_destination(directory);
        test_table_definition_page_boundary_is_supported(directory);
        test_oversized_table_definition_is_rejected(directory);
        std::cout << "database creation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "database creation tests failed: " << error.what() << '\n';
        return 1;
    }
}

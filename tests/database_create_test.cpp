#include "rea_jackcess/database.hpp"

#include "catalog/table_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
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

void mutate_table_definition_u16(const std::filesystem::path& path,
                                const std::string& table_name,
                                std::uint64_t field_offset,
                                std::uint16_t value) {
    std::uint64_t definition_page_number = 0;
    std::vector<std::uint8_t> page;
    {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
        const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [&](const auto& candidate) {
            return candidate.name == table_name;
        });
        check(table != catalog.tables.end(), "test fixture did not expose the requested table definition");
        definition_page_number = table->definition_page_number;
        page = file.read_page(definition_page_number);
    }

    check(field_offset <= page.size() && 2 <= page.size() - field_offset,
          "test fixture field offset is outside the table definition page");
    page[field_offset] = static_cast<std::uint8_t>(value & 0xff);
    page[field_offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);

    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    check(static_cast<bool>(output), "failed to open a test database for table-definition mutation");
    const auto page_offset = static_cast<std::streamoff>(definition_page_number * page.size());
    output.seekp(page_offset);
    output.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size()));
    output.flush();
    check(static_cast<bool>(output), "failed to write a mutated table definition fixture");
}

void mutate_unique_utf16le_string(const std::filesystem::path& path,
                                  const std::string& previous,
                                  const std::string& replacement) {
    check(previous.size() == replacement.size(), "test UTF-16LE strings must have equal lengths");
    auto bytes = read_bytes(path);
    std::vector<std::uint8_t> encoded_previous;
    std::vector<std::uint8_t> encoded_replacement;
    for (const char value : previous) {
        encoded_previous.push_back(static_cast<std::uint8_t>(value));
        encoded_previous.push_back(0);
    }
    for (const char value : replacement) {
        encoded_replacement.push_back(static_cast<std::uint8_t>(value));
        encoded_replacement.push_back(0);
    }

    const auto match = std::search(bytes.begin(), bytes.end(), encoded_previous.begin(), encoded_previous.end());
    check(match != bytes.end(), "test fixture did not contain the expected UTF-16LE string");
    check(std::search(std::next(match), bytes.end(), encoded_previous.begin(), encoded_previous.end()) == bytes.end(),
          "test fixture contained more than one matching UTF-16LE string");
    std::copy(encoded_replacement.begin(), encoded_replacement.end(), match);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "failed to open a test database for UTF-16LE string mutation");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    check(static_cast<bool>(output), "failed to write a mutated UTF-16LE string fixture");
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
        check(error.code() == code,
              "database creation returned error code " + std::to_string(static_cast<int>(error.code())) +
                  " instead of " + std::to_string(static_cast<int>(code)) + ": " + error.what());
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

    const auto global_usage_page = file.read_page(1);
    const std::uint16_t usage_row_start = read_u16(global_usage_page, 14) & 0x1fff;
    check(global_usage_page[usage_row_start] == 0,
          "global usage map is not using the expected inline declaration");
    const std::uint32_t first_page_number =
        static_cast<std::uint32_t>(global_usage_page[usage_row_start + 1]) |
        (static_cast<std::uint32_t>(global_usage_page[usage_row_start + 2]) << 8) |
        (static_cast<std::uint32_t>(global_usage_page[usage_row_start + 3]) << 16) |
        (static_cast<std::uint32_t>(global_usage_page[usage_row_start + 4]) << 24);
    for (std::uint32_t page_number = 24; page_number < 28; ++page_number) {
        const std::uint32_t bit_index = page_number - first_page_number;
        const std::size_t bitmap_offset = usage_row_start + 5 + bit_index / 8;
        const std::uint8_t bit_mask = static_cast<std::uint8_t>(1u << (bit_index % 8));
        check((global_usage_page[bitmap_offset] & bit_mask) == 0,
              "appended table page remains available in the global usage map");
    }
}

void test_add_rows_to_unindexed_long_table(const TemporaryDirectory& directory) {
    const auto path = directory.file("row-write.mdb");
    rea_jackcess::TableBuilder table_builder("T");
    table_builder.add_column("Id", rea_jackcess::DataType::int32);
    table_builder.add_column("GroupId", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    const auto before_read_only_write = read_bytes(path);
    {
        auto read_only = rea_jackcess::Database::open(path);
        auto tables = read_only.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& value) {
            return value.name() == "T";
        });
        check(table != tables.end(), "read-only database did not expose table T");
        check(table->add_rows({}).empty(), "empty batch insertion should be a no-op");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({rea_jackcess::Value{std::int32_t{42}}});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table->update_row({26, 0}, {rea_jackcess::Value{std::int32_t{42}}});
        });
    }
    check(read_bytes(path) == before_read_only_write,
          "read-only row insertion changed the database file");

    {
        auto first_writer = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        (void)first_writer;
        expect_error(rea_jackcess::DatabaseErrorCode::io_error, [&] {
            auto second_writer = rea_jackcess::Database::open(
                path, rea_jackcess::DatabaseOpenMode::read_write);
            (void)second_writer;
        });
        const auto before_locked_replace = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::io_error, [&] {
            auto replacement = rea_jackcess::Database::create(
                path, rea_jackcess::DatabaseFormat::v2000);
            (void)replacement;
        });
        check(read_bytes(path) == before_locked_replace,
              "database creation changed a target held by a read-write handle");
    }

    std::vector<rea_jackcess::RowId> batch_ids;
    rea_jackcess::RowId first_row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        auto table = std::find_if(tables.begin(), tables.end(), [](const auto& value) {
            return value.name() == "T";
        });
        check(table != tables.end(), "read-write database did not expose table T");

        const auto first_id = table->add_row({rea_jackcess::Value{std::int32_t{42}},
                                              rea_jackcess::Value{std::int32_t{-7}}});
        first_row_id = first_id;
        check(first_id.page_number == 26 && first_id.row_number == 0,
              "single-row insertion returned the wrong physical RowId");

        std::vector<std::vector<rea_jackcess::Value>> batch;
        batch.reserve(500);
        for (std::int32_t value = 0; value < 500; ++value) {
            batch.push_back({rea_jackcess::Value{value}, rea_jackcess::Value{value + 1000}});
        }
        batch_ids = table->add_rows(batch);
        check(batch_ids.size() == 500, "batch insertion returned the wrong RowId count");
        check(batch_ids[253].page_number == 26 && batch_ids[253].row_number == 254,
              "batch insertion did not fill the first 255-row data page");
        check(batch_ids[254].page_number == 27 && batch_ids[254].row_number == 0,
              "batch insertion did not continue on a second data page");
        check(batch_ids[451].page_number == 27 && batch_ids[451].row_number == 197,
              "batch insertion returned the wrong row location on the second data page");
        check(table->row_count_header() == 501,
              "writable Table handle did not refresh its row-count header");

        const auto before_invalid_write = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({rea_jackcess::Value{std::string("wrong type")},
                                  rea_jackcess::Value{std::int32_t{1}}});
        });
        check(read_bytes(path) == before_invalid_write,
              "invalid row value modified the database before validation failed");

        const auto before_invalid_update = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table->update_row(first_id,
                              {rea_jackcess::Value{std::string("wrong type")},
                               rea_jackcess::Value{std::int32_t{1}}});
        });
        check(read_bytes(path) == before_invalid_update,
              "invalid row update modified the database before validation failed");
        expect_error(rea_jackcess::DatabaseErrorCode::page_out_of_range, [&] {
            table->update_row({99, 0},
                              {rea_jackcess::Value{std::int32_t{1}},
                               rea_jackcess::Value{std::int32_t{2}}});
        });
        check(read_bytes(path) == before_invalid_update,
              "invalid RowId update changed the database file");

        table->update_row(first_id,
                          {rea_jackcess::Value{std::int32_t{43}}, rea_jackcess::Value{std::monostate{}}});
        check(table->row_count_header() == 501,
              "in-place row update changed the table row-count header");

        (void)table->add_row({rea_jackcess::Value{std::int32_t{501}},
                              rea_jackcess::Value{std::monostate{}}});
        check(table->row_count_header() == 502,
              "nullable LONG row did not update the row-count header");

        auto rows = table->rows();
        const auto first = rows.next();
        check(first && first->id() == first_id && std::get<std::int32_t>(first->at(0)) == 43 &&
                  std::holds_alternative<std::monostate>(first->at(1)),
              "in-place row update could not be read from the writable handle");
        for (std::int32_t expected = 0; expected < 500; ++expected) {
            const auto row = rows.next();
            check(row && std::get<std::int32_t>(row->at(0)) == expected &&
                      std::get<std::int32_t>(row->at(1)) == expected + 1000,
                  "batch insertion changed row values or order");
        }
        const auto null_row = rows.next();
        check(null_row && std::get<std::int32_t>(null_row->at(0)) == 501 &&
                  std::holds_alternative<std::monostate>(null_row->at(1)),
              "batch insertion did not preserve a nullable LONG value");
        check(!rows.next(), "row cursor returned extra rows after a 500-row batch");
    }

    const auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& value) {
        return value.name() == "T";
    });
    check(table != tables.end() && table->row_count_header() == 502,
          "read-only reopen did not observe the inserted rows");
    auto reopened_rows = table->rows();
    const auto reopened_first = reopened_rows.next();
    check(reopened_first && reopened_first->id().page_number == first_row_id.page_number &&
              reopened_first->id().row_number == first_row_id.row_number &&
              std::get<std::int32_t>(reopened_first->at(0)) == 43 &&
              std::holds_alternative<std::monostate>(reopened_first->at(1)),
          "read-only reopen did not preserve the updated row values and RowId");
    const auto file = rea_jackcess::DatabaseFile::open(path);
    check(file.page_count() == 28, "cross-page batch did not allocate exactly two data pages");
}

void test_add_rows_with_fixed_numeric_types(const TemporaryDirectory& directory) {
    const auto path = directory.file("fixed-numeric-rows.mdb");
    rea_jackcess::TableBuilder table_builder("T");
    table_builder.add_column("ByteValue", rea_jackcess::DataType::byte)
        .add_column("ShortValue", rea_jackcess::DataType::int16)
        .add_column("LongValue", rea_jackcess::DataType::int32)
        .add_column("SingleValue", rea_jackcess::DataType::float32)
        .add_column("DoubleValue", rea_jackcess::DataType::float64);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end() && table->columns().size() == 5,
              "new numeric table did not preserve all columns");
        check(table->columns()[0].type == rea_jackcess::DataType::byte &&
                  table->columns()[1].type == rea_jackcess::DataType::int16 &&
                  table->columns()[2].type == rea_jackcess::DataType::int32 &&
                  table->columns()[3].type == rea_jackcess::DataType::float32 &&
                  table->columns()[4].type == rea_jackcess::DataType::float64,
              "new numeric table did not preserve its fixed-width column types");
        check(!table->rows().next(), "new numeric table unexpectedly contains rows");
    }

    rea_jackcess::RowId updated_row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end(), "read-write database did not expose numeric table T");

        const auto row_ids = table->add_rows({
            {std::int8_t{-128}, std::int16_t{-32768}, std::int32_t{-2147483647 - 1}, -1.25F, 1.0 / 3.0},
            {std::int8_t{127}, std::int16_t{32767}, std::int32_t{2147483647}, 3.5F, 2.5},
            {std::int8_t{1}, std::int16_t{-10}, std::int32_t{100}, 0.25F, 42.0},
        });
        check(row_ids.size() == 3, "numeric batch insertion returned the wrong RowId count");
        updated_row_id = row_ids.back();
        check(table->row_count_header() == 3, "numeric batch insertion did not update the row count");

        auto initial_rows = table->rows();
        const auto lower = initial_rows.next();
        const auto upper = initial_rows.next();
        const auto to_update = initial_rows.next();
        check(lower && std::get<std::int8_t>(lower->at(0)) == -128 &&
                  std::get<std::int16_t>(lower->at(1)) == -32768 &&
                  std::get<std::int32_t>(lower->at(2)) == std::numeric_limits<std::int32_t>::min() &&
                  std::get<float>(lower->at(3)) == -1.25F &&
                  std::get<double>(lower->at(4)) == 1.0 / 3.0,
              "numeric row encoding did not preserve lower boundary values");
        check(upper && std::get<std::int8_t>(upper->at(0)) == 127 &&
                  std::get<std::int16_t>(upper->at(1)) == 32767 &&
                  std::get<std::int32_t>(upper->at(2)) == std::numeric_limits<std::int32_t>::max() &&
                  std::get<float>(upper->at(3)) == 3.5F &&
                  std::get<double>(upper->at(4)) == 2.5,
              "numeric row encoding did not preserve upper boundary values");
        check(to_update && std::get<std::int8_t>(to_update->at(0)) == 1 &&
                  std::get<std::int16_t>(to_update->at(1)) == -10 &&
                  std::get<std::int32_t>(to_update->at(2)) == 100 &&
                  std::get<float>(to_update->at(3)) == 0.25F &&
                  std::get<double>(to_update->at(4)) == 42.0,
              "numeric row encoding did not preserve the update fixture row");
        check(!initial_rows.next(), "numeric batch insertion returned an unexpected extra row");

        const auto before_invalid_write = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int8_t{1}, std::int32_t{2}, std::int32_t{3}, 4.0F, 5.0});
        });
        check(read_bytes(path) == before_invalid_write,
              "a mismatched numeric value changed the database before validation failed");

        table->update_row(updated_row_id,
                          {std::int8_t{-1}, std::int16_t{0}, std::int32_t{123456}, 12.5F, -0.5});
        check(table->row_count_header() == 3,
              "numeric in-place update changed the table row count");

        auto rows = table->rows();
        const auto verified_lower = rows.next();
        const auto verified_upper = rows.next();
        const auto updated = rows.next();
        check(verified_lower && verified_lower->id() == row_ids[0] &&
                  std::get<std::int8_t>(verified_lower->at(0)) == -128 &&
                  std::get<std::int16_t>(verified_lower->at(1)) == -32768 &&
                  std::get<std::int32_t>(verified_lower->at(2)) == std::numeric_limits<std::int32_t>::min() &&
                  std::get<float>(verified_lower->at(3)) == -1.25F &&
                  std::get<double>(verified_lower->at(4)) == 1.0 / 3.0,
              "numeric lower boundary row changed during another row update");
        check(verified_upper && verified_upper->id() == row_ids[1] &&
                  std::get<std::int8_t>(verified_upper->at(0)) == 127 &&
                  std::get<std::int16_t>(verified_upper->at(1)) == 32767 &&
                  std::get<std::int32_t>(verified_upper->at(2)) == std::numeric_limits<std::int32_t>::max() &&
                  std::get<float>(verified_upper->at(3)) == 3.5F &&
                  std::get<double>(verified_upper->at(4)) == 2.5,
              "numeric upper boundary row changed during another row update");
        check(updated && updated->id() == updated_row_id &&
                  std::get<std::int8_t>(updated->at(0)) == -1 &&
                  std::get<std::int16_t>(updated->at(1)) == 0 &&
                  std::get<std::int32_t>(updated->at(2)) == 123456 &&
                  std::get<float>(updated->at(3)) == 12.5F &&
                  std::get<double>(updated->at(4)) == -0.5 &&
                  !rows.next(),
              "numeric in-place update did not preserve values or RowId");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "T";
    });
    check(table != tables.end() && table->row_count_header() == 3,
          "numeric table reopen did not preserve its row count");
    auto rows = table->rows();
    const auto lower = rows.next();
    const auto upper = rows.next();
    const auto updated = rows.next();
    check(lower && lower->id().row_number == 0 &&
              std::get<std::int32_t>(lower->at(2)) == std::numeric_limits<std::int32_t>::min() &&
              upper && std::get<std::int32_t>(upper->at(2)) == std::numeric_limits<std::int32_t>::max() &&
              updated && updated->id() == updated_row_id &&
              std::get<std::int32_t>(updated->at(2)) == 123456 && !rows.next(),
          "numeric table reopen did not preserve boundary and updated rows");
}

void test_create_and_write_money_columns(const TemporaryDirectory& directory) {
    const auto path = directory.file("money-rows.mdb");
    rea_jackcess::TableBuilder table_builder("MoneyValues");
    table_builder.add_column("Amount", rea_jackcess::DataType::money);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "MoneyValues";
        });
        check(table != tables.end() && table->columns().size() == 1 &&
                  table->columns()[0].type == rea_jackcess::DataType::money &&
                  table->columns()[0].fixed && table->columns()[0].size == 8,
              "new MONEY table did not preserve the fixed-width currency schema");
        check(!table->rows().next(), "new MONEY table unexpectedly contains rows");
    }

    rea_jackcess::RowId updated_row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "MoneyValues";
        });
        check(table != tables.end(), "read-write database did not expose MONEY table MoneyValues");

        const auto row_ids = table->add_rows({
            {rea_jackcess::DecimalValue{"-2341234.03450"}},
            {rea_jackcess::DecimalValue{"37"}},
            {rea_jackcess::DecimalValue{"10000.45"}},
            {rea_jackcess::DecimalValue{"1e2"}},
            {rea_jackcess::DecimalValue{"0e1000001"}},
            {rea_jackcess::DecimalValue{"+922337203685477.5807"}},
            {rea_jackcess::DecimalValue{"-922337203685477.5808"}},
        });
        check(row_ids.size() == 7 && table->row_count_header() == 7,
              "MONEY batch insertion returned the wrong row count");
        updated_row_id = row_ids[2];

        auto initial_rows = table->rows();
        const std::vector<std::string> expected{
            "-2341234.0345", "37.0000", "10000.4500", "100.0000", "0.0000", "922337203685477.5807",
            "-922337203685477.5808"};
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto row = initial_rows.next();
            check(row && row->id() == row_ids[index] &&
                      std::get<rea_jackcess::DecimalValue>(row->at(0)).text == expected[index],
                  "MONEY encoding did not preserve exact decimal values and scale");
        }
        check(!initial_rows.next(), "MONEY batch insertion returned an unexpected extra row");

        const auto before_invalid_write = read_bytes(path);
        for (const std::string invalid : {"1.00001", "922337203685477.5808", "-922337203685477.5809", "1e",
                                          "0e-2147483648"}) {
            expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
                (void)table->add_row({rea_jackcess::DecimalValue{invalid}});
            });
            check(read_bytes(path) == before_invalid_write,
                  "invalid MONEY input changed the database before validation failed");
        }
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int64_t{42}});
        });
        check(read_bytes(path) == before_invalid_write,
              "a mismatched MONEY value type changed the database before validation failed");

        table->update_row(updated_row_id, {rea_jackcess::DecimalValue{"-0.0001"}});
        check(table->row_count_header() == 7,
              "MONEY update changed the table row count");
        auto rows = table->rows();
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto row = rows.next();
            const std::string& expected_value = index == 2 ? "-0.0001" : expected[index];
            check(row && row->id() == row_ids[index] &&
                      std::get<rea_jackcess::DecimalValue>(row->at(0)).text == expected_value,
                  "MONEY update changed an unexpected value or RowId");
        }
        check(!rows.next(), "MONEY update returned an unexpected extra row");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "MoneyValues";
    });
    check(table != tables.end() && table->row_count_header() == 7,
          "reopened MONEY table has an incorrect row count");
    auto rows = table->rows();
    for (const std::string expected : {"-2341234.0345", "37.0000", "-0.0001", "100.0000", "0.0000",
                                       "922337203685477.5807", "-922337203685477.5808"}) {
        const auto row = rows.next();
        check(row && std::get<rea_jackcess::DecimalValue>(row->at(0)).text == expected,
              "reopened MONEY table did not preserve exact decimal values");
    }
    check(!rows.next(), "reopened MONEY table returned an unexpected extra row");
}

void test_add_rows_with_text_and_binary_columns(const TemporaryDirectory& directory) {
    const auto path = directory.file("variable-rows.mdb");
    rea_jackcess::TableBuilder table_builder("Variable");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Name", rea_jackcess::DataType::text)
        .add_column("Payload", rea_jackcess::DataType::binary)
        .add_column("Note", rea_jackcess::DataType::text);

    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Variable";
        });
        check(table != tables.end() && table->columns().size() == 4,
              "new variable table did not preserve all columns");
        check(table->columns()[0].type == rea_jackcess::DataType::int32 &&
                  table->columns()[1].type == rea_jackcess::DataType::text &&
                  table->columns()[2].type == rea_jackcess::DataType::binary &&
                  table->columns()[3].type == rea_jackcess::DataType::text &&
                  table->columns()[0].fixed && !table->columns()[1].fixed &&
                  !table->columns()[2].fixed && !table->columns()[3].fixed &&
                  table->columns()[1].size == 510 && table->columns()[2].size == 255 &&
                  table->columns()[3].size == 510,
              "new variable table did not preserve fixed and variable column layouts");
        check(!table->rows().next(), "new variable table unexpectedly contains rows");
    }

    rea_jackcess::RowId updated_row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Variable";
        });
        check(table != tables.end(), "read-write database did not expose variable table");
        const auto row_ids = table->add_rows({
            {std::int32_t{1}, std::string("A"), rea_jackcess::BlobValue{{0, 0x7f, 0xff}}, std::string("🔥")},
            {std::int32_t{2}, std::string{}, rea_jackcess::BlobValue{}, std::monostate{}},
            {std::int32_t{3}, std::monostate{}, rea_jackcess::BlobValue{{0x80}}, std::string("雪")},
            {std::int32_t{4}, std::string(255, 'x'),
             rea_jackcess::BlobValue{std::vector<std::uint8_t>(255, 0xa5)}, std::string(255, 'y')},
        });
        check(row_ids.size() == 4 && table->row_count_header() == 4,
              "variable batch insertion did not preserve RowIds or row count");
        updated_row_id = row_ids[0];

        auto rows = table->rows();
        const auto first = rows.next();
        const auto second = rows.next();
        const auto third = rows.next();
        const auto boundary = rows.next();
        check(first && std::get<std::int32_t>(first->at(0)) == 1 &&
                  std::get<std::string>(first->at(1)) == "A" &&
                  std::get<rea_jackcess::BlobValue>(first->at(2)).bytes ==
                      std::vector<std::uint8_t>({0, 0x7f, 0xff}) &&
                  std::get<std::string>(first->at(3)) == "🔥",
              "variable row encoding did not preserve text, Unicode, and binary values");
        check(second && std::get<std::string>(second->at(1)).empty() &&
                  std::get<rea_jackcess::BlobValue>(second->at(2)).bytes.empty() &&
                  std::holds_alternative<std::monostate>(second->at(3)),
              "empty TEXT/BINARY values were confused with NULL");
        check(third && std::holds_alternative<std::monostate>(third->at(1)) &&
                  std::get<rea_jackcess::BlobValue>(third->at(2)).bytes ==
                      std::vector<std::uint8_t>({0x80}) &&
                  std::get<std::string>(third->at(3)) == "雪",
              "variable column offsets did not preserve NULL and following values");
        check(boundary && std::get<std::int32_t>(boundary->at(0)) == 4 &&
                  std::get<std::string>(boundary->at(1)) == std::string(255, 'x') &&
                  std::get<rea_jackcess::BlobValue>(boundary->at(2)).bytes ==
                      std::vector<std::uint8_t>(255, 0xa5) &&
                  std::get<std::string>(boundary->at(3)) == std::string(255, 'y') && !rows.next(),
              "maximum TEXT/BINARY column lengths were not preserved");

        const auto before_invalid_write = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{4}, rea_jackcess::BlobValue{},
                                  rea_jackcess::BlobValue{}, std::string("x")});
        });
        check(read_bytes(path) == before_invalid_write,
              "a mismatched variable value modified the file before validation failed");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{4}, std::string("\xc0\xaf"),
                                  rea_jackcess::BlobValue{}, std::string("x")});
        });
        check(read_bytes(path) == before_invalid_write,
              "invalid UTF-8 modified the file before validation failed");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{4}, std::string(256, 'x'),
                                  rea_jackcess::BlobValue{}, std::string("x")});
        });
        check(read_bytes(path) == before_invalid_write,
              "oversized TEXT modified the file before validation failed");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{4}, std::string("x"),
                                  rea_jackcess::BlobValue{std::vector<std::uint8_t>(256, 0)}, std::string("x")});
        });
        check(read_bytes(path) == before_invalid_write,
              "oversized BINARY modified the file before validation failed");

        const auto before_inline_shrink = read_bytes(path);
        table->update_row(row_ids[0],
                          {std::int32_t{10}, std::string{},
                           rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
        check(table->row_count_header() == 4 && read_bytes(path) != before_inline_shrink,
              "shrinking an inline row did not compact its data page");
        check(table->rows().next()->id() == row_ids[0],
              "shrinking an inline row changed the logical RowId");

        const auto before_first_growth = read_bytes(path);
        table->update_row(row_ids[0],
                          {std::int32_t{10}, std::string(127, 'z'),
                           rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
        check(table->row_count_header() == 4 && read_bytes(path) != before_first_growth,
              "growing TEXT update did not migrate the row without changing the row count");
        check(table->rows().next()->id() == row_ids[0],
              "growing TEXT update changed the logical RowId");

        const auto before_second_growth = read_bytes(path);
        table->update_row(row_ids[0],
                          {std::int32_t{10}, std::string(255, 'q'),
                           rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
        check(table->row_count_header() == 4 && read_bytes(path) != before_second_growth,
              "second growing TEXT update did not replace the overflow row");

        table->update_row(row_ids[0],
                          {std::int32_t{10}, std::string(255, 'r'),
                           rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
        check(table->row_count_header() == 4,
              "same-size overflow-row update changed the row count");
        const auto before_overflow_shrink = read_bytes(path);
        table->update_row(row_ids[0],
                          {std::int32_t{10}, std::string("B"),
                           rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
        check(table->row_count_header() == 4 && read_bytes(path) != before_overflow_shrink,
              "shrinking an overflow row did not compact its data page");

        auto updated_rows = table->rows();
        const auto updated = updated_rows.next();
        check(updated && updated->id() == row_ids[0] && std::get<std::int32_t>(updated->at(0)) == 10 &&
                  std::get<std::string>(updated->at(1)) == "B" &&
                  std::get<rea_jackcess::BlobValue>(updated->at(2)).bytes ==
                      std::vector<std::uint8_t>({1, 2, 3}) &&
                  std::get<std::string>(updated->at(3)) == "🚀",
              "shrinking an overflow row changed the wrong values or RowId");
    }

    const auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Variable";
    });
    check(table != tables.end() && table->row_count_header() == 4,
          "variable table reopen did not preserve its row count");
    auto rows = table->rows();
    const auto updated = rows.next();
    check(updated && updated->id() == updated_row_id && std::get<std::int32_t>(updated->at(0)) == 10 &&
              std::get<std::string>(updated->at(1)) == "B" &&
              std::get<rea_jackcess::BlobValue>(updated->at(2)).bytes ==
                  std::vector<std::uint8_t>({1, 2, 3}) &&
              std::get<std::string>(updated->at(3)) == "🚀",
          "read-only reopen did not preserve shrunk variable values and RowId");
    const auto empty_values = rows.next();
    const auto third = rows.next();
    check(empty_values && std::get<std::string>(empty_values->at(1)).empty() &&
              std::get<rea_jackcess::BlobValue>(empty_values->at(2)).bytes.empty() &&
              std::holds_alternative<std::monostate>(empty_values->at(3)) &&
              third && std::holds_alternative<std::monostate>(third->at(1)) &&
              std::get<rea_jackcess::BlobValue>(third->at(2)).bytes == std::vector<std::uint8_t>({0x80}) &&
              std::get<std::string>(third->at(3)) == "雪",
          "read-only reopen changed empty, NULL, or following variable values");
    const auto boundary = rows.next();
    check(boundary && std::get<std::string>(boundary->at(1)) == std::string(255, 'x') &&
              std::get<rea_jackcess::BlobValue>(boundary->at(2)).bytes ==
                  std::vector<std::uint8_t>(255, 0xa5) &&
              std::get<std::string>(boundary->at(3)) == std::string(255, 'y') && !rows.next(),
          "read-only reopen changed maximum TEXT/BINARY values");
}

void test_memo_and_ole_long_value_writes(const TemporaryDirectory& directory) {
    const auto path = directory.file("long-values.mdb");
    rea_jackcess::TableBuilder table_builder("LongValues");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Memo", rea_jackcess::DataType::memo)
        .add_column("Note", rea_jackcess::DataType::text)
        .add_column("Payload", rea_jackcess::DataType::ole);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    std::vector<std::uint8_t> initial_payload(5000, 0xa5);
    std::vector<std::uint8_t> updated_payload(8192, 0x6c);
    const std::string initial_memo(12000, 'M');
    const std::string updated_memo(20000, 'U');
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "LongValues";
        });
        check(table != tables.end(), "read-write database did not expose the Memo/OLE table");
        check(table->columns().size() == 4 &&
                  table->columns()[1].type == rea_jackcess::DataType::memo &&
                  table->columns()[3].type == rea_jackcess::DataType::ole,
              "created Memo/OLE columns did not preserve their Jet4 schema");

        const auto row_ids = table->add_rows({
            {rea_jackcess::Value{std::int32_t{1}},
             rea_jackcess::Value{rea_jackcess::MemoValue{initial_memo, {}}},
             rea_jackcess::Value{std::string("tail")},
             rea_jackcess::Value{rea_jackcess::BlobValue{initial_payload}}},
            {rea_jackcess::Value{std::int32_t{2}},
             rea_jackcess::Value{rea_jackcess::MemoValue{"🔥雪", {}}},
             rea_jackcess::Value{std::string("N")},
             rea_jackcess::Value{rea_jackcess::BlobValue{{}}}},
            {rea_jackcess::Value{std::int32_t{3}},
             rea_jackcess::Value{rea_jackcess::MemoValue{"", {}}},
             rea_jackcess::Value{std::string{}},
             rea_jackcess::Value{rea_jackcess::BlobValue{{}}}},
            {rea_jackcess::Value{std::int32_t{4}},
             rea_jackcess::Value{std::monostate{}},
             rea_jackcess::Value{std::monostate{}},
             rea_jackcess::Value{std::monostate{}}},
        });
        check(row_ids.size() == 4 && table->row_count_header() == 4,
              "Memo/OLE append did not preserve row ids and row count");

        table->update_row(row_ids.front(),
                          {rea_jackcess::Value{std::int32_t{1}},
                           rea_jackcess::Value{rea_jackcess::MemoValue{updated_memo, {}}},
                           rea_jackcess::Value{std::string("tail")},
                           rea_jackcess::Value{rea_jackcess::BlobValue{updated_payload}}});

        auto rows = table->rows();
        const auto updated = rows.next();
        check(updated && updated->id() == row_ids.front() &&
                  std::get<rea_jackcess::MemoValue>(updated->at(1)).text == updated_memo &&
                  std::get<rea_jackcess::MemoValue>(updated->at(1)).raw_bytes.size() == updated_memo.size() * 2 &&
                  std::get<std::string>(updated->at(2)) == "tail" &&
                  std::get<rea_jackcess::BlobValue>(updated->at(3)).bytes == updated_payload,
              "C++ did not read updated multi-page MEMO/OLE data and subsequent variable columns");

        const auto unicode = rows.next();
        check(unicode && std::get<rea_jackcess::MemoValue>(unicode->at(1)).text == "🔥雪" &&
                  std::get<std::string>(unicode->at(2)) == "N" &&
                  std::get<rea_jackcess::BlobValue>(unicode->at(3)).bytes.empty(),
              "C++ confused Unicode Memo, empty OLE, or a later variable-column offset");
        const auto empty = rows.next();
        check(empty && std::get<rea_jackcess::MemoValue>(empty->at(1)).text.empty() &&
                  std::get<rea_jackcess::BlobValue>(empty->at(3)).bytes.empty(),
              "C++ confused empty Memo/OLE values with NULL");
        const auto nulls = rows.next();
        check(nulls && std::holds_alternative<std::monostate>(nulls->at(1)) &&
                  std::holds_alternative<std::monostate>(nulls->at(2)) &&
                  std::holds_alternative<std::monostate>(nulls->at(3)) && !rows.next(),
              "C++ did not preserve NULL Memo/OLE values");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "LongValues";
    });
    check(table != tables.end() && table->row_count_header() == 4,
          "reopened Memo/OLE table has an incorrect row count");
    auto rows = table->rows();
    const auto first = rows.next();
    check(first && std::get<rea_jackcess::MemoValue>(first->at(1)).text == updated_memo &&
              std::get<rea_jackcess::BlobValue>(first->at(3)).bytes == updated_payload,
          "reopened database did not preserve multi-page Memo/OLE updates");
}

void test_create_and_write_guid_columns(const TemporaryDirectory& directory) {
    const auto path = directory.file("guid-rows.mdb");
    rea_jackcess::TableBuilder table_builder("Guids");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Uuid", rea_jackcess::DataType::guid);

    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Guids";
        });
        check(table != tables.end() && table->columns().size() == 2 &&
                  table->columns()[1].type == rea_jackcess::DataType::guid &&
                  table->columns()[1].fixed && table->columns()[1].size == 16,
              "new GUID table did not preserve its fixed-width column definition");
    }

    rea_jackcess::RowId first_row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Guids";
        });
        check(table != tables.end(), "read-write database did not expose GUID table Guids");

        const auto row_ids = table->add_rows({
            {std::int32_t{1}, rea_jackcess::GuidValue{"{00112233-4455-6677-8899-AABBCCDDEEFF}"}},
            {std::int32_t{2}, rea_jackcess::GuidValue{"01234567-89ab-cdef-0123-456789abcdef"}},
        });
        check(row_ids.size() == 2 && table->row_count_header() == 2,
              "GUID batch insertion returned an incorrect row count");
        first_row_id = row_ids.front();

        auto rows = table->rows();
        const auto first = rows.next();
        const auto second = rows.next();
        check(first && first->id() == row_ids[0] &&
                  std::get<rea_jackcess::GuidValue>(first->at(1)).text ==
                      "{00112233-4455-6677-8899-aabbccddeeff}" &&
                  second && second->id() == row_ids[1] &&
                  std::get<rea_jackcess::GuidValue>(second->at(1)).text ==
                      "{01234567-89ab-cdef-0123-456789abcdef}" && !rows.next(),
              "GUID encoding did not preserve canonical values and byte ordering");

        const auto before_invalid_guid = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{3}, rea_jackcess::GuidValue{"not-a-guid"}});
        });
        check(read_bytes(path) == before_invalid_guid,
              "malformed GUID changed the database before validation failed");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{3}, std::string("00112233-4455-6677-8899-aabbccddeeff")});
        });
        check(read_bytes(path) == before_invalid_guid,
              "a mismatched GUID value type changed the database before validation failed");

        table->update_row(row_ids[0],
                          {std::int32_t{1}, rea_jackcess::GuidValue{"{fedcba98-7654-3210-fedc-ba9876543210}"}});
        check(table->row_count_header() == 2,
              "GUID update changed the table row count");
        rows = table->rows();
        const auto updated = rows.next();
        check(updated && updated->id() == row_ids[0] &&
                  std::get<rea_jackcess::GuidValue>(updated->at(1)).text ==
                      "{fedcba98-7654-3210-fedc-ba9876543210}" &&
                  rows.next() && !rows.next(),
              "GUID update changed the wrong value or logical RowId");
    }

    const auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Guids";
    });
    check(table != tables.end() && table->row_count_header() == 2,
          "GUID table reopen did not preserve its row count");
    auto rows = table->rows();
    const auto updated = rows.next();
    const auto second = rows.next();
    check(updated && updated->id() == first_row_id &&
              std::get<rea_jackcess::GuidValue>(updated->at(1)).text ==
                  "{fedcba98-7654-3210-fedc-ba9876543210}" &&
              second && std::get<rea_jackcess::GuidValue>(second->at(1)).text ==
                            "{01234567-89ab-cdef-0123-456789abcdef}" && !rows.next(),
          "GUID table reopen did not preserve updated and untouched rows");
}

void test_later_oversized_batch_row_is_rejected(const TemporaryDirectory& directory) {
    const auto path = directory.file("oversized-batch-row.mdb");
    rea_jackcess::TableBuilder table_builder("LargeRows");
    table_builder.add_column("Id", rea_jackcess::DataType::int32);
    for (int index = 0; index < 8; ++index) {
        table_builder.add_column("Text" + std::to_string(index), rea_jackcess::DataType::text);
    }
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    const auto before_write = read_bytes(path);
    auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
    auto tables = writable.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "LargeRows";
    });
    check(table != tables.end(), "read-write database did not expose the oversized-row fixture");

    std::vector<rea_jackcess::Value> first_row{std::int32_t{1}};
    std::vector<rea_jackcess::Value> oversized_row{std::int32_t{2}};
    for (int index = 0; index < 8; ++index) {
        first_row.emplace_back(std::monostate{});
        oversized_row.emplace_back(std::string(255, 'x'));
    }
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)table->add_rows({first_row, oversized_row});
    });
    check(read_bytes(path) == before_write,
          "a later oversized row in a batch modified the database before rejection");
}

void test_v2003_row_writes_are_rejected(const TemporaryDirectory& directory) {
    const std::vector<std::pair<std::string, std::string>> access_versions{
        {"v2003-row-write.mdb", "09.50"},
        {"unknown-access-version-row-write.mdb", "99.99"},
        {"missing-access-version-row-write.mdb", ""},
    };
    for (const auto& [filename, access_version] : access_versions) {
        const auto path = directory.file(filename);
        rea_jackcess::TableBuilder table_builder("T");
        table_builder.add_column("Id", rea_jackcess::DataType::int32);
        {
            const auto created = rea_jackcess::Database::create(
                path,
                rea_jackcess::DatabaseFormat::v2000,
                std::vector<rea_jackcess::TableBuilder>{table_builder});
            (void)created;
        }
        // Jackcess identifies Jet4 variants through the database AccessVersion property.
        if (access_version.empty()) {
            mutate_unique_utf16le_string(path, "AccessVersion", "UnknownVersnX");
        } else {
            mutate_unique_utf16le_string(path, "08.50", access_version);
        }
        const auto before_write = read_bytes(path);

        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end(), "read-write database did not expose the format marker fixture");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row({std::int32_t{1}});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table->update_row({26, 0}, {std::int32_t{1}});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table->delete_row({26, 0});
        });
        check(read_bytes(path) == before_write,
              "non-V2000 Jet4 row operations modified the database before rejection");
    }
}

void test_row_writer_rejects_unsupported_column_layouts(const TemporaryDirectory& directory) {
    constexpr std::uint64_t variable_column_count_offset = 43;
    constexpr std::uint64_t second_column_fixed_offset_location = 63 + 25 + 21;
    const std::vector<std::pair<std::string, std::uint64_t>> unsupported_layouts{
        {"variable-columns.mdb", variable_column_count_offset},
        {"overlapping-columns.mdb", second_column_fixed_offset_location},
    };

    for (const auto& layout : unsupported_layouts) {
        const auto path = directory.file(layout.first);
        rea_jackcess::TableBuilder table_builder("T");
        table_builder.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("GroupId", rea_jackcess::DataType::int32);
        {
            const auto created = rea_jackcess::Database::create(
                path,
                rea_jackcess::DatabaseFormat::v2000,
                std::vector<rea_jackcess::TableBuilder>{table_builder});
            (void)created;
        }

        mutate_table_definition_u16(path, "T", layout.second,
                                    layout.first == "variable-columns.mdb" ? 1 : 0);
        const auto before_write = read_bytes(path);
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end(), "read-write database did not expose its malformed table fixture");
        if (layout.first == "overlapping-columns.mdb") {
            bool column_context_reported = false;
            try {
                (void)table->add_row({rea_jackcess::Value{std::int32_t{1}},
                                      rea_jackcess::Value{std::int32_t{2}}});
            } catch (const rea_jackcess::DatabaseError& error) {
                const auto& context = error.context();
                check(error.code() == rea_jackcess::DatabaseErrorCode::unsupported_operation &&
                          error.page_number().has_value() && context.table_name == "T" &&
                          context.column_number == 1 && context.column_name == "GroupId",
                      "unsupported column layout did not report its column context");
                column_context_reported = true;
            }
            check(column_context_reported, "expected unsupported fixed-offset layout to be rejected");
        } else {
            expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
                (void)table->add_row({rea_jackcess::Value{std::int32_t{1}},
                                      rea_jackcess::Value{std::int32_t{2}}});
            });
        }
        check(read_bytes(path) == before_write,
              "unsupported table column layout changed the database before rejecting a row write");
    }
}

void test_delete_rows_from_unindexed_table(const TemporaryDirectory& directory) {
    const auto path = directory.file("row-delete.mdb");
    rea_jackcess::TableBuilder table_builder("T");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("GroupId", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    const auto before_read_only_delete = read_bytes(path);
    {
        auto read_only = rea_jackcess::Database::open(path);
        auto tables = read_only.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end(), "read-only database did not expose table T");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table->delete_row({26, 0});
        });
    }
    check(read_bytes(path) == before_read_only_delete,
          "read-only row deletion changed the database file");

    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end(), "read-write database did not expose table T");
        row_ids = table->add_rows({
            {rea_jackcess::Value{std::int32_t{1}}, rea_jackcess::Value{std::int32_t{10}}},
            {rea_jackcess::Value{std::int32_t{2}}, rea_jackcess::Value{std::int32_t{20}}},
            {rea_jackcess::Value{std::int32_t{3}}, rea_jackcess::Value{std::int32_t{30}}},
        });
        check(table->row_count_header() == 3, "test fixture did not add three rows");

        const auto before_invalid_delete = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::page_out_of_range, [&] {
            table->delete_row({99, 0});
        });
        check(read_bytes(path) == before_invalid_delete,
              "invalid RowId deletion changed the database file");

        table->delete_row(row_ids[1]);
        check(table->row_count_header() == 2, "deleting a row did not decrement the table row count");
        auto rows = table->rows();
        const auto first = rows.next();
        const auto last = rows.next();
        check(first && first->id() == row_ids[0] && std::get<std::int32_t>(first->at(0)) == 1,
              "row deletion changed the first live row");
        check(last && last->id() == row_ids[2] && std::get<std::int32_t>(last->at(0)) == 3,
              "row deletion changed the last live row");
        check(!rows.next(), "row cursor returned the deleted row");

        const auto after_delete = read_bytes(path);
        table->delete_row(row_ids[1]);
        check(table->row_count_header() == 2 && read_bytes(path) == after_delete,
              "deleting the same RowId twice changed the table");
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table->update_row(row_ids[1],
                              {rea_jackcess::Value{std::int32_t{22}},
                               rea_jackcess::Value{std::int32_t{220}}});
        });
        check(read_bytes(path) == after_delete,
              "updating a deleted row changed the database file");

        (void)table->add_row({rea_jackcess::Value{std::int32_t{4}},
                              rea_jackcess::Value{std::int32_t{40}}});
        check(table->row_count_header() == 3,
              "appending after a deletion did not update the row count");
    }

    auto reopened = rea_jackcess::Database::open(path);
    auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "T";
    });
    check(table != tables.end() && table->row_count_header() == 3,
          "read-only reopen did not observe the post-delete row count");
    auto rows = table->rows();
    for (const std::int32_t expected : {1, 3, 4}) {
        const auto row = rows.next();
        check(row && std::get<std::int32_t>(row->at(0)) == expected,
              "read-only reopen returned the wrong rows after deletion and append");
    }
    check(!rows.next(), "read-only reopen returned an unexpected row after deletion");
}

void test_unsupported_table_schema_preserves_destination(const TemporaryDirectory& directory) {
    const auto path = directory.file("unsupported-schema.mdb");
    check_empty_database(path, rea_jackcess::DatabaseFormat::v2000, 24);
    const auto original_bytes = read_bytes(path);

    rea_jackcess::TableBuilder unsupported_table("Unsupported");
    unsupported_table.add_column("Amount", rea_jackcess::DataType::numeric);
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
        test_add_rows_to_unindexed_long_table(directory);
        test_add_rows_with_fixed_numeric_types(directory);
        test_create_and_write_money_columns(directory);
        test_create_and_write_guid_columns(directory);
        test_add_rows_with_text_and_binary_columns(directory);
        test_memo_and_ole_long_value_writes(directory);
        test_later_oversized_batch_row_is_rejected(directory);
        test_v2003_row_writes_are_rejected(directory);
        test_row_writer_rejects_unsupported_column_layouts(directory);
        test_delete_rows_from_unindexed_table(directory);
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

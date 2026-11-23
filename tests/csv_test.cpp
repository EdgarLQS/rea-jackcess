#include "rea_jackcess/csv.hpp"
#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::CsvOptions;
using rea_jackcess::Database;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

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
                ("rea-jackcess-csv-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path file(const std::string& name) const { return path_ / name; }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

TableBuilder types_schema() {
    return TableBuilder("Types")
        .add_column("Flag", rea_jackcess::DataType::boolean)
        .add_column("Small", rea_jackcess::DataType::byte)
        .add_column("Shorty", rea_jackcess::DataType::int16)
        .add_column("Number", rea_jackcess::DataType::int32)
        .add_column("Real", rea_jackcess::DataType::float32)
        .add_column("Double", rea_jackcess::DataType::float64)
        .add_column("Amount", rea_jackcess::DataType::money)
        .add_column("Code", rea_jackcess::DataType::guid)
        .add_column("Label", rea_jackcess::DataType::text)
        .add_column("Blob", rea_jackcess::DataType::binary)
        .add_column("Notes", rea_jackcess::DataType::memo)
        .add_column("Picture", rea_jackcess::DataType::ole);
}

Table types_table(const Database& database) {
    for (const auto& candidate : database.tables()) {
        if (candidate.is_user_table() && candidate.name() == "Types") {
            return candidate;
        }
    }
    throw std::runtime_error("the Types table is missing");
}

void write_rows(Table& table) {
    table.add_rows({
        {true, std::int8_t{7}, std::int16_t{300}, std::int32_t{70000}, 1.5f, 2.25,
         rea_jackcess::DecimalValue{"1234.5678"}, rea_jackcess::GuidValue{"00112233-4455-6677-8899-aabbccddeeff"},
         std::string("plain"), rea_jackcess::BlobValue{{1, 2, 3}}, rea_jackcess::MemoValue{"memo text", {}},
         rea_jackcess::BlobValue{{9, 8}}},
        {false, std::int8_t{-7}, std::int16_t{-300}, std::int32_t{-70000}, -1.5f, -2.25,
         rea_jackcess::DecimalValue{"-0.0001"},
         rea_jackcess::GuidValue{"ffeeddcc-bbaa-9988-7766-554433221100"},
         std::string("has,comma and \"quote\""), rea_jackcess::BlobValue{{}},
         rea_jackcess::MemoValue{"line1\nline2", {}}, Value{}},
        {Value{}, Value{}, Value{}, Value{}, Value{}, Value{}, Value{}, Value{}, Value{}, Value{},
         Value{}, Value{}},
        {true, std::int8_t{0}, std::int16_t{0}, std::int32_t{0}, 0.0f, 0.0,
         rea_jackcess::DecimalValue{"0"}, rea_jackcess::GuidValue{"00112233-4455-6677-8899-aabbccddeeff"},
         std::string("grouped"), rea_jackcess::BlobValue{{1, 2, 3, 4, 5, 6, 7, 8}},
         rea_jackcess::MemoValue{"", {}},
         rea_jackcess::BlobValue{std::vector<std::uint8_t>(20, 0xab)}},
    });
}

// The text the fixed Jackcess 5.2.0 ExportUtil writes for the rows above.
std::string baseline_csv() {
    return R"CSV(true,7,300,70000,1.5,2.25,1234.5678,{00112233-4455-6677-8899-AABBCCDDEEFF},plain,01 02 03,memo text,09 08
false,-7,-300,-70000,-1.5,-2.25,-0.0001,{FFEEDDCC-BBAA-9988-7766-554433221100},"has,comma and ""quote""",,"line1
line2",
false,,,,,,,,,,,
true,0,0,0,0.0,0.0,0.0000,{00112233-4455-6677-8899-AABBCCDDEEFF},grouped,01 02 03 04  05 06 07 08,,AB AB AB AB  AB AB AB AB  AB AB AB AB  AB AB AB AB  AB AB AB AB
)CSV";
}

void test_export_matches_baseline(const std::filesystem::path& directory) {
    const auto path = directory / "types.mdb";
    Database::create(path, DatabaseFormat::v2000, {types_schema()});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = types_table(database);
        write_rows(table);

        std::ostringstream exported;
        rea_jackcess::export_table_csv(table, exported);
        check(exported.str() == baseline_csv(),
              "exported CSV differs from the fixed Jackcess 5.2.0 output:\n" + exported.str());

        CsvOptions with_header;
        with_header.header = true;
        std::ostringstream headed;
        rea_jackcess::export_table_csv(table, headed, with_header);
        check(headed.str() ==
                  "Flag,Small,Shorty,Number,Real,Double,Amount,Code,Label,Blob,Notes,Picture\n" +
                      baseline_csv(),
              "the header record is not the column names in order");
    }
}

void expect_invalid(const std::string& label, const std::function<void()>& operation);

void test_export_columns_and_row_filter(const std::filesystem::path& directory) {
    const auto path = directory / "export-filter.mdb";
    Database::create(path, DatabaseFormat::v2000, {types_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = types_table(database);
    write_rows(table);

    CsvOptions options;
    options.header = true;
    options.columns = {"label", "NUMBER"};
    options.row_filter = [](const rea_jackcess::Row& row) {
        check(row.size() == 2, "the CSV row filter did not receive the projected columns");
        const auto* number = std::get_if<std::int32_t>(&row.at(1));
        return number != nullptr && *number > 0;
    };
    std::ostringstream exported;
    rea_jackcess::export_table_csv(table, exported, options);
    check(exported.str() == "Label,Number\nplain,70000\n",
          "CSV column selection or row filtering produced the wrong output:\n" + exported.str());

    options.columns = {"Missing"};
    std::ostringstream rejected_output;
    expect_invalid("unknown export column", [&] {
        rea_jackcess::export_table_csv(table, rejected_output, options);
    });
    check(rejected_output.str().empty(), "an invalid export column wrote partial CSV output");
}

void test_import_round_trip(const std::filesystem::path& directory) {
    const auto path = directory / "import.mdb";
    Database::create(path, DatabaseFormat::v2000, {types_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = types_table(database);

    std::istringstream input(baseline_csv());
    const auto imported = rea_jackcess::import_table_csv(table, input);
    check(imported == 4, "expected four imported rows");
    check(table.row_count_header() == 4, "the imported rows are not in the table");

    std::ostringstream again;
    rea_jackcess::export_table_csv(table, again);
    check(again.str() == baseline_csv(), "import then export did not reproduce the text:\n" + again.str());

    auto cursor = table.rows();
    const auto first = cursor.next();
    check(first.has_value(), "the first imported row is missing");
    check(std::get_if<bool>(&first->at(0)) != nullptr && *std::get_if<bool>(&first->at(0)),
          "BOOLEAN was not imported");
    check(std::get_if<std::int32_t>(&first->at(3)) != nullptr &&
              *std::get_if<std::int32_t>(&first->at(3)) == 70000,
          "LONG was not imported");
    const auto* money = std::get_if<rea_jackcess::DecimalValue>(&first->at(6));
    check(money != nullptr && money->text.find("1234.5678") != std::string::npos,
          "MONEY was not imported");
    const auto* memo = std::get_if<rea_jackcess::MemoValue>(&first->at(10));
    check(memo != nullptr && memo->text == "memo text", "MEMO was not imported");

    // The record with a quoted newline must stay one row.
    const auto second = cursor.next();
    const auto* notes = second.has_value() ? std::get_if<rea_jackcess::MemoValue>(&second->at(10)) : nullptr;
    check(notes != nullptr && notes->text == "line1\nline2",
          "a quoted newline split the record");
    check(second.has_value() && std::holds_alternative<std::monostate>(second->at(11)),
          "an empty field should import as NULL");
}

void test_import_row_filter(const std::filesystem::path& directory) {
    const auto path = directory / "import-filter.mdb";
    Database::create(path, DatabaseFormat::v2000, {types_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = types_table(database);

    CsvOptions options;
    options.header = true;
    std::size_t filtered_records = 0;
    options.import_row_filter = [&](const std::vector<std::string>& fields) {
        ++filtered_records;
        return fields.size() == 2 && fields[0] == "42";
    };
    std::istringstream input("Number,Label\n42,kept\nnot-a-number,skipped before conversion\n");
    check(rea_jackcess::import_table_csv(table, input, options) == 1,
          "the CSV import filter did not skip a source row");
    check(filtered_records == 2, "the CSV import filter did not run once per data record");
    check(table.row_count_header() == 1, "a filtered CSV row was written");
    auto cursor = table.rows();
    const auto row = cursor.next();
    check(row && std::get_if<std::int32_t>(&row->at(3)) &&
              *std::get_if<std::int32_t>(&row->at(3)) == 42 &&
              std::get_if<std::string>(&row->at(8)) &&
              *std::get_if<std::string>(&row->at(8)) == "kept",
          "the accepted filtered row was mapped incorrectly");
}

void test_header_mapping(const std::filesystem::path& directory) {
    const auto path = directory / "header.mdb";
    Database::create(path, DatabaseFormat::v2000, {types_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = types_table(database);

    CsvOptions options;
    options.header = true;
    std::istringstream input("Number,Label\n42,hello\n");
    check(rea_jackcess::import_table_csv(table, input, options) == 1, "header import missed the row");
    auto cursor = table.rows();
    const auto row = cursor.next();
    check(row.has_value(), "the header-mapped row is missing");
    check(std::get_if<std::int32_t>(&row->at(3)) != nullptr && *std::get_if<std::int32_t>(&row->at(3)) == 42,
          "the header did not map Number");
    check(std::get_if<std::string>(&row->at(8)) != nullptr &&
              *std::get_if<std::string>(&row->at(8)) == "hello",
          "the header did not map Label");
    check(std::holds_alternative<std::monostate>(row->at(1)),
          "columns missing from the header should be NULL");
}

void expect_invalid(const std::string& label, const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(label + ": the input was accepted");
}

void test_errors(const std::filesystem::path& directory) {
    const auto path = directory / "errors.mdb";
    Database::create(path, DatabaseFormat::v2000, {types_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = types_table(database);

    CsvOptions options;
    options.header = true;
    expect_invalid("unknown header column", [&] {
        std::istringstream input("Nope\n1\n");
        (void)rea_jackcess::import_table_csv(table, input, options);
    });
    expect_invalid("bad integer", [&] {
        std::istringstream input("1,2,3,not-a-number\n");
        (void)rea_jackcess::import_table_csv(table, input);
    });
    expect_invalid("field count", [&] {
        std::istringstream input("true,1\n");
        (void)rea_jackcess::import_table_csv(table, input);
    });
    expect_invalid("bad hex", [&] {
        std::istringstream input("true,1,2,3,1.0,1.0,1.0,{00112233-4455-6677-8899-AABBCCDDEEFF},x,zz,,\n");
        (void)rea_jackcess::import_table_csv(table, input);
    });
    expect_invalid("unterminated quote", [&] {
        std::istringstream input("true,1,2,3,1.0,1.0,1.0,{00112233-4455-6677-8899-AABBCCDDEEFF},\"open,,,\n");
        (void)rea_jackcess::import_table_csv(table, input);
    });
    check(table.row_count_header() == 0, "a rejected import should not add rows");
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_export_matches_baseline(directory.path());
        test_export_columns_and_row_filter(directory.path());
        test_import_round_trip(directory.path());
        test_import_row_filter(directory.path());
        test_header_mapping(directory.path());
        test_errors(directory.path());
        std::cout << "CSV tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CSV tests failed: " << error.what() << '\n';
        return 1;
    }
}

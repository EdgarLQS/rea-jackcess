#include "rea_jackcess/csv.hpp"
#include "rea_jackcess/database.hpp"

#include "page/data_page.hpp"
#include "page/page_file.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::CsvOptions;
using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::DataType;
using rea_jackcess::Row;
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

Table named_table(const Database& database, const std::string& name) {
    const auto tables = database.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    if (table == tables.end()) {
        throw std::runtime_error("the " + name + " table is missing");
    }
    return *table;
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

void test_complex_reference_export(const std::filesystem::path& fixture) {
    auto database = Database::open(fixture);
    auto table = named_table(database, "Table1");
    const auto& columns = table.columns();
    check(columns.size() == 6 && columns[1].type == DataType::complex &&
              columns[4].type == DataType::complex && columns[5].type == DataType::complex,
          "the fixed ACE sample has an unexpected complex-column schema");

    CsvOptions options;
    options.header = true;
    options.columns = {"id", columns[1].name, columns[4].name, columns[5].name};
    std::ostringstream all_rows;
    rea_jackcess::export_table_csv(table, all_rows, options);
    check(all_rows.str() ==
              "id,VersionHistory_F5F8918F-0A3F-4DA9-AE71-184EE5012880,multi-value-data,attach-data\n"
              "row1,1,1,1\nrow2,2,2,2\nrow3,3,3,3\nrow4,4,4,4\n",
          "complex CSV output did not match Jackcess foreign-key text:\n" + all_rows.str());

    options.row_filter = [](const Row& row) {
        check(row.size() == 4, "the complex CSV filter did not receive projected values");
        const auto* foreign_key = std::get_if<std::int32_t>(&row.at(1));
        return foreign_key != nullptr && *foreign_key >= 3;
    };
    std::ostringstream filtered_rows;
    rea_jackcess::export_table_csv(table, filtered_rows, options);
    check(filtered_rows.str() ==
              "id,VersionHistory_F5F8918F-0A3F-4DA9-AE71-184EE5012880,multi-value-data,attach-data\n"
              "row3,3,3,3\nrow4,4,4,4\n",
          "complex CSV filtering changed the selected rows:\n" + filtered_rows.str());

    std::istringstream import_input;
    bool complex_import_rejected = false;
    try {
        (void)rea_jackcess::import_table_csv(table, import_input);
    } catch (const DatabaseError& error) {
        complex_import_rejected = error.code() == DatabaseErrorCode::unsupported_operation &&
                                  error.context().table_name == "Table1" &&
                                  error.context().column_name == columns[1].name;
    }
    check(complex_import_rejected,
          "CSV import did not reject complex columns with table/column context");
}

void test_unsupported_complex_reference_export(const std::filesystem::path& fixture) {
    auto database = Database::open(fixture);
    const auto table = named_table(database, "Test");
    const auto unknown_complex = std::find_if(table.columns().begin(), table.columns().end(),
                                              [](const auto& column) {
        return column.name == "UnknownComplex";
    });
    check(unknown_complex != table.columns().end() &&
              unknown_complex->type == DataType::complex &&
              unknown_complex->complex_column_reference &&
              unknown_complex->complex_column_reference->type ==
                  rea_jackcess::ComplexColumnType::unsupported,
          "the fixed sample did not expose its unsupported complex field");

    CsvOptions options;
    options.header = true;
    options.columns = {"ID", "UnknownComplex"};
    std::ostringstream exported;
    rea_jackcess::export_table_csv(table, exported, options);
    check(exported.str() == "ID,UnknownComplex\n1,1\n2,2\n3,3\n",
          "an unsupported complex type did not export its raw foreign-key reference");

    options.columns = {"ID", "UnknownVar"};
    std::ostringstream rejected;
    bool unsupported_rejected = false;
    try {
        rea_jackcess::export_table_csv(table, rejected, options);
    } catch (const DatabaseError& error) {
        unsupported_rejected = error.code() == DatabaseErrorCode::unsupported_operation &&
                               error.context().table_name == "Test" &&
                               error.context().column_name == "UnknownVar";
    }
    check(unsupported_rejected && rejected.str().empty(),
          "an unsupported scalar value was not rejected before CSV output");
}

// The upstream ACE fixture stores every complex foreign key. To exercise a NULL complex cell, copy
// the fixture and clear the row NULL-mask bit for `multi-value-data` on the first row. The mask
// layout and the bit semantics (set = value present) are the same ones the row reader decodes, so
// this produces the wire form a writer would emit when the column is left unset.
void create_null_complex_fixture(const std::filesystem::path& source,
                                 const std::filesystem::path& target) {
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);

    rea_jackcess::RowId row_id{};
    std::size_t column_index = 0;
    {
        const auto database = Database::open(source);
        const auto table = named_table(database, "Table1");
        const auto& columns = table.columns();
        const auto column = std::find_if(columns.begin(), columns.end(), [](const auto& candidate) {
            return candidate.name == "multi-value-data";
        });
        check(column != columns.end() && column->type == DataType::complex,
              "the fixed ACE sample no longer exposes multi-value-data as a complex column");
        column_index = static_cast<std::size_t>(column - columns.begin());
        check(column_index == 4, "the fixed ACE sample changed its complex column order");

        auto cursor = table.rows();
        const auto row = cursor.next();
        const auto* id = row ? std::get_if<std::string>(&row->at(0)) : nullptr;
        check(row.has_value() && id != nullptr && *id == "row1",
              "the fixed ACE sample changed its first row");
        check(std::get_if<std::int32_t>(&row->at(column_index)) != nullptr,
              "the first fixed ACE row has no multi-value foreign key to clear");
        row_id = row->id();
    }

    const auto file = rea_jackcess::detail::PageFile::open(source);
    auto page = file.read_page(row_id.page_number);
    const auto parsed =
        rea_jackcess::detail::parse_data_page(file, row_id.page_number, page);
    const rea_jackcess::detail::DataPageRow* stored = nullptr;
    for (const auto& row : parsed.rows) {
        if (row.row_number == row_id.row_number) {
            stored = &row;
            break;
        }
    }
    check(stored != nullptr && !stored->overflow,
          "the fixed ACE sample stores its first row in an unexpected slot");
    const auto length = static_cast<std::size_t>(stored->end_offset - stored->start_offset);
    check(stored->bytes.size() == length,
          "the fixed ACE sample stores its first row with an overflow body");
    const auto null_byte = static_cast<std::size_t>(stored->start_offset) + length - 1;
    page[null_byte] &= static_cast<std::uint8_t>(~(1u << (column_index % 8)));

    std::fstream output(target, std::ios::in | std::ios::out | std::ios::binary);
    output.seekp(static_cast<std::streamoff>(row_id.page_number * file.format().page_size));
    output.write(reinterpret_cast<const char*>(page.data()),
                 static_cast<std::streamsize>(page.size()));
    output.flush();
    check(static_cast<bool>(output), "could not write the NULL complex fixture");
}

void test_null_complex_reference_export(const std::filesystem::path& fixture) {
    TemporaryDirectory directory;
    const auto patched = directory.file("complex-null.accdb");
    create_null_complex_fixture(fixture, patched);

    const auto database = Database::open(patched);
    const auto table = named_table(database, "Table1");

    auto cursor = table.rows();
    const auto first = cursor.next();
    check(first && std::holds_alternative<std::monostate>(first->at(4)),
          "the patched row did not decode its multi-value foreign key as NULL");
    const auto* attachment = std::get_if<std::int32_t>(&first->at(5));
    check(attachment != nullptr && *attachment == 1,
          "clearing one complex column changed the neighbouring attachment foreign key");

    std::ostringstream exported;
    rea_jackcess::export_table_csv(table, exported);
    check(exported.str() ==
              "row1,1,,,,1\n"
              "row2,2,row2-memo,row2-memo,2,2\n"
              "row3,3,row3-memo,row3-memo-again,3,3\n"
              "row4,4,row4-memo,row4-memo,4,4\n",
          "a NULL complex foreign key did not export as an empty field:\n" + exported.str());

    CsvOptions options;
    options.header = true;
    options.columns = {"id", "multi-value-data", "attach-data"};
    std::ostringstream projected;
    rea_jackcess::export_table_csv(table, projected, options);
    check(projected.str() == "id,multi-value-data,attach-data\nrow1,,1\nrow2,2,2\nrow3,3,3\nrow4,4,4\n",
          "the projected NULL complex CSV differs:\n" + projected.str());
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

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "complex-null-fixture") {
            create_null_complex_fixture(argv[2], argv[3]);
            std::cout << "wrote a NULL complex fixture to " << argv[3] << '\n';
            return 0;
        }
        if (argc == 3) {
            const std::string mode = argv[1];
            if (mode == "complex") {
                test_complex_reference_export(argv[2]);
            } else if (mode == "unsupported-complex") {
                test_unsupported_complex_reference_export(argv[2]);
            } else if (mode == "complex-null") {
                test_null_complex_reference_export(argv[2]);
            } else {
                throw std::invalid_argument("unknown CSV test mode: " + mode);
            }
            std::cout << "CSV complex export tests passed\n";
            return 0;
        }
        if (argc != 1) {
            throw std::invalid_argument(
                "usage: rea_jackcess_csv_tests [complex|unsupported-complex|complex-null fixture] "
                "| complex-null-fixture <source> <target>");
        }
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

#include "rea_jackcess/csv.hpp"
#include "rea_jackcess/database.hpp"

#include "value/jet4_value.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
using rea_jackcess::DateTimeValue;
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
                ("rea-jackcess-date-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

TableBuilder dated_schema() {
    return TableBuilder("Dated")
        .add_column("When", DataType::short_date_time)
        .add_column("Label", DataType::text);
}

Table find_table(const Database& database, const std::string& name) {
    for (const auto& candidate : database.tables()) {
        if (candidate.is_user_table() && candidate.name() == name) {
            return candidate;
        }
    }
    throw std::runtime_error("missing table " + name);
}

// A NULL date cell is reported as "NULL" so the row order can be asserted in one list.
std::string cell_text(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return "NULL";
    }
    const auto* date = std::get_if<DateTimeValue>(&value);
    if (date == nullptr) {
        throw std::runtime_error("value is not a date");
    }
    return date->iso8601;
}

void expect_rejected(const std::string& text, const std::string& message) {
    check(!rea_jackcess::detail::jet4_date_serial(text).has_value(), message);
}

void test_round_trip(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format, {dated_schema()});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Dated");
        const auto& column = table.columns().at(0);
        check(column.type == DataType::short_date_time && column.raw_type_code == 0x08,
              "the date column should be SHORT_DATE_TIME with type code 0x08");
        check(column.size == 8, "a Jet4 date column occupies eight bytes");

        const auto ids = table.add_rows({
            {DateTimeValue{"2024-03-04T05:06:07"}, std::string("exact")},
            {DateTimeValue{"1899-12-30T00:00"}, std::string("epoch")},
            {DateTimeValue{"2024-02-29T23:59:59.500"}, std::string("fraction")},
            {DateTimeValue{"1765-06-14T12:45"}, std::string("pre-epoch")},
            {DateTimeValue{"0100-06-14T12:45"}, std::string("early")},
            {Value{}, std::string("null")},
        });
        check(ids.size() == 6 && table.row_count_header() == 6,
              "the date rows were not appended");

        table.update_row(ids[1], {DateTimeValue{"2000-01-01T12:00"}, std::string("updated")});

        std::vector<std::string> values;
        for (auto cursor = table.rows(); auto row = cursor.next();) {
            values.push_back(cell_text(row->at(0)));
        }
        check(values.size() == 6, "unexpected row count after the date writes");
        check(values[0] == "2024-03-04T05:06:07",
              "a whole-second date did not round-trip: " + values[0]);
        check(values[1] == "2000-01-01T12:00",
              "the updated date lost its zero seconds: " + values[1]);
        check(values[2] == "2024-02-29T23:59:59.500",
              "a fractional second was not grouped in threes: " + values[2]);
        check(values[3] == "1765-06-14T12:45",
              "a pre-epoch date did not round-trip: " + values[3]);
        check(values[4] == "0100-06-14T12:45",
              "an early Access date did not round-trip: " + values[4]);
        check(values[5] == "NULL", "a NULL date was not left NULL: " + values[5]);

        table.delete_row(ids[2]);
        check(table.row_count_header() == 5, "deleting a date row did not lower the count");
    }

    {
        // Reopening proves the serial was stored, not just cached on the handle.
        auto reopened = Database::open(path);
        auto table = find_table(reopened, "Dated");
        std::vector<std::string> values;
        for (auto cursor = table.rows(); auto row = cursor.next();) {
            if (!std::holds_alternative<std::monostate>(row->at(0))) {
                values.push_back(cell_text(row->at(0)));
            }
        }
        check(values.size() == 4 && values[0] == "2024-03-04T05:06:07" &&
                  values[1] == "2000-01-01T12:00" && values[2] == "1765-06-14T12:45" &&
                  values[3] == "0100-06-14T12:45",
              "the reopened dates do not match what was written");
    }
}

void test_date_literal_defaults(DatabaseFormat format, const std::filesystem::path& path) {
    using rea_jackcess::PropertyInfo;
    using rea_jackcess::PropertyMapInfo;
    Database::create(path, format,
                     {TableBuilder("DateDefaults")
                          .add_column("DateOnly", DataType::short_date_time)
                          .add_column("LeapDay", DataType::short_date_time)
                          .add_column("EarlyDate", DataType::short_date_time)});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "DateDefaults");
        std::vector<PropertyMapInfo> maps{{"DateOnly", 1, {}}, {"LeapDay", 1, {}}, {"EarlyDate", 1, {}}};
        const auto set_default = [&](const std::string& column_name, const std::string& value) {
            auto map = std::find_if(maps.begin(), maps.end(), [&](const auto& candidate) {
                return candidate.name == column_name;
            });
            check(map != maps.end(), "date default property map is missing");
            PropertyInfo property{"DefaultValue", 0x0a, 0, {}, {}, {}};
            property.set_value(value);
            map->put(std::move(property));
        };
        set_default("DateOnly", "#1/2/2020#");
        set_default("LeapDay", "#2/29/2020#");
        set_default("EarlyDate", "#1/1/0100#");
        table.save_property_maps(maps);

        const auto ids = table.add_rows({
            {Value{}, Value{}, Value{}},
            {DateTimeValue{"2024-12-31T23:59:59"}, Value{}, Value{}},
        });
        check(ids.size() == 2, "date-literal defaults did not append the rows");
        std::vector<std::vector<std::string>> values;
        for (auto cursor = table.rows(); auto row = cursor.next();) {
            values.push_back({cell_text(row->at(0)), cell_text(row->at(1)), cell_text(row->at(2))});
        }
        check(values.size() == 2 &&
                  values[0] == std::vector<std::string>{"2020-01-02T00:00", "2020-02-29T00:00", "0100-01-01T00:00"} &&
                  values[1] == std::vector<std::string>{"2024-12-31T23:59:59", "2020-02-29T00:00", "0100-01-01T00:00"},
              "date-literal defaults or explicit-value precedence changed the date values");

        set_default("DateOnly", "#13/1/2020#");
        table.save_property_maps(maps);
        std::ifstream input(path, std::ios::binary);
        const std::string before(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
        try {
            table.add_row({Value{}, Value{}, Value{}});
            throw std::runtime_error("an invalid date-literal default was accepted");
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::unsupported_operation,
                  "an invalid date-literal default did not report unsupported operation");
        }
        std::ifstream after_input(path, std::ios::binary);
        const std::string after(std::istreambuf_iterator<char>{after_input}, std::istreambuf_iterator<char>{});
        check(before == after && table.row_count_header() == 2,
              "a rejected date-literal default modified the database");
    }

    auto reopened = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(reopened, "DateDefaults");
    check(table.row_count_header() == 2,
          "a rejected date-literal default changed the saved row count");
    std::vector<std::string> dates;
    for (auto cursor = table.rows(); auto row = cursor.next();) {
        dates.push_back(cell_text(row->at(0)));
    }
    check(dates == std::vector<std::string>{"2020-01-02T00:00", "2024-12-31T23:59:59"},
          "date-literal defaults or explicit values changed after reopening");
}

void test_csv(DatabaseFormat format, const std::filesystem::path& source,
              const std::filesystem::path& target) {
    std::ostringstream exported;
    {
        auto database = Database::open(source);
        auto table = find_table(database, "Dated");
        rea_jackcess::export_table_csv(table, exported, {});
    }
    const std::string expected =
        "2024-03-04T05:06:07,exact\n2000-01-01T12:00,updated\n"
        "1765-06-14T12:45,pre-epoch\n0100-06-14T12:45,early\n,null\n";
    check(exported.str() == expected,
          "the CSV export does not use the baseline date text:\n" + exported.str());

    Database::create(target, format, {dated_schema()});
    {
        auto database = Database::open(target, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Dated");
        std::istringstream input(expected);
        const auto rows = rea_jackcess::import_table_csv(table, input, {});
        check(rows == 5, "the CSV import did not consume every date row");

        std::ostringstream reexported;
        rea_jackcess::export_table_csv(table, reexported, {});
        check(reexported.str() == expected,
              "the date CSV round-trip changed the text:\n" + reexported.str());
    }
}

void test_rejections(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format, {dated_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Dated");

    const std::vector<std::string> invalid = {
        "2024-13-01T00:00",
        "2024-02-30T00:00",
        "2023-02-29T00:00",
        "2024-01-01T24:00",
        "2024-01-01T00:60",
        "2024-01-01T00:00:60",
        "2024-01-01T00:00:00.1234567891",
        "0099-12-31T00:00",
        "1899-12-30T00:00 trailing",
        "not a date",
    };
    for (const auto& text : invalid) {
        bool rejected = false;
        try {
            table.add_row({DateTimeValue{text}, std::string("bad")});
        } catch (const DatabaseError& error) {
            rejected = error.code() == DatabaseErrorCode::unsupported_operation;
        }
        check(rejected, "a date value should have been rejected: " + text);
    }
    check(table.row_count_header() == 0, "a rejected date still appended a row");

    for (const auto& text : invalid) {
        expect_rejected(text, "the shared date parser accepted: " + text);
    }
    check(rea_jackcess::detail::jet4_date_serial("2024-03-04T05:06:07").has_value(),
          "the shared date parser rejected a valid value");
    check(rea_jackcess::detail::jet4_date_serial("2024-03-04 05:06").has_value(),
          "the shared date parser should accept a space separator and no seconds");
    check(rea_jackcess::detail::jet4_date_serial("2024-03-04T05:06:07.5").has_value(),
          "the shared date parser should accept a one digit fraction");
    check(rea_jackcess::detail::jet4_date_serial("1765-06-14T12:45").has_value(),
          "the shared date parser should accept dates before the Access epoch");
}

void test_csv_rejects_bad_date(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format, {dated_schema()});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Dated");
    std::istringstream input("2024-02-30T00:00\n");
    bool rejected = false;
    try {
        (void)rea_jackcess::import_table_csv(table, input, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "a CSV date field outside the calendar should be an invalid_argument");
    check(table.row_count_header() == 0, "the rejected CSV date still appended a row");
}

int parse_format(const std::string& text) { return text == "v2003" ? 1 : 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: date_time_test <v2000|v2003> <fixture-database>\n";
        return 2;
    }
    TemporaryDirectory directory;
    const auto format = parse_format(argv[1]) == 1 ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    try {
        test_round_trip(format, directory.file("round.mdb"));
        test_date_literal_defaults(format, directory.file("defaults.mdb"));
        test_csv(format, directory.file("round.mdb"), directory.file("csv.mdb"));
        test_rejections(format, directory.file("reject.mdb"));
        test_csv_rejects_bad_date(format, directory.file("badcsv.mdb"));
        std::cout << "date and time tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "date and time tests failed: " << error.what() << '\n';
        return 1;
    }
}

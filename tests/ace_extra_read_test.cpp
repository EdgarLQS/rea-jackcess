#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::BlobValue;
using rea_jackcess::Database;
using rea_jackcess::DataType;
using rea_jackcess::DateTimeValue;
using rea_jackcess::DecimalValue;
using rea_jackcess::GuidValue;
using rea_jackcess::MemoValue;
using rea_jackcess::Row;
using rea_jackcess::Table;
using rea_jackcess::Value;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read the ACE fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Table find_table(const Database& database, const std::string& name) {
    for (const auto& table : database.tables()) {
        if (table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("fixture table is missing: " + name);
}

std::vector<Row> read_rows(const Table& table) {
    std::vector<Row> rows;
    auto cursor = table.rows();
    while (auto row = cursor.next()) {
        rows.push_back(*row);
    }
    return rows;
}

const std::int32_t& as_long(const Value& value, const std::string& label) {
    const auto* number = std::get_if<std::int32_t>(&value);
    check(number != nullptr, label + ": expected LONG");
    return *number;
}

void check_null(const Value& value, const std::string& label) {
    check(std::holds_alternative<std::monostate>(value), label + ": expected NULL");
}

void check_text(const Value& value, const std::string& expected, const std::string& label) {
    const auto* text = std::get_if<std::string>(&value);
    check(text != nullptr, label + ": expected TEXT");
    check(*text == expected, label + ": unexpected TEXT payload");
}

void check_date(const Value& value, const std::string& expected, const std::string& label) {
    const auto* date = std::get_if<DateTimeValue>(&value);
    check(date != nullptr, label + ": expected a date");
    check(date->iso8601 == expected, label + ": expected " + expected + ", got " + date->iso8601);
}

// The generator repeats this 19-character block until the memo passes 5000 characters, then
// appends a non-ASCII tail.
std::string long_memo() {
    std::string text;
    while (text.size() < 5000) {
        text += "ace-memo-0123456789";
    }
    text += "-尾";
    return text;
}

void check_v2016(Database& database) {
    check(database.format().version == rea_jackcess::EngineVersion::ace2016, "fixture is not V2016");
    Table table = find_table(database, "V2016Data");
    const auto& columns = table.columns();
    check(columns.size() == 8, "unexpected V2016Data column count");
    check(columns[0].name == "Id" && columns[0].type == DataType::int32, "Id must be a LONG");
    check(columns[1].name == "Flag" && columns[1].type == DataType::boolean, "Flag must be BOOLEAN");
    check(columns[2].name == "Name" && columns[2].type == DataType::text && columns[2].size == 60,
          "Name must be a TEXT column with 30 units");
    check(columns[3].name == "Amount" && columns[3].type == DataType::money, "Amount must be MONEY");
    check(columns[4].name == "Created" && columns[4].type == DataType::short_date_time,
          "Created must be SHORT_DATE_TIME");
    check(columns[5].name == "Ref" && columns[5].type == DataType::guid, "Ref must be GUID");
    check(columns[6].name == "Memo" && columns[6].type == DataType::memo, "Memo must be MEMO");
    check(columns[7].name == "Payload" && columns[7].type == DataType::ole, "Payload must be OLE");
    check(table.row_count_header() == 3, "unexpected V2016Data row count");

    const auto rows = read_rows(table);
    check(rows.size() == 3, "unexpected V2016Data live row count");

    check(as_long(rows[0].at(0), "row 1 Id") == 1, "row 1 Id");
    const auto* flag = std::get_if<bool>(&rows[0].at(1));
    check(flag != nullptr && *flag, "row 1 Flag must be true");
    check_text(rows[0].at(2), "V2016 中文 ✓", "row 1 Name");
    const auto* amount = std::get_if<DecimalValue>(&rows[0].at(3));
    check(amount != nullptr && amount->text == "9876.5432", "row 1 Amount");
    check_date(rows[0].at(4), "2021-06-07T08:09:10", "row 1 Created");
    const auto* guid = std::get_if<GuidValue>(&rows[0].at(5));
    check(guid != nullptr, "row 1 Ref must be a GUID");
    std::string digits;
    for (const char character : guid->text) {
        if (character != '{' && character != '}') {
            digits.push_back(character >= 'A' && character <= 'F'
                                 ? static_cast<char>(character + ('a' - 'A'))
                                 : character);
        }
    }
    check(digits == "aabbccdd-1122-3344-5566-77889900aabb", "row 1 Ref value");
    const auto* memo = std::get_if<MemoValue>(&rows[0].at(6));
    check(memo != nullptr, "row 1 Memo must be a MEMO value");
    check(memo->text == long_memo(), "row 1 Memo payload");
    const auto* payload = std::get_if<BlobValue>(&rows[0].at(7));
    check(payload != nullptr && payload->bytes == std::vector<std::uint8_t>({1, 2, 3, 4, 5}),
          "row 1 Payload");

    check(as_long(rows[1].at(0), "row 2 Id") == 2, "row 2 Id");
    const auto* null_flag = std::get_if<bool>(&rows[1].at(1));
    check(null_flag != nullptr && !*null_flag, "row 2 Flag: a NULL BOOLEAN reads back as FALSE");
    for (std::size_t index = 2; index < 8; ++index) {
        check_null(rows[1].at(index), "row 2 column " + std::to_string(index + 1));
    }

    check(as_long(rows[2].at(0), "row 3 Id") == 3, "row 3 Id");
    check_text(rows[2].at(2), "", "row 3 Name: empty text is not NULL");
    const auto* empty_memo = std::get_if<MemoValue>(&rows[2].at(6));
    check(empty_memo != nullptr && empty_memo->text.empty(), "row 3 Memo: empty memo is not NULL");
    const auto* empty_payload = std::get_if<BlobValue>(&rows[2].at(7));
    check(empty_payload != nullptr && empty_payload->bytes.empty(), "row 3 Payload: empty OLE");
}

void check_v2019(Database& database) {
    check(database.format().version == rea_jackcess::EngineVersion::ace2019, "fixture is not V2019");
    Table table = find_table(database, "V2019Data");
    const auto& columns = table.columns();
    check(columns.size() == 3, "unexpected V2019Data column count");
    check(columns[1].name == "Stamp" && columns[1].type == DataType::extended_date_time,
          "Stamp must be EXT_DATE_TIME");
    check(table.row_count_header() == 6, "unexpected V2019Data row count");

    const auto rows = read_rows(table);
    check(rows.size() == 6, "unexpected V2019Data live row count");
    const std::vector<std::string> notes = {"null", "nanos", "year-100", "year-1265", "year-1765",
                                            "pre-epoch"};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        check(as_long(rows[index].at(0), "row Id") == static_cast<std::int32_t>(index + 1),
              "unexpected V2019Data row order");
        check_text(rows[index].at(2), notes[index], "row " + std::to_string(index + 1) + " Note");
    }
    check_null(rows[0].at(1), "row 1 Stamp");
    // EXT_DATE_TIME keeps 100 ns ticks, so the JAR stores 123456789 ns as 123456700.
    check_date(rows[1].at(1), "2020-01-02T03:04:05.123456700", "row 2 Stamp");
    check_date(rows[2].at(1), "0100-03-04T05:06:07", "row 3 Stamp");
    check_date(rows[3].at(1), "1265-07-08T09:10:11", "row 4 Stamp");
    check_date(rows[4].at(1), "1765-12-13T14:15:16", "row 5 Stamp");
    check_date(rows[5].at(1), "1899-12-29T18:30", "row 6 Stamp");
}

// MSysResources.Data is a COMPLEX_TYPE column without a catalog link. Jackcess returns a complex
// value foreign key, so the reader must expose the stored 4-byte number instead of raw bytes.
void check_system_resources(Database& database) {
    Table table = find_table(database, "MSysResources");
    const auto& columns = table.columns();
    const auto data = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
        return column.name == "Data";
    });
    check(data != columns.end(), "MSysResources.Data is missing");
    check(data->type == DataType::complex, "MSysResources.Data must stay a complex column");

    const auto rows = read_rows(table);
    check(rows.size() == 1, "unexpected MSysResources row count");
    check(as_long(rows[0].at(static_cast<std::size_t>(data - columns.begin())), "Data") == 1,
          "MSysResources.Data must expose the complex value foreign key");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: ace_extra_read <fixture.accdb> <v2016|v2019>\n";
        return 2;
    }
    const std::filesystem::path fixture(argv[1]);
    const std::string mode(argv[2]);
    if (!std::filesystem::exists(fixture)) {
        // Generated on demand by tools/validation; never committed. CTest maps 77 to skipped.
        std::cout << "ACE " << mode << " fixture is missing, run the generator first: " << fixture << '\n';
        return 77;
    }
    try {
        const std::string before = read_bytes(fixture);
        {
            Database database = Database::open(fixture);
            if (mode == "v2016") {
                check_v2016(database);
            } else if (mode == "v2019") {
                check_v2019(database);
            } else {
                throw std::runtime_error("unknown mode: " + mode);
            }
            check_system_resources(database);
        }
        check(read_bytes(fixture) == before, "a read-only open modified the ACE fixture");
        std::cout << "ACE " << mode << " read checks passed: " << fixture.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ACE " << mode << " read check failed: " << error.what() << '\n';
        return 1;
    }
}

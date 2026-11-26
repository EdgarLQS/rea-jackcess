#include "rea_jackcess/database.hpp"

#include <algorithm>
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
using rea_jackcess::DataType;
using rea_jackcess::GuidValue;
using rea_jackcess::DecimalValue;
using rea_jackcess::DateTimeValue;
using rea_jackcess::MemoValue;
using rea_jackcess::BlobValue;
using rea_jackcess::Row;
using rea_jackcess::Table;
using rea_jackcess::Value;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// The fixture stores 9000 characters plus a non-ASCII tail in rows 3 and 4, so the MEMO payload
// spans several data pages and row 4 arrives through an overflow row after the update.
std::string long_memo() {
    std::string text;
    while (text.size() < 9000) {
        text += "0123456789abcdef";
    }
    text += "-尾-𝄞";
    return text;
}

std::vector<std::uint8_t> ramp(std::size_t size, unsigned int start) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::uint8_t>((start + index) & 0xffu);
    }
    return bytes;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read the Jet4 data fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'F' ? character + ('a' - 'A') : character);
    });
    return value;
}

// Guid text keeps whatever brace and case style the reader emits; only the value matters here.
std::string guid_digits(const Value& value) {
    const auto* guid = std::get_if<GuidValue>(&value);
    check(guid != nullptr, "expected a GUID value");
    std::string digits;
    for (const char character : lowercase(guid->text)) {
        if (character != '{' && character != '}') {
            digits.push_back(character);
        }
    }
    return digits;
}

// Jackcess omits seconds when they are zero, so compare on the minute.
std::string date_prefix(const Value& value) {
    const auto* date = std::get_if<DateTimeValue>(&value);
    check(date != nullptr, "expected a date value");
    return date->iso8601.substr(0, 16);
}

void check_int(const Value& value, std::int64_t expected, const std::string& label) {
    const bool matches =
        (std::get_if<std::int8_t>(&value) != nullptr && *std::get_if<std::int8_t>(&value) == expected) ||
        (std::get_if<std::int16_t>(&value) != nullptr && *std::get_if<std::int16_t>(&value) == expected) ||
        (std::get_if<std::int32_t>(&value) != nullptr && *std::get_if<std::int32_t>(&value) == expected) ||
        (std::get_if<std::int64_t>(&value) != nullptr && *std::get_if<std::int64_t>(&value) == expected);
    check(matches, label + ": unexpected integer payload");
}

void check_null(const Value& value, const std::string& label) {
    check(std::holds_alternative<std::monostate>(value), label + ": expected NULL");
}

void check_text(const Value& value, const std::string& expected, const std::string& label) {
    const auto* text = std::get_if<std::string>(&value);
    check(text != nullptr, label + ": expected TEXT");
    check(*text == expected, label + ": unexpected TEXT payload");
}

void check_blob(const Value& value, const std::vector<std::uint8_t>& expected, const std::string& label) {
    const auto* blob = std::get_if<BlobValue>(&value);
    check(blob != nullptr, label + ": expected binary payload");
    check(blob->bytes == expected, label + ": unexpected binary payload");
}

void check_memo(const Value& value, const std::string& expected, const std::string& label) {
    const auto* memo = std::get_if<MemoValue>(&value);
    check(memo != nullptr, label + ": expected MEMO");
    check(memo->text == expected, label + ": unexpected MEMO payload");
}

void check_money(const Value& value, const std::string& expected, const std::string& label) {
    const auto* money = std::get_if<DecimalValue>(&value);
    check(money != nullptr, label + ": expected MONEY");
    check(money->text == expected, label + ": unexpected MONEY payload " + money->text);
}

void check_schema(const Table& table) {
    const auto& columns = table.columns();
    check(columns.size() == 14, "unexpected column count");
    const std::vector<std::string> names = {
        "Id", "Flag", "ByteVal", "IntVal", "LongVal", "FloatVal", "DoubleVal", "MoneyVal",
        "DateVal", "GuidVal", "TextVal", "BinVal", "MemoVal", "OleVal"};
    const std::vector<DataType> types = {
        DataType::int32, DataType::boolean, DataType::byte, DataType::int16, DataType::int32,
        DataType::float32, DataType::float64, DataType::money, DataType::short_date_time,
        DataType::guid, DataType::text, DataType::binary, DataType::memo, DataType::ole};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        check(columns[index].name == names[index], "unexpected column name at ordinal " +
                                                       std::to_string(index + 1));
        check(columns[index].type == types[index], "unexpected column type for " + names[index]);
        check(columns[index].nullable && *columns[index].nullable,
              "every fixture column is nullable: " + names[index]);
    }
    // TEXT length is stored in bytes: 50 UTF-16 units, and BINARY stays at 32 bytes.
    check(columns[10].size == 100, "unexpected TEXT length in bytes");
    check(columns[11].size == 32, "unexpected BINARY length");
    check(columns[12].size == 0 && columns[13].size == 0, "MEMO and OLE carry no fixed length");

    const auto& indexes = table.indexes();
    check(indexes.size() == 1, "the fixture declares one index");
    check(indexes[0].name == "PrimaryKey" && indexes[0].primary_key && indexes[0].unique,
          "unexpected primary key definition");
    check(indexes[0].columns.size() == 1 && indexes[0].columns[0].name == "Id",
          "primary key no longer covers Id");
    check(indexes[0].unique_entry_count == 6, "unexpected index entry count");
}

void check_row(std::int32_t expected_id, const Row& row) {
    const std::string label = "row " + std::to_string(expected_id);
    check(row.size() == 14, label + ": unexpected value count");
    check_int(row.at(0), expected_id, label + " Id");
    switch (expected_id) {
        case 1:
            check(std::get_if<bool>(&row.at(1)) != nullptr && *std::get_if<bool>(&row.at(1)),
                  "row 1 Flag: expected true");
            check_int(row.at(2), 127, "row 1 ByteVal");
            check_int(row.at(3), 32767, "row 1 IntVal");
            check_int(row.at(4), 2147483647, "row 1 LongVal");
            check(std::get_if<float>(&row.at(5)) != nullptr && *std::get_if<float>(&row.at(5)) == 1.5f,
                  "row 1 FloatVal");
            check(std::get_if<double>(&row.at(6)) != nullptr && *std::get_if<double>(&row.at(6)) == -2.25,
                  "row 1 DoubleVal");
            check_money(row.at(7), "1234.5678", "row 1 MoneyVal");
            check(date_prefix(row.at(8)) == "2020-01-02T03:04", "row 1 DateVal: unexpected timestamp");
            check(guid_digits(row.at(9)) == "00112233-4455-6677-8899-aabbccddeeff",
                  "row 1 GuidVal: unexpected value");
            check_text(row.at(10), "中文 Ünïcode ✓ 𝄞",
                       "row 1 TextVal");
            check_blob(row.at(11), ramp(16, 0), "row 1 BinVal");
            check_memo(row.at(12), "短备忘录", "row 1 MemoVal");
            check_blob(row.at(13), ramp(24, 0x80), "row 1 OleVal");
            break;
        case 2:
            // A NULL BOOLEAN is stored in the row NULL mask and reads back as FALSE.
            check(std::get_if<bool>(&row.at(1)) != nullptr && !*std::get_if<bool>(&row.at(1)),
                  "row 2 Flag: a NULL BOOLEAN reads back as FALSE");
            for (std::size_t index = 2; index < 14; ++index) {
                check_null(row.at(index), "row 2 column " + std::to_string(index + 1));
            }
            break;
        case 3:
            check_memo(row.at(12), long_memo(), "row 3 MemoVal");
            break;
        case 4:
            check_memo(row.at(12), long_memo(), "row 4 MemoVal");
            break;
        case 5:
            check(std::get_if<bool>(&row.at(1)) != nullptr && !*std::get_if<bool>(&row.at(1)),
                  "row 5 Flag: expected false");
            check_int(row.at(2), -128, "row 5 ByteVal");
            check_int(row.at(3), -32768, "row 5 IntVal");
            check_int(row.at(4), -2147483647 - 1, "row 5 LongVal");
            check(std::get_if<float>(&row.at(5)) != nullptr && *std::get_if<float>(&row.at(5)) == -0.5f,
                  "row 5 FloatVal");
            check(std::get_if<double>(&row.at(6)) != nullptr &&
                      *std::get_if<double>(&row.at(6)) == 1.7976931348623157e308,
                  "row 5 DoubleVal");
            check_money(row.at(7), "-0.0001", "row 5 MoneyVal");
            check(date_prefix(row.at(8)) == "1899-12-29T18:30", "row 5 DateVal: expected a pre-epoch date");
            check(guid_digits(row.at(9)) == "00000000-0000-0000-0000-000000000000",
                  "row 5 GuidVal: expected the zero GUID");
            check_text(row.at(10), "", "row 5 TextVal: an empty string is not NULL");
            check_blob(row.at(11), {}, "row 5 BinVal: an empty BLOB is not NULL");
            check_blob(row.at(13), {}, "row 5 OleVal: an empty OLE payload is not NULL");
            check_memo(row.at(12), "", "row 5 MemoVal: an empty memo is not NULL");
            break;
        case 6:
            check_text(row.at(10), "quote\" backslash\\ tab\t newline\n cr\r end", "row 6 TextVal");
            check_memo(row.at(12), "第一行\r\n第二行 𝄞",
                       "row 6 MemoVal");
            break;
        default:
            throw std::runtime_error("unexpected row id in the Jet4 data fixture");
    }
}

std::vector<std::int32_t> scan(std::vector<Row>& rows) {
    std::vector<std::int32_t> ids;
    ids.reserve(rows.size());
    for (const auto& row : rows) {
        const auto* id = std::get_if<std::int32_t>(&row.at(0));
        check(id != nullptr, "Id is always a LONG");
        ids.push_back(*id);
    }
    return ids;
}

std::vector<std::int32_t> scan_cursor(rea_jackcess::RowCursor cursor) {
    std::vector<std::int32_t> ids;
    while (auto row = cursor.next()) {
        const auto* id = std::get_if<std::int32_t>(&row->at(0));
        check(id != nullptr, "Id is always a LONG");
        ids.push_back(*id);
    }
    return ids;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: jet4_data_read <fixture.mdb>\n";
        return 2;
    }
    const std::filesystem::path fixture(argv[1]);
    if (!std::filesystem::exists(fixture)) {
        // Generated on demand by tools/validation; never committed. CTest maps 77 to skipped.
        std::cout << "Jet4 data fixture is missing, run the generator first: " << fixture << '\n';
        return 77;
    }
    try {
        const std::string before = read_bytes(fixture);
        {
            Database database = Database::open(fixture);
            const auto tables = database.tables();
            const auto found = std::find_if(tables.begin(), tables.end(), [](const auto& table) {
                return table.is_user_table();
            });
            check(found != tables.end(), "the fixture has no user table");
            check(found->name() == "DataRows", "unexpected user table name");

            check_schema(*found);
            check(found->row_count_header() == 6, "unexpected row count header");

            std::vector<Row> rows;
            auto cursor = found->rows();
            while (auto row = cursor.next()) {
                rows.push_back(*row);
            }
            check(rows.size() == 6, "unexpected number of live rows");
            for (std::size_t index = 0; index < rows.size(); ++index) {
                check_row(static_cast<std::int32_t>(index + 1), rows[index]);
            }
            check((scan(rows) == std::vector<std::int32_t>{1, 2, 3, 4, 5, 6}),
                  "sequential scan order changed");

            check((scan_cursor(found->rows_by_index("PrimaryKey")) ==
                   std::vector<std::int32_t>{1, 2, 3, 4, 5, 6}),
                  "primary key scan order changed");

            auto reverse = found->rows_by_index_reverse("PrimaryKey");
            std::vector<std::int32_t> reversed;
            while (auto row = reverse.next()) {
                reversed.push_back(*std::get_if<std::int32_t>(&row->at(0)));
            }
            check((reversed == std::vector<std::int32_t>{6, 5, 4, 3, 2, 1}),
                  "reverse primary key scan order changed");

            // Row 4 was updated from a short memo into a cross-page one, so the lookup also proves
            // the index still points at the overflow row.
            const Value key = std::int32_t{4};
            const auto found_row = found->find_row_by_index("PrimaryKey", key);
            check(found_row.has_value(), "primary key lookup missed row 4");
            check_row(4, *found_row);
        }
        const std::string after = read_bytes(fixture);
        check(before == after, "a read-only open modified the fixture");
        std::cout << "Jet4 data read checks passed: " << fixture.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet4 data read check failed: " << error.what() << '\n';
        return 1;
    }
}

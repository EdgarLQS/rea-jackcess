#include "rea_jackcess/csv.hpp"

#include "rea_jackcess/database_file.hpp"

#include "../value/jet4_value.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rea_jackcess {
namespace {

constexpr std::size_t import_chunk_rows = 500;

[[noreturn]] void unsupported_column(const Table& table, const ColumnInfo& column) {
    DatabaseErrorContext context;
    context.table_name = table.name();
    context.column_name = column.name;
    throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                        "CSV import and export do not support this column type yet",
                        {},
                        std::nullopt,
                        std::move(context));
}

[[noreturn]] void invalid_field(const Table& table, const ColumnInfo& column, const std::string& reason) {
    throw std::invalid_argument("CSV field for " + table.name() + "." + column.name + " is invalid: " + reason);
}

// Java's Double.toString/Float.toString rules: the shortest text that round-trips, plain form for
// 1e-3 <= |value| < 1e7 with at least one fractional digit, scientific form otherwise, upper-case
// E and an exponent without a plus sign or leading zeros.
template <typename Float>
std::string java_floating_point(Float value) {
    static_assert(std::is_floating_point_v<Float>);
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Infinity" : "Infinity";
    }
    if (value == Float{0}) {
        return std::signbit(value) ? "-0.0" : "0.0";
    }
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    const std::string text(buffer, converted.ptr);

    std::size_t position = 0;
    const bool negative = text[position] == '-';
    if (negative) {
        ++position;
    }
    const auto exponent_at = std::min(text.find_first_of("eE", position), text.size());
    int exponent = 0;
    if (exponent_at < text.size()) {
        exponent = std::stoi(text.substr(exponent_at + 1));
    }
    const auto point = text.find('.', position);
    const bool has_point = point != std::string::npos && point < exponent_at;
    const auto integer_end = has_point ? point : exponent_at;
    const auto fraction_begin = has_point ? point + 1 : exponent_at;
    std::string digits = text.substr(position, integer_end - position) +
                         text.substr(fraction_begin, exponent_at - fraction_begin);
    const int fraction_length = static_cast<int>(exponent_at - fraction_begin);

    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        return negative ? "-0.0" : "0.0";
    }
    digits.erase(0, first_nonzero);
    const auto exponent10 = exponent - fraction_length + static_cast<int>(digits.size()) - 1;

    std::string result = negative ? "-" : "";
    if (exponent10 >= -3 && exponent10 < 7) {
        if (exponent10 >= static_cast<int>(digits.size()) - 1) {
            result += digits;
            result.append(static_cast<std::size_t>(exponent10 - (static_cast<int>(digits.size()) - 1)), '0');
            result += ".0";
        } else if (exponent10 >= 0) {
            result += digits.substr(0, static_cast<std::size_t>(exponent10) + 1);
            result += '.';
            result += digits.substr(static_cast<std::size_t>(exponent10) + 1);
        } else {
            result += "0.";
            result.append(static_cast<std::size_t>(-exponent10 - 1), '0');
            result += digits;
        }
    } else {
        result += digits.substr(0, 1);
        result += '.';
        result += digits.size() > 1 ? digits.substr(1) : std::string("0");
        result += 'E';
        result += std::to_string(exponent10);
    }
    return result;
}

std::string money_text(const Table& table, const ColumnInfo& column, const std::string& text) {
    std::string digits;
    bool negative = false;
    std::size_t position = 0;
    if (position < text.size() && (text[position] == '-' || text[position] == '+')) {
        negative = text[position] == '-';
        ++position;
    }
    std::string integer;
    std::string fraction;
    for (; position < text.size() && text[position] != '.'; ++position) {
        if (text[position] < '0' || text[position] > '9') {
            invalid_field(table, column, "not a decimal amount");
        }
        integer.push_back(text[position]);
    }
    if (position < text.size()) {
        for (++position; position < text.size(); ++position) {
            if (text[position] < '0' || text[position] > '9') {
                invalid_field(table, column, "not a decimal amount");
            }
            fraction.push_back(text[position]);
        }
    }
    if (integer.empty()) {
        integer = "0";
    }
    if (fraction.size() > 4) {
        invalid_field(table, column, "MONEY keeps four fractional digits");
    }
    fraction.append(4 - fraction.size(), '0');
    return (negative ? "-" : "") + integer + "." + fraction;
}

std::string guid_text(const std::string& text) {
    std::string body = text;
    if (body.size() >= 2 && body.front() == '{' && body.back() == '}') {
        body = body.substr(1, body.size() - 2);
    }
    for (auto& character : body) {
        if (character >= 'a' && character <= 'f') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return "{" + body + "}";
}

std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 3 + bytes.size() / 4);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            result += (index % 4 == 0) ? "  " : " ";
        }
        result.push_back(digits[(bytes[index] >> 4) & 0x0f]);
        result.push_back(digits[bytes[index] & 0x0f]);
    }
    return result;
}

bool needs_quoting(const std::string& text, const CsvOptions& options) {
    return text.find(options.delimiter) != std::string::npos ||
           text.find(options.quote) != std::string::npos ||
           text.find('\r') != std::string::npos ||
           text.find('\n') != std::string::npos;
}

void write_field(std::ostream& output, const std::string& text, const CsvOptions& options) {
    if (!needs_quoting(text, options)) {
        output << text;
        return;
    }
    output << options.quote;
    for (const char character : text) {
        if (character == options.quote) {
            output << options.quote;
        }
        output << character;
    }
    output << options.quote;
}

std::string column_text(const Table& table, const ColumnInfo& column, const Value& value) {
    switch (column.type) {
        case DataType::boolean:
            if (const auto* flag = std::get_if<bool>(&value)) {
                return *flag ? "true" : "false";
            }
            break;
        case DataType::byte:
            if (const auto* number = std::get_if<std::int8_t>(&value)) {
                return std::to_string(static_cast<int>(*number));
            }
            break;
        case DataType::int16:
            if (const auto* number = std::get_if<std::int16_t>(&value)) {
                return std::to_string(*number);
            }
            break;
        case DataType::int32:
            if (const auto* number = std::get_if<std::int32_t>(&value)) {
                return std::to_string(*number);
            }
            break;
        case DataType::int64:
            if (const auto* number = std::get_if<std::int64_t>(&value)) {
                return std::to_string(*number);
            }
            break;
        case DataType::float32:
            if (const auto* number = std::get_if<float>(&value)) {
                return java_floating_point(*number);
            }
            break;
        case DataType::float64:
            if (const auto* number = std::get_if<double>(&value)) {
                return java_floating_point(*number);
            }
            break;
        case DataType::money:
            if (const auto* amount = std::get_if<DecimalValue>(&value)) {
                return money_text(table, column, amount->text);
            }
            break;
        case DataType::guid:
            if (const auto* guid = std::get_if<GuidValue>(&value)) {
                return guid_text(guid->text);
            }
            break;
        case DataType::short_date_time:
            // ExportUtil writes the value's toString(), i.e. the ISO_LOCAL_DATE_TIME text.
            if (const auto* date = std::get_if<DateTimeValue>(&value)) {
                return date->iso8601;
            }
            break;
        case DataType::text:
            if (const auto* text = std::get_if<std::string>(&value)) {
                return *text;
            }
            break;
        case DataType::memo:
            if (const auto* memo = std::get_if<MemoValue>(&value)) {
                // Decoded memos carry both the decoded text and the raw payload; only fall back
                // to hex when the payload has no text form.
                return memo->text.empty() && !memo->raw_bytes.empty() ? hex_bytes(memo->raw_bytes)
                                                                     : memo->text;
            }
            break;
        case DataType::binary:
        case DataType::ole:
            if (const auto* blob = std::get_if<BlobValue>(&value)) {
                return hex_bytes(blob->bytes);
            }
            break;
        default:
            break;
    }
    if (std::holds_alternative<std::monostate>(value)) {
        return {};
    }
    unsupported_column(table, column);
}

bool is_supported_type(DataType type) {
    switch (type) {
        case DataType::boolean:
        case DataType::byte:
        case DataType::int16:
        case DataType::int32:
        case DataType::int64:
        case DataType::float32:
        case DataType::float64:
        case DataType::money:
        case DataType::guid:
        case DataType::short_date_time:
        case DataType::text:
        case DataType::memo:
        case DataType::binary:
        case DataType::ole:
            return true;
        default:
            return false;
    }
}

std::string lower_ascii(std::string text) {
    for (auto& character : text) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return text;
}

long long parse_integer(const Table& table, const ColumnInfo& column, const std::string& text) {
    long long value = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec != std::errc() || converted.ptr != text.data() + text.size()) {
        invalid_field(table, column, "not an integer");
    }
    return value;
}

double parse_floating_point(const Table& table, const ColumnInfo& column, const std::string& text) {
    if (text == "NaN") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text == "Infinity") {
        return std::numeric_limits<double>::infinity();
    }
    if (text == "-Infinity") {
        return -std::numeric_limits<double>::infinity();
    }
    std::size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        invalid_field(table, column, "not a floating point number");
    }
    if (consumed != text.size()) {
        invalid_field(table, column, "not a floating point number");
    }
    return value;
}

std::vector<std::uint8_t> parse_hex_bytes(const Table& table, const ColumnInfo& column, const std::string& text) {
    std::vector<std::uint8_t> bytes;
    int high = -1;
    for (const char character : text) {
        if (character == ' ' || character == '\t') {
            continue;
        }
        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        } else {
            invalid_field(table, column, "not a hexadecimal byte string");
        }
        if (high < 0) {
            high = digit;
            continue;
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | digit));
        high = -1;
    }
    if (high >= 0) {
        invalid_field(table, column, "hexadecimal byte string has an odd number of digits");
    }
    return bytes;
}

Value field_value(const Table& table, const ColumnInfo& column, const std::string& field) {
    if (field.empty()) {
        return Value{};
    }
    switch (column.type) {
        case DataType::boolean: {
            const auto lowered = lower_ascii(field);
            if (lowered == "true" || lowered == "1") {
                return Value{true};
            }
            if (lowered == "false" || lowered == "0") {
                return Value{false};
            }
            invalid_field(table, column, "expected true or false");
        }
        case DataType::byte: {
            const auto value = parse_integer(table, column, field);
            if (value < std::numeric_limits<std::int8_t>::min() || value > std::numeric_limits<std::int8_t>::max()) {
                invalid_field(table, column, "outside the BYTE range");
            }
            return Value{static_cast<std::int8_t>(value)};
        }
        case DataType::int16: {
            const auto value = parse_integer(table, column, field);
            if (value < std::numeric_limits<std::int16_t>::min() || value > std::numeric_limits<std::int16_t>::max()) {
                invalid_field(table, column, "outside the SHORT range");
            }
            return Value{static_cast<std::int16_t>(value)};
        }
        case DataType::int32: {
            const auto value = parse_integer(table, column, field);
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
                invalid_field(table, column, "outside the LONG range");
            }
            return Value{static_cast<std::int32_t>(value)};
        }
        case DataType::int64:
            return Value{static_cast<std::int64_t>(parse_integer(table, column, field))};
        case DataType::float32: {
            const auto value = parse_floating_point(table, column, field);
            if (std::isfinite(value) &&
                (value > std::numeric_limits<float>::max() || value < std::numeric_limits<float>::lowest())) {
                invalid_field(table, column, "outside the SINGLE range");
            }
            return Value{static_cast<float>(value)};
        }
        case DataType::float64:
            return Value{parse_floating_point(table, column, field)};
        case DataType::money:
            (void)money_text(table, column, field);
            return Value{DecimalValue{field}};
        case DataType::guid: {
            std::string text = field;
            if (text.size() >= 2 && text.front() == '{' && text.back() == '}') {
                text = text.substr(1, text.size() - 2);
            }
            return Value{GuidValue{text}};
        }
        case DataType::short_date_time: {
            std::string error;
            if (!detail::jet4_date_serial(field, &error)) {
                invalid_field(table, column, error);
            }
            return Value{DateTimeValue{field}};
        }
        case DataType::text:
            return Value{field};
        case DataType::memo:
            return Value{MemoValue{field, {}}};
        case DataType::binary:
        case DataType::ole:
            return Value{BlobValue{parse_hex_bytes(table, column, field)}};
        default:
            unsupported_column(table, column);
    }
}

// Streaming record reader: honours the quote character, doubled quotes and newlines inside a
// quoted field, and accepts CRLF or LF record separators.
void read_records(std::istream& input,
                  const CsvOptions& options,
                  const std::function<void(std::vector<std::string>&)>& consume) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    bool field_started = false;
    bool any = false;
    char character = 0;
    while (input.get(character)) {
        any = true;
        if (quoted) {
            if (character == options.quote) {
                if (input.peek() == options.quote) {
                    input.get(character);
                    field.push_back(options.quote);
                    continue;
                }
                quoted = false;
                continue;
            }
            field.push_back(character);
            continue;
        }
        if (character == options.quote && !field_started) {
            quoted = true;
            field_started = true;
            continue;
        }
        if (character == options.delimiter) {
            fields.push_back(std::move(field));
            field.clear();
            field_started = false;
            continue;
        }
        if (character == '\n' || character == '\r') {
            if (character == '\r' && input.peek() == '\n') {
                input.get(character);
            }
            fields.push_back(std::move(field));
            field.clear();
            field_started = false;
            consume(fields);
            fields.clear();
            any = false;
            continue;
        }
        field.push_back(character);
        field_started = true;
    }
    if (quoted) {
        throw std::invalid_argument("CSV input ends inside a quoted field");
    }
    if (any || !field.empty() || !fields.empty()) {
        fields.push_back(std::move(field));
        consume(fields);
    }
}

struct ColumnMapping {
    std::size_t column;
    std::size_t field;
};

}

void export_table_csv(const Table& table, std::ostream& output, const CsvOptions& options) {
    const auto& all_columns = table.columns();
    std::vector<const ColumnInfo*> columns;
    std::vector<std::string> column_names;
    if (options.columns.empty()) {
        columns.reserve(all_columns.size());
        column_names.reserve(all_columns.size());
        for (const auto& column : all_columns) {
            columns.push_back(&column);
            column_names.push_back(column.name);
        }
    } else {
        columns.reserve(options.columns.size());
        column_names.reserve(options.columns.size());
        for (const auto& requested_name : options.columns) {
            const auto requested_key = lower_ascii(requested_name);
            const auto column = std::find_if(all_columns.begin(), all_columns.end(), [&](const auto& candidate) {
                return lower_ascii(candidate.name) == requested_key;
            });
            if (column == all_columns.end()) {
                throw std::invalid_argument("CSV export column does not exist: " + requested_name);
            }
            columns.push_back(&*column);
            column_names.push_back(column->name);
        }
    }
    for (const auto* column : columns) {
        if (!is_supported_type(column->type)) {
            unsupported_column(table, *column);
        }
    }
    if (options.header) {
        for (std::size_t index = 0; index < columns.size(); ++index) {
            if (index != 0) {
                output << options.delimiter;
            }
            write_field(output, columns[index]->name, options);
        }
        output << '\n';
    }
    auto cursor = table.rows(column_names, options.row_filter);
    while (const auto row = cursor.next()) {
        for (std::size_t index = 0; index < columns.size(); ++index) {
            if (index != 0) {
                output << options.delimiter;
            }
            write_field(output, column_text(table, *columns[index], row->at(index)), options);
        }
        output << '\n';
    }
}

void export_table_csv(const Table& table, const std::filesystem::path& path, const CsvOptions& options) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw DatabaseError(DatabaseErrorCode::io_error, "Cannot open the CSV file for writing", path);
    }
    export_table_csv(table, output, options);
    output.flush();
    if (!output) {
        throw DatabaseError(DatabaseErrorCode::io_error, "Cannot write the CSV file", path);
    }
}

std::size_t import_table_csv(Table& table, std::istream& input, const CsvOptions& options) {
    const auto& columns = table.columns();
    for (const auto& column : columns) {
        if (!is_supported_type(column.type)) {
            unsupported_column(table, column);
        }
    }
    std::vector<ColumnMapping> mapping;
    if (!options.header) {
        for (std::size_t index = 0; index < columns.size(); ++index) {
            mapping.push_back({index, index});
        }
    }
    bool header_seen = false;
    std::size_t imported = 0;
    std::vector<std::vector<Value>> rows;
    rows.reserve(import_chunk_rows);

    const auto flush = [&] {
        if (rows.empty()) {
            return;
        }
        table.add_rows(rows);
        imported += rows.size();
        rows.clear();
    };

    read_records(input, options, [&](std::vector<std::string>& fields) {
        if (options.header && !header_seen) {
            header_seen = true;
            mapping.clear();
            for (std::size_t index = 0; index < fields.size(); ++index) {
                const auto key = lower_ascii(fields[index]);
                const auto column = std::find_if(columns.begin(), columns.end(), [&](const auto& candidate) {
                    return lower_ascii(candidate.name) == key;
                });
                if (column == columns.end()) {
                    throw std::invalid_argument("CSV header names a column the table does not have: " + fields[index]);
                }
                mapping.push_back({static_cast<std::size_t>(column - columns.begin()), index});
            }
            return;
        }
        if (options.import_row_filter && !options.import_row_filter(fields)) {
            return;
        }
        if (fields.size() != mapping.size() && !options.header) {
            throw std::invalid_argument("CSV record has " + std::to_string(fields.size()) +
                                        " fields but the table has " + std::to_string(mapping.size()) +
                                        " columns");
        }
        std::vector<Value> row(columns.size(), Value{});
        for (const auto& entry : mapping) {
            if (entry.field >= fields.size()) {
                continue;
            }
            row[entry.column] = field_value(table, columns[entry.column], fields[entry.field]);
        }
        rows.push_back(std::move(row));
        if (rows.size() >= import_chunk_rows) {
            flush();
        }
    });
    flush();
    return imported;
}

std::size_t import_table_csv(Table& table, const std::filesystem::path& path, const CsvOptions& options) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw DatabaseError(DatabaseErrorCode::io_error, "Cannot open the CSV file for reading", path);
    }
    return import_table_csv(table, input, options);
}

}

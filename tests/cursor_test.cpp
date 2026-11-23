#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::DataType;
using rea_jackcess::ColumnMatcher;
using rea_jackcess::BlobValue;
using rea_jackcess::DecimalValue;
using rea_jackcess::IndexBuilder;
using rea_jackcess::Row;
using rea_jackcess::RowCursor;
using rea_jackcess::RowFilter;
using rea_jackcess::RowId;
using rea_jackcess::RowPattern;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

constexpr int row_count = 100;

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
                ("rea-jackcess-cursor-" + std::to_string(stamp));
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

Table find_table(const Database& database, const std::string& name) {
    for (auto table : database.tables()) {
        if (table.is_user_table() && table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("missing table " + name);
}

std::int32_t key_of(const Row& row) {
    const auto* key = std::get_if<std::int32_t>(&row.at(0));
    if (key == nullptr) {
        throw std::runtime_error("row does not start with an int32 key");
    }
    return *key;
}

std::vector<std::int32_t> collect(RowCursor cursor) {
    std::vector<std::int32_t> keys;
    while (auto row = cursor.next()) {
        keys.push_back(key_of(*row));
    }
    return keys;
}

void create_fixture(const std::filesystem::path& path, DatabaseFormat format) {
    Database::create(path, format,
                     {TableBuilder("Items")
                          .add_column("Key", DataType::int32)
                          .add_column("Label", DataType::text)
                          .add_index(IndexBuilder("ByKey", "Key"))
                          .add_index(IndexBuilder("ByLabel", "Label", true))
                          .add_index(IndexBuilder("ByLabelScan", "Label"))});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Items");
    std::vector<std::vector<Value>> rows;
    rows.reserve(row_count);
    for (int key = 1; key <= row_count; ++key) {
        rows.push_back({std::int32_t{key}, std::string("label-") + std::to_string(key)});
    }
    table.add_rows(rows);
}

void test_forward_and_backward(DatabaseFormat format, const std::filesystem::path& path) {
    create_fixture(path, format);
    auto database = Database::open(path);
    auto table = find_table(database, "Items");

    auto cursor = table.rows();
    check(!cursor.position().has_value(), "a fresh cursor should have no position");
    const auto first = cursor.next();
    check(first && key_of(*first) == 1, "the first row should have key 1");
    check(cursor.position() == first->id(), "position() should report the last returned row");

    const auto second = cursor.next();
    check(second && key_of(*second) == 2, "the second row should have key 2");

    const auto back = cursor.previous();
    check(back && key_of(*back) == 1, "previous() should return the first row again");
    check(!cursor.previous().has_value(), "previous() at the first row should stop");

    const auto current = cursor.current();
    check(current && key_of(*current) == 1, "current() should re-read the positioned row");

    // Walk to the end, then step back from the exhausted end.
    while (cursor.next()) {
    }
    check(!cursor.next().has_value(), "next() past the end should keep returning nothing");
    const auto last = cursor.previous();
    check(last && key_of(*last) == row_count, "previous() at the end should return the last row");
}

void test_row_pattern_matching(DatabaseFormat format, const std::filesystem::path& path) {
    create_fixture(path, format);
    auto database = Database::open(path);
    auto table = find_table(database, "Items");

    auto exact = table.rows();
    check(!exact.current_row_matches({{"Key", std::int32_t{1}}}),
          "a cursor without a current row should not match a pattern");
    const auto sixth = exact.find_first_row({{"kEy", std::int32_t{6}}});
    check(sixth && key_of(*sixth) == 6 && exact.current_row_matches({{"Label", std::string("label-6")}}),
          "default row-pattern matching did not find or inspect the expected row");
    auto coerced_number = table.rows();
    const auto short_match = coerced_number.find_first_row({{"Key", std::int16_t{6}}});
    check(short_match && key_of(*short_match) == 6,
          "default matching did not coerce a short value to the column type");
    const auto string_match = coerced_number.find_first_row({{"Key", std::string("7")}});
    check(string_match && key_of(*string_match) == 7,
          "default matching did not parse a numeric string for a numeric column");

    auto unique_text_index = table.rows_by_index("ByLabel");
    const auto unique_text_match = unique_text_index.find_first_row({{"Label", std::string("LABEL-8")}});
    check(unique_text_match && key_of(*unique_text_match) == 8,
          "a unique text index did not default to case-insensitive matching");
    unique_text_index.set_column_matcher([](std::string_view, std::string_view,
                                            const Value&, const Value&) { return false; });
    check(!unique_text_index.find_first_row({{"Label", std::string("label-8")}}),
          "a custom matcher did not override the unique-index default");
    unique_text_index.set_column_matcher({});
    check(unique_text_index.find_first_row({{"Label", std::string("LABEL-8")}}).has_value(),
          "clearing a custom matcher did not restore the unique-index default");
    auto nonunique_text_index = table.rows_by_index("ByLabelScan");
    check(!nonunique_text_index.find_first_row({{"Label", std::string("LABEL-8")}}),
          "a non-unique text index incorrectly defaulted to case-insensitive matching");

    auto case_insensitive = table.rows();
    const ColumnMatcher insensitive_matcher = [](std::string_view table_name,
                                                  std::string_view column_name,
                                                  const Value& expected,
                                                  const Value& actual) {
        check(table_name == "Items" && column_name == "Label",
              "column matcher received incorrect table or column metadata");
        const auto* expected_text = std::get_if<std::string>(&expected);
        const auto* actual_text = std::get_if<std::string>(&actual);
        if (!expected_text || !actual_text) {
            return false;
        }
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        };
        return expected_text->size() == actual_text->size() &&
               std::equal(expected_text->begin(), expected_text->end(), actual_text->begin(),
                          [&](char left, char right) { return lower(left) == lower(right); });
    };
    case_insensitive.set_column_matcher(insensitive_matcher);
    const auto first_match = case_insensitive.find_first_row({{"Label", std::string("LABEL-6")}});
    check(first_match && key_of(*first_match) == 6 &&
              case_insensitive.current_row_matches({{"Label", std::string("label-6")}}),
          "custom column matcher did not handle a case-insensitive match");
    const auto next_match = case_insensitive.find_next_row({{"Label", std::string("LABEL-7")}});
    check(next_match && key_of(*next_match) == 7,
          "find_next_row did not continue after the current row");
    const auto saved_position = case_insensitive.position();
    check(!case_insensitive.find_next_row({{"Label", std::string("LABEL-999")}}) &&
              case_insensitive.position() == saved_position,
          "a failed row-pattern search did not restore the cursor position");
    const auto reset_match = case_insensitive.find_first_row({{"Label", std::string("LABEL-3")}});
    check(reset_match && key_of(*reset_match) == 3,
          "find_first_row did not restart at the beginning of the cursor scan");

    auto indexed = table.rows_by_index("ByKey");
    indexed.set_column_matcher(insensitive_matcher);
    const auto indexed_match = indexed.find_first_row({{"Label", std::string("LABEL-8")}});
    check(indexed_match && key_of(*indexed_match) == 8,
          "row-pattern matching did not follow the index cursor scan");

    case_insensitive.set_column_matcher({});
    const auto position_before_miss = case_insensitive.position();
    check(!case_insensitive.find_first_row({{"Label", std::string("LABEL-3")}}) &&
              case_insensitive.position() == position_before_miss,
          "clearing a column matcher did not restore exact-value matching and preserve position");

    bool duplicate_column = false;
    try {
        (void)exact.find_first_row({{"Key", std::int32_t{6}}, {"key", std::int32_t{7}}});
    } catch (const std::invalid_argument&) {
        duplicate_column = true;
    }
    check(duplicate_column, "a row pattern with duplicate case-insensitive columns was accepted");
}

void test_default_matcher_coercion(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format,
                     {TableBuilder("Values")
                          .add_column("Id", DataType::int32)
                          .add_column("Enabled", DataType::boolean)
                          .add_column("Amount", DataType::money)
                          .add_column("OccurredAt", DataType::short_date_time)
                          .add_column("Floating", DataType::float64)});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Values");
    table.add_rows({{std::int32_t{1}, true, DecimalValue{"12.0000"},
                     rea_jackcess::DateTimeValue{"2024-02-29T12:34:56.123"}, 13.5},
                    {std::int32_t{2}, false, DecimalValue{"12.0000"},
                     rea_jackcess::DateTimeValue{"2024-02-28T12:34:56.123"},
                     std::numeric_limits<double>::infinity()},
                    {std::int32_t{3}, false, DecimalValue{"12.0000"},
                     rea_jackcess::DateTimeValue{"2024-02-27T12:34:56.123"},
                     std::numeric_limits<double>::quiet_NaN()}});

    auto cursor = table.rows();
    const auto numeric_boolean = cursor.find_first_row({{"Enabled", std::int16_t{-1}}});
    check(numeric_boolean && key_of(*numeric_boolean) == 1,
          "default matching did not coerce an integer to a Boolean column");

    const auto boolean_string = cursor.find_first_row({{"Enabled", std::string("1")}});
    check(boolean_string && key_of(*boolean_string) == 2,
          "Boolean matching treated a numeric string as a number instead of parsing Boolean text");

    const auto decimal_string = cursor.find_first_row({{"Amount", std::string("12.0000")}});
    check(decimal_string && key_of(*decimal_string) == 1,
          "default matching did not coerce a decimal string to a Money column");
    const auto position_before_scale_miss = cursor.position();
    check(!cursor.find_first_row({{"Amount", std::int32_t{12}}}) &&
              cursor.position() == position_before_scale_miss,
          "Money matching ignored BigDecimal scale or changed position after a miss");

    const auto date_match = cursor.find_first_row(
        {{"OccurredAt", std::string("2024-02-29 12:34:56.123")}});
    check(date_match && key_of(*date_match) == 1,
          "default matching did not coerce an ISO date string to SHORT_DATE_TIME");
    const auto position_before_invalid_date = cursor.position();
    check(!cursor.find_first_row({{"OccurredAt", std::string("2024-02-30T12:34:56.123")}}) &&
              cursor.position() == position_before_invalid_date,
          "an invalid date matched or changed cursor position after a miss");

    const auto suffixed_float = cursor.find_first_row({{"Floating", std::string("13.5d")}});
    check(suffixed_float && key_of(*suffixed_float) == 1,
          "default matching did not accept Java floating-point suffixes");
    const auto upper_suffix_float = cursor.find_first_row({{"Floating", std::string("13.5F")}});
    check(upper_suffix_float && key_of(*upper_suffix_float) == 1,
          "default matching did not accept uppercase Java floating-point suffixes");
    const auto spaced_float = cursor.find_first_row({{"Floating", std::string(" \t13.5d\n")}});
    check(spaced_float && key_of(*spaced_float) == 1,
          "default matching did not trim Java-compatible surrounding whitespace");
    const auto hex_float = cursor.find_first_row({{"Floating", std::string("0x1.bp3")}});
    check(hex_float && key_of(*hex_float) == 1,
          "default matching did not accept Java hexadecimal floating-point strings");
    const auto infinity = cursor.find_first_row({{"Floating", std::string("Infinity")}});
    check(infinity && key_of(*infinity) == 2,
          "default matching did not accept Java Infinity text");
    bool rejected_short_infinity = false;
    try {
        (void)cursor.find_first_row({{"Floating", std::string("inf")}});
    } catch (const std::invalid_argument&) {
        rejected_short_infinity = true;
    }
    check(rejected_short_infinity,
          "default matching did not reject the non-Java 'inf' spelling");
    const auto nan = cursor.find_first_row({{"Floating", std::string("NaN")}});
    check(nan && key_of(*nan) == 3,
          "default matching did not accept Java NaN text");
    bool rejected_suffixed_nan = false;
    try {
        (void)cursor.find_first_row({{"Floating", std::string("NaNd")}});
    } catch (const std::invalid_argument&) {
        rejected_suffixed_nan = true;
    }
    check(rejected_suffixed_nan,
          "default matching did not reject a suffix on Java NaN text");
}

void test_binary_default_matcher(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format,
                     {TableBuilder("BinaryValues")
                          .add_column("Id", DataType::int32)
                          .add_column("Payload", DataType::binary)});
    const BlobValue serialized_hello{{0xac, 0xed, 0x00, 0x05, 0x74, 0x00, 0x05,
                                      0x68, 0x65, 0x6c, 0x6c, 0x6f}};
    const BlobValue serialized_unicode{{0xac, 0xed, 0x00, 0x05, 0x74, 0x00, 0x09,
                                        0x41, 0xc0, 0x80, 0xed, 0xa0, 0xbd,
                                        0xed, 0xb8, 0x80}};
    const BlobValue raw_bytes{{0x00, 0xff, 0x10}};
    const std::string unicode_text = std::string("A\0", 2) + u8"\U0001f600";
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "BinaryValues");
        table.add_rows({{std::int32_t{1}, serialized_hello},
                        {std::int32_t{2}, serialized_unicode},
                        {std::int32_t{3}, raw_bytes}});
    }

    auto database = Database::open(path);
    auto table = find_table(database, "BinaryValues");
    auto cursor = table.rows();
    const auto hello = cursor.find_first_row({{"Payload", std::string("hello")}});
    check(hello && key_of(*hello) == 1,
          "default matching did not serialize a string as Java binary column bytes");
    const auto unicode = cursor.find_first_row({{"Payload", unicode_text}});
    check(unicode && key_of(*unicode) == 2,
          "binary string matching did not use Java modified UTF-8 for NUL and supplementary text");
    const auto exact = cursor.find_first_row({{"Payload", raw_bytes}});
    check(exact && key_of(*exact) == 3,
          "default matching did not preserve exact binary value equality");
    check(!cursor.find_first_row({{"Payload", std::string("missing")}}),
          "a non-matching serialized string matched a binary value");
}

void test_unicode_unique_text_matching(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format,
                     {TableBuilder("Accent")
                          .add_column("Id", DataType::int32)
                          .add_column("Text", DataType::text)
                          .add_index(IndexBuilder("ByValue", "Text", true)),
                      TableBuilder("SharpS")
                          .add_column("Id", DataType::int32)
                          .add_column("Text", DataType::text)
                          .add_index(IndexBuilder("ByValue", "Text", true)),
                      TableBuilder("Greek")
                          .add_column("Id", DataType::int32)
                          .add_column("Text", DataType::text)
                          .add_index(IndexBuilder("ByValue", "Text", true)),
                      TableBuilder("Turkish")
                          .add_column("Id", DataType::int32)
                          .add_column("Text", DataType::text)
                          .add_index(IndexBuilder("ByValue", "Text", true)),
                      TableBuilder("Supplementary")
                          .add_column("Id", DataType::int32)
                          .add_column("Text", DataType::text)
                          .add_index(IndexBuilder("ByValue", "Text", true))});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        find_table(database, "Accent").add_rows({{std::int32_t{1}, std::string(u8"café")}});
        find_table(database, "SharpS").add_rows({{std::int32_t{1}, std::string(u8"straße")}});
        find_table(database, "Greek").add_rows({{std::int32_t{1}, std::string(u8"ς")}});
        find_table(database, "Turkish").add_rows({{std::int32_t{1}, std::string(u8"ı")}});
        find_table(database, "Supplementary").add_rows(
            {{std::int32_t{1}, std::string(u8"\U00010428")}});
    }

    auto database = Database::open(path);
    const auto expect_match = [&](std::string_view table_name,
                                  std::string expected,
                                  bool should_match) {
        auto cursor = find_table(database, std::string(table_name)).rows_by_index("ByValue");
        const auto match = cursor.find_first_row({{"Text", std::move(expected)}});
        check(match.has_value() == should_match,
              "the unique text-index matcher disagreed with Java Unicode case matching");
        if (match) {
            check(key_of(*match) == 1, "Unicode text matching returned the wrong row");
        }
    };
    expect_match("Accent", std::string(u8"CAFÉ"), true);
    expect_match("SharpS", std::string(u8"STRAẞE"), true);
    expect_match("SharpS", std::string(u8"STRASSE"), false);
    expect_match("Greek", std::string(u8"Σ"), true);
    expect_match("Turkish", std::string("I"), true);
    expect_match("Supplementary", std::string(u8"\U00010400"), true);

    std::string invalid_utf8(1, static_cast<char>(0xff));
    expect_match("Accent", std::move(invalid_utf8), false);
}

void test_jump_and_savepoint(DatabaseFormat format, const std::filesystem::path& path) {
    create_fixture(path, format);
    auto database = Database::open(path);
    auto table = find_table(database, "Items");

    auto cursor = table.rows();
    const auto third = cursor.next();
    const auto fourth = cursor.next();
    const auto fifth = cursor.next();
    check(third && fourth && fifth && key_of(*fifth) == 3, "fixture iteration is unexpected");
    const RowId savepoint = *cursor.position();

    while (cursor.next()) {
    }
    const auto restored = cursor.move_to(savepoint);
    check(restored && key_of(*restored) == 3, "move_to() should return the saved row");
    const auto after = cursor.next();
    check(after && key_of(*after) == 4, "next() after move_to() should continue from the savepoint");

    auto other = table.rows();
    const auto first_row = other.next();
    check(first_row && key_of(*first_row) == 1, "the second cursor did not start at the first row");

    const auto last = other.last();
    check(last && key_of(*last) == row_count, "last() should return the final row");
    const auto first_again = other.first();
    check(first_again && key_of(*first_again) == 1, "first() should return the first row");
}

void test_projection_and_filter(DatabaseFormat format, const std::filesystem::path& path) {
    create_fixture(path, format);
    auto database = Database::open(path);
    auto table = find_table(database, "Items");

    auto projected = table.rows({"label"});
    const auto row = projected.next();
    check(row && row->size() == 1, "the projection should decode one column");
    const auto* label = std::get_if<std::string>(&row->at(0));
    check(label != nullptr && *label == "label-1",
          "the projection should return the requested column in order");

    RowFilter every_tenth = [](const Row& candidate) {
        return key_of(candidate) % 10 == 0;
    };
    const auto filtered = collect(table.rows({"Key"}, every_tenth));
    check(filtered.size() == 10, "the filter should keep ten rows");
    for (std::size_t index = 0; index < filtered.size(); ++index) {
        check(filtered[index] == static_cast<std::int32_t>((index + 1) * 10),
              "the filtered scan order is wrong");
    }

    // Navigation follows the filtered sequence, not the table.
    auto navigable = table.rows({"Key"}, every_tenth);
    const auto filtered_last = navigable.last();
    check(filtered_last && key_of(*filtered_last) == 100, "last() should honour the filter");
    const auto filtered_previous = navigable.previous();
    check(filtered_previous && key_of(*filtered_previous) == 90,
          "previous() should step through the filtered rows");

    auto hidden = table.rows({"Key"}, [](const Row& candidate) {
        return key_of(candidate) == 1;
    });
    const auto only = hidden.next();
    check(only && key_of(*only) == 1, "a narrow filter should still return its row");
    const auto target = only->id();
    auto unfiltered = table.rows();
    check(unfiltered.move_to(target).has_value(), "a row id should resolve in an unfiltered cursor");
}

void test_index_cursor_navigation(DatabaseFormat format, const std::filesystem::path& path) {
    create_fixture(path, format);
    auto database = Database::open(path);
    auto table = find_table(database, "Items");

    auto cursor = table.rows_by_index("ByKey", {"Key"}, [](const Row& candidate) {
        return key_of(candidate) <= 5;
    });
    check(key_of(*cursor.next()) == 1, "the index scan should start at the smallest key");
    check(key_of(*cursor.last()) == 5, "last() should stop at the end of the filtered index scan");
    check(key_of(*cursor.previous()) == 4, "previous() should walk back through the index scan");

    auto reverse = table.rows_by_index_reverse("ByKey");
    check(key_of(*reverse.next()) == row_count, "a reverse index scan should start at the largest key");
    check(key_of(*reverse.next()) == row_count - 1, "a reverse index scan should descend");
    check(key_of(*reverse.previous()) == row_count,
          "reverse scans should still support backwards movement");
}

void test_rejections(DatabaseFormat format, const std::filesystem::path& path) {
    create_fixture(path, format);
    auto database = Database::open(path);
    auto table = find_table(database, "Items");

    bool unknown_column = false;
    try {
        auto cursor = table.rows({"Missing"});
        (void)cursor;
    } catch (const std::invalid_argument&) {
        unknown_column = true;
    }
    check(unknown_column, "an unknown projection column should be an invalid_argument");

    bool empty_projection = false;
    try {
        auto cursor = table.rows({});
        (void)cursor;
    } catch (const std::invalid_argument&) {
        empty_projection = true;
    }
    check(empty_projection, "an empty projection should be an invalid_argument");

    auto other = table.rows();
    check(!other.move_to(RowId{1000000, 0}).has_value(), "move_to() should reject an unknown row id");
}

int parse_format(const std::string& text) { return text == "v2003" ? 1 : 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: cursor_test <v2000|v2003>\n";
        return 2;
    }
    TemporaryDirectory directory;
    const auto format = parse_format(argv[1]) == 1 ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    try {
        test_forward_and_backward(format, directory.file("walk.mdb"));
        test_row_pattern_matching(format, directory.file("match.mdb"));
        test_default_matcher_coercion(format, directory.file("matcher-coercion.mdb"));
        test_binary_default_matcher(format, directory.file("binary-matcher.mdb"));
        test_unicode_unique_text_matching(format, directory.file("unicode-matcher.mdb"));
        test_jump_and_savepoint(format, directory.file("jump.mdb"));
        test_projection_and_filter(format, directory.file("project.mdb"));
        test_index_cursor_navigation(format, directory.file("index.mdb"));
        test_rejections(format, directory.file("reject.mdb"));
        std::cout << "cursor navigation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "cursor navigation tests failed: " << error.what() << '\n';
        return 1;
    }
}

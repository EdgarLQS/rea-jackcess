#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::ComplexColumnType;
using rea_jackcess::DataType;
using rea_jackcess::EngineVersion;
using rea_jackcess::Row;
using rea_jackcess::Table;
using rea_jackcess::Value;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const Value& value_for_column(const Table& table, const Row& row, std::string_view column_name) {
    const auto& columns = table.columns();
    const auto column = std::find_if(columns.begin(), columns.end(), [&](const auto& candidate) {
        return candidate.name == column_name;
    });
    check(column != columns.end(), "Fixture table is missing column " + std::string(column_name));
    return row.at(static_cast<std::size_t>(column - columns.begin()));
}

void check_read_only_ace(const std::filesystem::path& path, EngineVersion expected_version) {
    auto database = Database::open(path);
    check(database.format().version == expected_version, "ACE engine version did not match the fixture");

    const auto tables = database.tables();
    check(tables.size() >= 1, "ACE database did not enumerate its table schema");
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Table1";
    });
    check(table != tables.end() && table->is_user_table(), "ACE fixture is missing its user table");
    check(table->row_count_header() == 4, "ACE table row count did not match the fixture");

    const auto& columns = table->columns();
    check(columns.size() == 6, "ACE complex-column fixture schema has an unexpected column count");
    check(columns[1].name.rfind("VersionHistory_", 0) == 0 &&
              columns[1].type == DataType::complex && columns[1].raw_type_code == 0x12,
          "ACE version-history column was not retained in the schema");
    check(columns[1].complex_type_id == 2 && !columns[1].complex_value_foreign_key,
          "ACE version-history complex id was not read from the column definition");
    check(columns[4].name == "multi-value-data" && columns[4].type == DataType::complex &&
              columns[4].raw_type_code == 0x12,
          "ACE multi-value column was not retained in the schema");
    check(columns[4].complex_type_id == 3 && !columns[4].complex_value_foreign_key,
          "ACE multi-value complex id was not read from the column definition");
    check(columns[5].name == "attach-data" && columns[5].type == DataType::complex &&
              columns[5].raw_type_code == 0x12,
          "ACE attachment column was not retained in the schema");
    check(columns[5].complex_type_id == 4 && !columns[5].complex_value_foreign_key,
          "ACE attachment complex id was not read from the column definition");

    for (const std::size_t column_index : {1u, 4u, 5u}) {
        const auto& column = columns[column_index];
        check(column.complex_column_reference.has_value(),
              "ACE ComplexID did not resolve through MSysComplexColumns");
        const auto& reference = *column.complex_column_reference;
        check(reference.conceptual_table_page_number > 0 &&
                  reference.complex_type_table_page_number > 0 &&
                  reference.complex_type_table_name.has_value() &&
                  reference.flat_table_page_number > 0 && reference.flat_table_name.has_value() &&
                  reference.primary_key_column_name.has_value() &&
                  reference.foreign_key_column_name.has_value(),
              "ACE complex-column supporting table/key relationship is incomplete");
        check(*reference.primary_key_column_name != *reference.foreign_key_column_name,
              "ACE complex flat-table primary and foreign keys were not distinguished");
        const auto flat_table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
            return candidate.name() == *reference.flat_table_name;
        });
        check(flat_table != tables.end(), "ACE complex flat table was not found in the catalog");
        const auto primary_key = std::find_if(flat_table->columns().begin(), flat_table->columns().end(),
                                              [&](const auto& candidate) {
            return candidate.name == *reference.primary_key_column_name;
        });
        check(primary_key != flat_table->columns().end(),
              "ACE complex flat-table primary key name did not resolve to a column");
        const auto foreign_key = std::find_if(flat_table->columns().begin(), flat_table->columns().end(),
                                              [&](const auto& candidate) {
            return candidate.name == *reference.foreign_key_column_name &&
                   candidate.complex_value_foreign_key;
        });
        check(foreign_key != flat_table->columns().end(),
              "ACE complex flat-table reference does not identify its flagged foreign key");
    }
    check(columns[1].complex_column_reference->complex_type_table_name->rfind("MSysComplexTypeVH_", 0) == 0,
          "ACE version-history column did not resolve its version-history type table");
    check(columns[1].complex_column_reference->type == ComplexColumnType::version_history,
          "ACE version-history type table was not classified");
    check(columns[4].complex_column_reference->complex_type_table_name->rfind("MSysComplexType_", 0) == 0 &&
              *columns[4].complex_column_reference->complex_type_table_name != "MSysComplexType_Attachment",
          "ACE multi-value column did not resolve its multi-value type table");
    check(columns[4].complex_column_reference->type == ComplexColumnType::multi_value,
          "ACE single-value type table was not classified as multi-value");
    check(*columns[5].complex_column_reference->complex_type_table_name == "MSysComplexType_Attachment",
          "ACE attachment column did not resolve the attachment type table");
    check(columns[5].complex_column_reference->type == ComplexColumnType::attachment,
          "ACE attachment type table was not classified");

    std::size_t flagged_complex_foreign_keys = 0;
    for (const auto& candidate : database.tables()) {
        for (const auto& column : candidate.columns()) {
            flagged_complex_foreign_keys += column.complex_value_foreign_key ? 1 : 0;
        }
    }
    check(flagged_complex_foreign_keys >= 3,
          "ACE flat-table complex foreign-key column flags were not exposed");

    const std::array<std::size_t, 3> complex_column_indexes{1, 4, 5};
    std::array<std::size_t, 3> complex_value_totals{};
    std::size_t rows = 0;
    auto cursor = table->rows();
    while (auto row = cursor.next()) {
        ++rows;
        const auto* row_name = std::get_if<std::string>(&value_for_column(*table, *row, "id"));
        check(row_name != nullptr, "ACE fixture row id was not decoded as text");
        for (std::size_t index = 0; index < complex_column_indexes.size(); ++index) {
            const auto& column = columns[complex_column_indexes[index]];
            auto raw_values = table->raw_complex_values(column.name, row->id());
            complex_value_totals[index] += raw_values.size();

            if (index == 0 && *row_name == "row3") {
                const auto& reference = *column.complex_column_reference;
                const auto type_table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
                    return candidate.name() == *reference.complex_type_table_name;
                });
                const auto flat_table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
                    return candidate.name() == *reference.flat_table_name;
                });
                check(type_table != tables.end() && flat_table != tables.end(),
                      "ACE version-history supporting tables were not found");
                const auto value_column = std::find_if(type_table->columns().begin(), type_table->columns().end(),
                                                      [](const auto& candidate) {
                    return candidate.type == DataType::memo;
                });
                check(value_column != type_table->columns().end(),
                      "ACE version-history type table has no memo value column");
                std::array<bool, 3> expected_versions{};
                for (const auto& raw_value : raw_values) {
                    const auto& value = value_for_column(*flat_table, raw_value, value_column->name);
                    const auto* memo = std::get_if<rea_jackcess::MemoValue>(&value);
                    check(memo != nullptr, "ACE version-history raw value was not decoded as Memo");
                    expected_versions[0] |= memo->text == "row3-memo-again";
                    expected_versions[1] |= memo->text == "row3-memo-revised";
                    expected_versions[2] |= memo->text == "row3-memo";
                }
                check(expected_versions == std::array<bool, 3>{true, true, true},
                      "ACE version-history memo values did not match the fixed Jackcess fixture");
                const auto typed_values = table->complex_values(column.name, row->id());
                check(typed_values.size() == 3,
                      "ACE typed version-history values did not match the fixed Jackcess fixture");
                std::vector<std::string> typed_versions;
                for (const auto& typed_value : typed_values) {
                    const auto* version = std::get_if<rea_jackcess::VersionHistoryValue>(&typed_value.value);
                    check(version != nullptr && std::holds_alternative<rea_jackcess::DateTimeValue>(version->modified_at),
                          "ACE version-history value did not decode its typed fields");
                    check(version->value.has_value(), "ACE version-history text was unexpectedly NULL");
                    typed_versions.push_back(*version->value);
                }
                check(typed_versions == std::vector<std::string>{
                          "row3-memo-again", "row3-memo-revised", "row3-memo"},
                      "ACE typed versions were not returned newest first");
            } else if (index == 1 && *row_name == "row3") {
                const auto& reference = *column.complex_column_reference;
                const auto type_table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
                    return candidate.name() == *reference.complex_type_table_name;
                });
                const auto flat_table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
                    return candidate.name() == *reference.flat_table_name;
                });
                check(type_table != tables.end() && flat_table != tables.end() &&
                          type_table->columns().size() == 1,
                      "ACE multi-value supporting table schema was unexpected");
                std::vector<std::string> values;
                for (const auto& raw_value : raw_values) {
                    const auto& value = value_for_column(*flat_table, raw_value, type_table->columns().front().name);
                    const auto* text = std::get_if<std::string>(&value);
                    check(text != nullptr, "ACE multi-value raw value was not decoded as text");
                    values.push_back(*text);
                }
                check(values == std::vector<std::string>{"value1", "value2", "value3", "value4"},
                      "ACE multi-value text values did not match the fixed Jackcess fixture");
                const auto typed_values = table->complex_values(column.name, row->id());
                std::vector<std::string> typed_text_values;
                for (const auto& typed_value : typed_values) {
                    const auto* item = std::get_if<rea_jackcess::MultiValueItem>(&typed_value.value);
                    check(item != nullptr, "ACE multi-value row did not use the typed value representation");
                    const auto* text = std::get_if<std::string>(&item->value);
                    check(text != nullptr, "ACE multi-value typed item was not decoded as text");
                    typed_text_values.push_back(*text);
                }
                check(typed_text_values == values,
                      "ACE typed multi-values did not preserve the flat-table order");
            } else if (index == 2 && *row_name == "row2") {
                const auto& reference = *column.complex_column_reference;
                const auto flat_table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
                    return candidate.name() == *reference.flat_table_name;
                });
                check(flat_table != tables.end() && raw_values.size() == 2,
                      "ACE attachment raw rows did not match the fixed Jackcess fixture");
                std::array<bool, 2> expected_attachments{};
                for (const auto& raw_value : raw_values) {
                    const auto& name_value = value_for_column(*flat_table, raw_value, "FileName");
                    const auto* name = std::get_if<std::string>(&name_value);
                    check(name != nullptr, "ACE attachment file name was not decoded as text");
                    const auto& data_value = value_for_column(*flat_table, raw_value, "FileData");
                    const auto* data = std::get_if<rea_jackcess::BlobValue>(&data_value);
                    check(data != nullptr, "ACE attachment payload was not decoded as binary");
                    const std::uint32_t encoded_length = data->bytes.size() < 8
                        ? 0
                        : static_cast<std::uint32_t>(data->bytes[4]) |
                              (static_cast<std::uint32_t>(data->bytes[5]) << 8) |
                              (static_cast<std::uint32_t>(data->bytes[6]) << 16) |
                              (static_cast<std::uint32_t>(data->bytes[7]) << 24);
                    const bool compressed_attachment_wrapper = data->bytes.size() > 8 &&
                        data->bytes[0] == 1 && data->bytes[1] == 0 && data->bytes[2] == 0 &&
                        data->bytes[3] == 0 && encoded_length == data->bytes.size() - 8;
                    if (*name == "test_data.txt") {
                        expected_attachments[0] = compressed_attachment_wrapper;
                    } else if (*name == "test_data2.txt") {
                        expected_attachments[1] = compressed_attachment_wrapper;
                    }
                }
                check(expected_attachments == std::array<bool, 2>{true, true},
                      "ACE attachment OLE wrappers did not match the fixed Jackcess fixture format");
                const auto typed_values = table->complex_values(column.name, row->id());
                check(typed_values.size() == 2,
                      "ACE typed attachment count did not match the fixed Jackcess fixture");
                std::array<bool, 2> decoded_attachments{};
                for (const auto& typed_value : typed_values) {
                    const auto* attachment = std::get_if<rea_jackcess::AttachmentValue>(&typed_value.value);
                    check(attachment != nullptr && attachment->file_name.has_value() &&
                              attachment->file_type == std::optional<std::string>{"txt"} &&
                              attachment->encoded_file_data.has_value() && attachment->file_data.has_value(),
                          "ACE typed attachment omitted file metadata or decoded payload");
                    if (*attachment->file_name == "test_data.txt") {
                        decoded_attachments[0] = !attachment->file_data->empty();
                    } else if (*attachment->file_name == "test_data2.txt") {
                        const std::string payload(attachment->file_data->begin(), attachment->file_data->end());
                        decoded_attachments[1] = payload == "this is some more test data for attachment.";
                    }
                }
                check(decoded_attachments == std::array<bool, 2>{true, true},
                      "ACE attachment OLE wrapper did not yield the fixed Jackcess file contents");
            }
        }
    }
    check(rows == 4, "ACE table rows were not readable");
    check(complex_value_totals == std::array<std::size_t, 3>{5, 6, 3},
          "ACE raw complex-value counts did not match the fixed Jackcess fixture results");
}

void check_ace_write_rejected(const std::filesystem::path& path) {
    try {
        (void)Database::open(path, DatabaseOpenMode::read_write);
    } catch (const DatabaseError& error) {
        check(error.code() == DatabaseErrorCode::unsupported_operation,
              "ACE read-write rejection used the wrong error code");
        return;
    }
    throw std::runtime_error("ACE read-write open unexpectedly succeeded");
}

void check_unsupported_complex_type(const std::filesystem::path& path) {
    auto database = Database::open(path);
    check(database.format().version == EngineVersion::ace2007,
          "unsupported-complex fixture did not identify as ACE V2007");
    const auto tables = database.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Test";
    });
    check(table != tables.end(), "unsupported-complex fixture is missing table Test");
    const auto& columns = table->columns();
    const auto column = std::find_if(columns.begin(), columns.end(), [](const auto& candidate) {
        return candidate.name == "UnknownComplex";
    });
    check(column != columns.end() && column->type == DataType::complex &&
              column->complex_column_reference.has_value(),
          "unsupported complex column was not resolved to its supporting tables");
    const auto& reference = *column->complex_column_reference;
    check(reference.type == ComplexColumnType::unsupported,
          "unknown complex type did not use the unsupported classification");
    check(reference.primary_key_column_name.has_value() && reference.foreign_key_column_name.has_value(),
          "unsupported complex flat-table keys were not identified");

    const auto id_column = std::find_if(columns.begin(), columns.end(), [](const auto& candidate) {
        return candidate.name == "ID";
    });
    check(id_column != columns.end(), "unsupported-complex fixture is missing its ID column");
    const auto id_column_index = static_cast<std::size_t>(id_column - columns.begin());

    std::size_t row_count = 0;
    bool retained_unknown_field_bytes = false;
    auto cursor = table->rows();
    while (auto row = cursor.next()) {
        ++row_count;
        const auto* id = std::get_if<std::int32_t>(&row->at(id_column_index));
        check(id != nullptr, "unsupported-complex fixture row id was not decoded");
        const std::size_t expected_values = *id == 1 ? 1 : (*id == 2 ? 2 : 0);
        check(*id >= 1 && *id <= 3, "unsupported-complex fixture had an unexpected ID");
        const auto raw_values = table->raw_complex_values("UnknownComplex", row->id());
        check(raw_values.size() == expected_values,
              "unsupported-complex raw values did not match the fixed Jackcess counts");
        const auto typed_values = table->complex_values("UnknownComplex", row->id());
        check(typed_values.size() == expected_values &&
                  std::all_of(typed_values.begin(), typed_values.end(), [](const auto& value) {
                      return std::holds_alternative<rea_jackcess::UnsupportedComplexValue>(value.value);
                  }),
              "unsupported complex values did not retain their raw typed records");
        for (const auto& typed_value : typed_values) {
            for (const auto& field : typed_value.raw_value.values()) {
                const auto* unsupported = std::get_if<rea_jackcess::UnsupportedValue>(&field);
                retained_unknown_field_bytes |= unsupported != nullptr && !unsupported->raw_bytes.empty();
            }
        }
    }
    check(row_count == 3, "unsupported-complex fixture rows were not readable");
    check(retained_unknown_field_bytes,
          "unsupported complex fields did not preserve their encoded column bytes");
}

void check_ace2019_extended_date(const std::filesystem::path& path) {
    auto database = Database::open(path);
    check(database.format().version == EngineVersion::ace2019,
          "ACE extended-date fixture did not identify as V2019");

    const auto tables = database.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Table1";
    });
    check(table != tables.end() && table->is_user_table(), "ACE V2019 fixture is missing Table1");
    check(table->row_count_header() == 9, "ACE V2019 extended-date row count did not match the fixture");

    const auto& columns = table->columns();
    const auto date_column = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
        return column.name == "DateExt";
    });
    check(date_column != columns.end() && date_column->type == DataType::extended_date_time &&
              date_column->raw_type_code == 0x14,
          "ACE V2019 extended-date schema was not retained");
    const auto calculated_date_column = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
        return column.name == "DateNormalCalc";
    });
    check(calculated_date_column != columns.end() && calculated_date_column->calculated &&
              calculated_date_column->type == DataType::short_date_time,
          "ACE V2019 calculated date schema was not identified");

    const std::array<const char*, 9> expected_dates{
        "2020-06-17T00:00",
        "2021-06-14T00:00",
        "2021-06-14T12:45",
        "2021-06-14T01:45",
        nullptr,
        "2021-06-14T22:45:12.345678900",
        "1765-06-14T12:45",
        "0100-06-14T12:45:00.123456700",
        "1265-06-14T12:45",
    };
    const std::array<const char*, 9> expected_calculated_dates{
        "2020-06-17T00:00",
        "2021-06-14T00:00",
        "2021-06-14T12:45",
        "2021-06-14T01:45",
        nullptr,
        "2021-06-14T22:45:12",
        "1765-06-14T12:45",
        "0100-06-14T12:45",
        "1265-06-14T12:45",
    };
    auto rows = table->rows({"ID", "DateExt", "DateNormalCalc"});
    std::size_t row_count = 0;
    while (auto row = rows.next()) {
        check(row_count < expected_dates.size(), "ACE V2019 fixture returned extra table rows");
        const auto* id = std::get_if<std::int32_t>(&row->at(0));
        check(id != nullptr && *id == static_cast<std::int32_t>(row_count + 1),
              "ACE V2019 extended-date rows were not read in fixture order");
        const auto& value = row->at(1);
        const auto& calculated_value = row->at(2);
        if (expected_dates[row_count] == nullptr) {
            check(std::holds_alternative<std::monostate>(value),
                  "ACE V2019 NULL extended date was not preserved");
        } else {
            const auto* date = std::get_if<rea_jackcess::DateTimeValue>(&value);
            check(date != nullptr && date->iso8601 == expected_dates[row_count],
                  "ACE V2019 extended date did not match the fixed Jackcess value");
        }
        if (expected_calculated_dates[row_count] == nullptr) {
            check(std::holds_alternative<std::monostate>(calculated_value),
                  "ACE V2019 NULL calculated date was not preserved");
        } else {
            const auto* date = std::get_if<rea_jackcess::DateTimeValue>(&calculated_value);
            check(date != nullptr && date->iso8601 == expected_calculated_dates[row_count],
                  "ACE V2019 calculated date did not match the fixed Jackcess value");
        }
        ++row_count;
    }
    check(row_count == expected_dates.size(), "ACE V2019 extended-date rows were incomplete");
}

void check_ace2010_calculated_field_types(const std::filesystem::path& path) {
    auto database = Database::open(path);
    check(database.format().version == EngineVersion::ace2010,
          "ACE calculated-field fixture did not identify as V2010");

    const auto tables = database.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Table1";
    });
    check(table != tables.end() && table->is_user_table(),
          "ACE V2010 calculated-field fixture is missing Table1");
    const auto& columns = table->columns();
    const auto is_rich = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
        return column.name == "IsRich";
    });
    const auto bool_test = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
        return column.name == "BoolTest";
    });
    const auto id_column = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
        return column.name == "ID";
    });
    check(id_column != columns.end(), "ACE V2010 calculated-field fixture is missing ID");
    check(!id_column->calculated, "ACE V2010 ID was incorrectly marked calculated");
    check(id_column->raw_type_code == 0x04, "ACE V2010 ID physical type was changed");
    check(id_column->type == DataType::int32,
          "ACE V2010 ID logical type was unexpectedly changed: " +
              std::to_string(static_cast<int>(id_column->type)));
    check(is_rich != columns.end() && is_rich->calculated && is_rich->type == DataType::boolean &&
              is_rich->raw_type_code == 0x03,
          "ACE V2010 calculated Boolean result type was not resolved separately from its stored type");
    check(bool_test != columns.end() && bool_test->calculated && bool_test->type == DataType::boolean,
          "ACE V2010 second calculated Boolean result type was not resolved");
    const auto calculated_column = [&](std::string_view name, DataType type) {
        const auto found = std::find_if(columns.begin(), columns.end(), [&](const auto& column) {
            return column.name == name;
        });
        check(found != columns.end() && found->calculated && found->type == type,
              "ACE V2010 calculated result type was not resolved for " + std::string(name));
    };
    calculated_column("MonthlySalary", DataType::money);
    calculated_column("AllNames", DataType::memo);
    calculated_column("WeeklySalary", DataType::numeric);
    calculated_column("SalaryTest", DataType::money);
    calculated_column("DecimalTest", DataType::numeric);
    calculated_column("FloatTest", DataType::float32);
    calculated_column("BigNumTest", DataType::numeric);

    const std::array<bool, 4> expected_is_rich{true, false, false, false};
    const std::array<const char*, 4> expected_monthly_salary{"83333.3333", "-0.0833", "0.0000", "8.3333"};
    const std::array<const char*, 4> expected_weekly_salary{
        "19230.7692307692", "-0.0192307692307692", "0", "1.92307692307692",
    };
    const std::array<const char*, 4> expected_salary_test{"1000000.0000", "-1.0000", "0.0000", "100.0000"};
    const std::array<const char*, 4> expected_decimal_test{
        "50.325000", "-36.222200", "0.012300", "102030405060.654321",
    };
    const std::array<float, 4> expected_float_test{2583.2092f, 0.0035889593f, 0.0f, 1.27413e-10f};
    const std::array<const char*, 4> expected_big_num_test{
        "56505085819.424791296572280180",
        "-0.0784734499180612994241100748",
        "0.00000000",
        "0.0000002787019289824216980830",
    };
    const std::array<const char*, 4> expected_last_first{
        "Wayne, Bruce", "Simpson, Bart", "Doe, John", "User, Test",
    };
    const std::array<const char*, 4> expected_all_names{
        "Wayne, Bruce=Wayne, Bruce",
        "Simpson, Bart=Simpson, Bart",
        "Doe, John=Doe, John",
        "User, Test=User, Test",
    };
    const std::array<std::int32_t, 4> expected_name_lengths{12, 13, 9, 10};
    auto rows = table->rows({"ID", "LastFirst", "LastFirstLen", "MonthlySalary", "IsRich", "AllNames",
                             "WeeklySalary", "SalaryTest", "BoolTest", "DecimalTest", "FloatTest", "BigNumTest"});
    std::size_t row_count = 0;
    while (auto row = rows.next()) {
        check(row_count < expected_is_rich.size(), "ACE V2010 calculated-field table returned extra rows");
        const auto* id = std::get_if<std::int32_t>(&row->at(0));
        const auto* name = std::get_if<std::string>(&row->at(1));
        const auto* name_length = std::get_if<std::int32_t>(&row->at(2));
        const auto* monthly_salary = std::get_if<rea_jackcess::DecimalValue>(&row->at(3));
        const auto* rich = std::get_if<bool>(&row->at(4));
        const auto* all_names = std::get_if<rea_jackcess::MemoValue>(&row->at(5));
        const auto* weekly_salary = std::get_if<rea_jackcess::DecimalValue>(&row->at(6));
        const auto* salary_test = std::get_if<rea_jackcess::DecimalValue>(&row->at(7));
        const auto* bool_value = std::get_if<bool>(&row->at(8));
        const auto* decimal_test = std::get_if<rea_jackcess::DecimalValue>(&row->at(9));
        const auto* float_test = std::get_if<float>(&row->at(10));
        const auto* big_num_test = std::get_if<rea_jackcess::DecimalValue>(&row->at(11));
        if (id == nullptr) {
            const auto* unsupported = std::get_if<rea_jackcess::UnsupportedValue>(&row->at(0));
            const std::string details = unsupported == nullptr
                                            ? "variant=" + std::to_string(row->at(0).index())
                                            : unsupported->description;
            check(false, "ACE V2010 calculated row ID did not decode as int32: " + details);
        }
        check(*id == static_cast<std::int32_t>(row_count + 1),
              "ACE V2010 calculated-field row order did not match fixed Jackcess 5.2.0: " +
                  std::to_string(*id));
        check(name != nullptr && *name == expected_last_first[row_count],
              "ACE V2010 calculated text did not match fixed Jackcess 5.2.0");
        check(name_length != nullptr && *name_length == expected_name_lengths[row_count],
              "ACE V2010 calculated integer did not match fixed Jackcess 5.2.0");
        check(monthly_salary != nullptr && monthly_salary->text == expected_monthly_salary[row_count],
              "ACE V2010 calculated MONEY did not match fixed Jackcess 5.2.0");
        check(rich != nullptr && *rich == expected_is_rich[row_count],
              "ACE V2010 calculated Boolean did not match fixed Jackcess 5.2.0");
        check(all_names != nullptr && all_names->text == expected_all_names[row_count],
              "ACE V2010 calculated MEMO did not match fixed Jackcess 5.2.0 at row " +
                  std::to_string(row_count + 1) + ": " +
                  (all_names == nullptr ? "<wrong value type>" : all_names->text));
        check(weekly_salary != nullptr && weekly_salary->text == expected_weekly_salary[row_count],
              "ACE V2010 calculated NUMERIC did not match fixed Jackcess 5.2.0");
        check(salary_test != nullptr && salary_test->text == expected_salary_test[row_count],
              "ACE V2010 second calculated MONEY did not match fixed Jackcess 5.2.0");
        check(bool_value != nullptr && *bool_value,
              "ACE V2010 second calculated Boolean did not match fixed Jackcess 5.2.0");
        check(decimal_test != nullptr && decimal_test->text == expected_decimal_test[row_count],
              "ACE V2010 calculated decimal did not match fixed Jackcess 5.2.0");
        check(float_test != nullptr && std::abs(*float_test - expected_float_test[row_count]) <= 1e-6f,
              "ACE V2010 calculated FLOAT did not match fixed Jackcess 5.2.0");
        check(big_num_test != nullptr && big_num_test->text == expected_big_num_test[row_count],
              "ACE V2010 calculated big NUMERIC did not match fixed Jackcess 5.2.0");
        ++row_count;
    }
    check(row_count == expected_is_rich.size(), "ACE V2010 calculated-field rows were incomplete");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: ace_read_test <database.accdb> <ace2007|ace2010|ace2010_calc|ace2019|unsupported>\n";
        return 2;
    }
    const std::string version(argv[2]);
    try {
        if (version == "unsupported") {
            check_unsupported_complex_type(argv[1]);
        } else if (version == "ace2019") {
            check_ace2019_extended_date(argv[1]);
        } else if (version == "ace2010_calc") {
            check_ace2010_calculated_field_types(argv[1]);
        } else {
            const EngineVersion expected = version == "ace2007" ? EngineVersion::ace2007 : EngineVersion::ace2010;
            check_read_only_ace(argv[1], expected);
        }
        check_ace_write_rejected(argv[1]);
        std::cout << version << " read-only ACE tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ACE read tests failed: " << error.what() << '\n';
        return 1;
    }
}

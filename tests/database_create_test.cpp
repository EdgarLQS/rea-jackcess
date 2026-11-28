#include "rea_jackcess/database.hpp"

#include "catalog/table_catalog.hpp"
#include "catalog/row_reader.hpp"
#include "catalog/system_catalog.hpp"
#include "catalog/usage_map.hpp"
#include "index/jet4_text_key.hpp"
#include "page/data_page.hpp"
#include "page/page_file.hpp"
#include "page/page_writer.hpp"
#include "write/jet4_index_entry.hpp"
#include "write/jet4_global_usage_map.hpp"
#include "write/jet4_row_writer.hpp"
#include "util/little_endian.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

rea_jackcess::Table get_table(const rea_jackcess::Database& database, const std::string& name);

std::set<std::uint64_t> read_long_value_usage_pages(const std::filesystem::path& path,
                                                    const std::string& table_name,
                                                    const std::string& column_name,
                                                    bool free_space_pages) {
    const auto file = rea_jackcess::detail::PageFile::open(path);
    const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
    const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [&](const auto& candidate) {
        return candidate.name == table_name;
    });
    check(table != catalog.tables.end(), "long-value fixture did not expose its table definition");
    const auto column = std::find_if(table->definition.columns.begin(),
                                     table->definition.columns.end(),
                                     [&](const auto& candidate) {
        return candidate.name == column_name;
    });
    check(column != table->definition.columns.end(),
          "long-value fixture did not expose its column definition");
    const auto& usage_map = free_space_pages ? column->long_value_free_space_pages
                                             : column->long_value_owned_pages;
    check(usage_map.has_value(), "long-value fixture did not expose its column usage map");
    const auto pages = rea_jackcess::detail::read_usage_map_pages(file, *usage_map);
    return {pages.begin(), pages.end()};
}

std::set<std::uint64_t> read_long_value_owned_pages(const std::filesystem::path& path,
                                                    const std::string& table_name,
                                                    const std::string& column_name) {
    return read_long_value_usage_pages(path, table_name, column_name, false);
}

std::set<std::uint64_t> read_long_value_free_pages(const std::filesystem::path& path,
                                                   const std::string& table_name,
                                                   const std::string& column_name) {
    return read_long_value_usage_pages(path, table_name, column_name, true);
}

bool global_page_is_free(const std::filesystem::path& path, std::uint64_t page_number) {
    const auto file = rea_jackcess::detail::PageFile::open(path);
    const auto declaration_page = file.read_page(1);
    const auto declaration = rea_jackcess::detail::parse_data_page(file, 1, declaration_page);
    check(!declaration.rows.empty() && !declaration.rows.front().deleted &&
              !declaration.rows.front().overflow && declaration.rows.front().bytes.size() >= 5,
          "long-value fixture has an invalid global usage map");
    const auto& bytes = declaration.rows.front().bytes;
    std::uint64_t bit = 0;
    std::vector<std::uint8_t> bitmap;
    std::uint64_t bit_offset = 0;
    if (bytes[0] == 0) {
        const auto first_page = rea_jackcess::detail::read_u32(bytes, 1);
        const auto capacity = static_cast<std::uint64_t>(bytes.size() - 5) * 8;
        if (page_number < first_page || page_number - first_page >= capacity) {
            return false;
        }
        bitmap = declaration_page;
        bit = page_number - first_page;
        bit_offset = declaration.rows.front().start_offset + 5;
    } else if (bytes[0] == 1) {
        constexpr std::uint64_t bitmap_offset = 4;
        const auto pages_per_bitmap = (file.format().page_size - bitmap_offset) * 8;
        const auto slot = page_number / pages_per_bitmap;
        if (slot >= (bytes.size() - 1) / 4) {
            return false;
        }
        const auto bitmap_page = rea_jackcess::detail::read_u32(bytes, 1 + slot * 4);
        if (bitmap_page == 0 || bitmap_page >= file.page_count()) {
            return false;
        }
        bitmap = file.read_page(bitmap_page);
        bit = page_number % pages_per_bitmap;
        bit_offset = bitmap_offset;
    } else {
        throw std::runtime_error("long-value fixture has an unsupported global usage map");
    }
    return (bitmap.at(bit_offset + bit / 8) & (1u << (bit % 8))) != 0;
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
    check(reopened.relationships().empty() && reopened.relationships(true).empty(),
          "new empty database unexpectedly has relationships");
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

    const auto verify_version_property = [&](const std::vector<std::uint8_t>& expected) {
        const auto database = rea_jackcess::Database::open(path);
        const auto expected_format = expected[2] == '8' ? rea_jackcess::FileFormat::v2000
                                                        : rea_jackcess::FileFormat::v2003;
        check(database.file_format() == expected_format &&
                  rea_jackcess::DatabaseFile::open(path).file_format() == expected_format,
              "file format did not use the database AccessVersion");
        const auto maps = database.property_maps();
        for (const auto& map : maps) {
            if (map.type_code != 0 || !map.name.empty()) {
                continue;
            }
            for (const auto& property : map.properties) {
                if (property.name == "AccessVersion") {
                    const auto text = std::get_if<std::string>(&property.value);
                    check(property.type_code == 0x0a && property.raw_value == expected && text &&
                              *text == (expected[2] == '8' ? "08.50" : "09.50"),
                          "database AccessVersion property did not preserve its type and bytes");
                    return;
                }
            }
        }
        throw std::runtime_error("database AccessVersion property was missing");
    };
    verify_version_property({'0', 0, '8', 0, '.', 0, '5', 0, '0', 0});

    const auto original_bytes = read_bytes(path);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        auto database = rea_jackcess::Database::create(
            path, static_cast<rea_jackcess::DatabaseFormat>(0xff));
        (void)database;
    });
    check(read_bytes(path) == original_bytes,
          "unsupported creation request modified the existing destination");

    check_empty_database(path, rea_jackcess::DatabaseFormat::v2003, 50);
    verify_version_property({'0', 0, '9', 0, '.', 0, '5', 0, '0', 0});
}

void test_save_database_properties(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("database-properties.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format);
        {
            auto writable = Database::open(path, DatabaseOpenMode::read_write);
            auto maps = writable.property_maps();
            auto default_map = std::find_if(maps.begin(), maps.end(), [](const auto& map) {
                return map.name.empty() && map.type_code == 0;
            });
            check(default_map != maps.end(), "default database property map is missing");
            PropertyInfo number{"CustomNumber", 0x04, 0x02, {}, {}, {}};
            number.set_value(std::int32_t{-42});
            default_map->properties.push_back(std::move(number));
            PropertyInfo text{"CustomText", 0x0c, 0, {}, {}, {}};
            text.set_value(std::string("描述"));
            default_map->properties.push_back(std::move(text));
            writable.save_property_maps(maps);
        }
        auto reopened = Database::open(path);
        const auto maps = reopened.property_maps();
        bool found = false;
        bool found_text = false;
        for (const auto& map : maps) {
            for (const auto& property : map.properties) {
                if (property.name == "CustomNumber") {
                    found = property.flags == 0x02 && std::get<std::int32_t>(property.value) == -42;
                } else if (property.name == "CustomText") {
                    found_text = std::get<std::string>(property.value) == "描述";
                }
            }
        }
        check(found && found_text, "saved database properties did not survive reopen");
        expect_error(DatabaseErrorCode::unsupported_operation, [&] { reopened.save_property_maps(maps); });
    }
}

void test_save_table_properties(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("table-properties.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format, {TableBuilder("Items").add_column("Id", DataType::int32)});
        {
            auto writable = Database::open(path, DatabaseOpenMode::read_write);
            auto tables = writable.tables();
            auto table = std::find_if(tables.begin(), tables.end(), [](const auto& item) {
                return item.name() == "Items";
            });
            check(table != tables.end(), "property fixture table is missing");
            PropertyInfo payload{"LargePayload", 0x0b, 0, {}, {}, {}};
            payload.set_value(BlobValue{std::vector<std::uint8_t>(9000, 0x37)});
            table->save_property_maps({{"Id", 1, {{"Required", 1, 1, {1}, {}, {}}, std::move(payload)}}});
            check(table->columns().front().nullable == false, "Required did not refresh column metadata");
            expect_error(DatabaseErrorCode::unsupported_operation, [&] {
                table->add_row({std::monostate{}});
            });
        }
        {
            const auto reopened = Database::open(path);
            const auto tables = reopened.tables();
            const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& item) {
                return item.name() == "Items";
            });
            check(table != tables.end() && table->columns().front().nullable == false &&
                      table->property_maps().size() == 1,
                  "the saved column property did not survive reopen");
            const auto maps = table->property_maps();
            const auto* payload = maps.front().find("LargePayload");
            check(payload && std::get<BlobValue>(payload->value).bytes ==
                      std::vector<std::uint8_t>(9000, 0x37),
                  "the saved multi-page table property did not survive reopen");
        }
        {
            auto writable = Database::open(path, DatabaseOpenMode::read_write);
            auto tables = writable.tables();
            auto table = std::find_if(tables.begin(), tables.end(), [](const auto& item) {
                return item.name() == "Items";
            });
            auto maps = table->property_maps();
            PropertyInfo value{"DefaultValue", 0x0a, 0, {}, {}, {}};
            value.set_value(std::string("=42"));
            maps.front().put(std::move(value));
            PropertyInfo rule{"ValidationRule", 0x0a, 0, {}, {}, {}};
            rule.set_value(std::string("[iD] >= 0"));
            maps.front().put(std::move(rule));
            PropertyInfo message{"ValidationText", 0x0a, 0, {}, {}, {}};
            message.set_value(std::string("Id must be nonnegative"));
            maps.front().put(std::move(message));
            table->save_property_maps(maps);
            const auto ids = table->add_rows({{}, {std::monostate{}}, {std::int32_t{9}}});
            const auto before = read_bytes(path);
            expect_error(DatabaseErrorCode::constraint_violation, [&] {
                table->add_rows({{}, {std::int32_t{-1}}});
            });
            try {
                table->update_row(ids.front(), {std::int32_t{-1}});
                throw std::runtime_error("validation accepted a negative Id");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "Id must be nonnegative" &&
                          error.context().column_name == "Id",
                      "validation error lost its configured message or column context");
            }
            check(read_bytes(path) == before, "column validation failure modified the database");
            auto rows = table->rows();
            for (const auto expected : {42, 42, 9}) {
                const auto row = rows.next();
                check(row && std::get<std::int32_t>(row->at(0)) == expected,
                      "column defaults did not respect supplied and omitted values");
            }
        }
        {
            auto writable = Database::open(path, DatabaseOpenMode::read_write);
            auto tables = writable.tables();
            auto table = std::find_if(tables.begin(), tables.end(), [](const auto& item) {
                return item.name() == "Items";
            });
            table->add_row({});
            expect_error(DatabaseErrorCode::constraint_violation, [&] {
                table->add_row({std::int32_t{-1}});
            });
            check(table->row_count_header() == 4, "column default was not available after reopening");
        }
    }
}

void test_numeric_column_defaults(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("numeric-column-defaults.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("Values")
                              .add_column("SingleValue", DataType::float32)
                              .add_column("DoubleValue", DataType::float64)
                              .add_column("MoneyValue", DataType::money)});
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            auto table = get_table(database, "Values");
            std::vector<PropertyMapInfo> maps{{"SingleValue", 1, {}},
                                              {"DoubleValue", 1, {}},
                                              {"MoneyValue", 1, {}}};
            const auto set_default = [&](const std::string& column_name, const std::string& expression) {
                auto map = std::find_if(maps.begin(), maps.end(), [&](const auto& candidate) {
                    return candidate.name == column_name;
                });
                check(map != maps.end(), "numeric column property map is missing");
                PropertyInfo value{"DefaultValue", 0x0a, 0, {}, {}, {}};
                value.set_value(expression);
                map->put(std::move(value));
            };
            set_default("SingleValue", "=+1.25");
            set_default("DoubleValue", "=-2.5e1");
            set_default("MoneyValue", "=-12.3456e0");
            table.save_property_maps(maps);

            const auto ids = table.add_rows({{}, {4.5F, 6.25, DecimalValue{"3.1415"}}});
            check(ids.size() == 2, "numeric defaults did not add both rows");
            auto rows = table.rows();
            const auto defaulted = rows.next();
            const auto explicit_values = rows.next();
            check(defaulted && std::get<float>(defaulted->at(0)) == 1.25F &&
                      std::get<double>(defaulted->at(1)) == -25.0 &&
                      std::get<DecimalValue>(defaulted->at(2)).text == "-12.3456",
                  "decimal and scientific numeric defaults were not evaluated");
            check(explicit_values && std::get<float>(explicit_values->at(0)) == 4.5F &&
                      std::get<double>(explicit_values->at(1)) == 6.25 &&
                      std::get<DecimalValue>(explicit_values->at(2)).text == "3.1415",
                  "explicit numeric values did not override column defaults");

            for (const auto& invalid_expression : {"=1.25 + 2", "=Infinity", "=1e9999"}) {
                set_default("SingleValue", invalid_expression);
                table.save_property_maps(maps);
                const auto before_rejected_write = read_bytes(path);
                expect_error(DatabaseErrorCode::unsupported_operation, [&] {
                    (void)table.add_row({});
                });
                check(read_bytes(path) == before_rejected_write,
                      "an invalid floating-point default modified the database");
            }
            for (const auto& invalid_expression : {"=1.23456", "=922337203685477.5808"}) {
                set_default("SingleValue", "=+1.25");
                set_default("MoneyValue", invalid_expression);
                table.save_property_maps(maps);
                const auto before_invalid_money = read_bytes(path);
                expect_error(DatabaseErrorCode::unsupported_operation, [&] {
                    (void)table.add_row({});
                });
                check(read_bytes(path) == before_invalid_money,
                      "an invalid MONEY default modified the database");
            }
        }
    }
}

void test_column_validation_between(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("column-validation-between.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("Scores").add_column("Score", DataType::int32),
                          TableBuilder("ReverseScores").add_column("Score", DataType::int32),
                          TableBuilder("ReverseFloatingScores").add_column("Score", DataType::float64),
                          TableBuilder("ReverseMoneyScores").add_column("Score", DataType::money)});
        std::vector<RowId> ids;
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            auto table = get_table(database, "Scores");
            PropertyMapInfo map{"Score", 1, {}};
            PropertyInfo rule{"ValidationRule", 0x0a, 0, {}, {}, {}};
            rule.set_value(std::string("between 7 and 10"));
            map.put(std::move(rule));
            PropertyInfo message{"ValidationText", 0x0a, 0, {}, {}, {}};
            message.set_value(std::string("Score must be between 7 and 10"));
            map.put(std::move(message));
            table.save_property_maps({std::move(map)});

            ids = table.add_rows({{std::int32_t{7}}, {std::int32_t{8}}, {std::int32_t{10}}});
            check(ids.size() == 3, "BETWEEN endpoints and an interior value were not accepted");
            const auto before_rejected_values = read_bytes(path);
            for (const auto invalid_score : {6, 11}) {
                try {
                    table.add_row({std::int32_t{invalid_score}});
                    throw std::runtime_error("column BETWEEN validation accepted an out-of-range value");
                } catch (const DatabaseError& error) {
                    check(error.code() == DatabaseErrorCode::constraint_violation &&
                              std::string(error.what()) == "Score must be between 7 and 10" &&
                              error.context().column_name == "Score",
                          "column BETWEEN validation lost its message or column context");
                }
                check(read_bytes(path) == before_rejected_values,
                      "rejected column BETWEEN value modified the database");
            }

            try {
                table.update_row(ids.front(), {std::int32_t{6}});
                throw std::runtime_error("column BETWEEN validation accepted an invalid update");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation,
                      "column BETWEEN update did not report a constraint violation");
            }
            check(read_bytes(path) == before_rejected_values,
                  "rejected column BETWEEN update modified the database");

            const auto set_reverse_column_rule = [](Table& target, const std::string& rule_text,
                                                    const std::string& validation_text) {
                PropertyMapInfo map{"Score", 1, {}};
                PropertyInfo rule_info{"ValidationRule", 0x0a, 0, {}, {}, {}};
                rule_info.set_value(rule_text);
                map.put(std::move(rule_info));
                PropertyInfo text_info{"ValidationText", 0x0a, 0, {}, {}, {}};
                text_info.set_value(validation_text);
                map.put(std::move(text_info));
                target.save_property_maps({std::move(map)});
            };
            const auto expect_reverse_column_rejection = [&](Table& target, Value value,
                                                             const std::string& validation_text) {
                const auto before = read_bytes(path);
                try {
                    target.add_row({std::move(value)});
                    throw std::runtime_error("column BETWEEN accepted a value outside reversed bounds");
                } catch (const DatabaseError& error) {
                    check(error.code() == DatabaseErrorCode::constraint_violation &&
                              std::string(error.what()) == validation_text &&
                              error.context().column_name == "Score",
                          "reversed column BETWEEN lost its message or column context");
                }
                check(read_bytes(path) == before,
                      "rejected reversed column BETWEEN value modified the database");
            };

            auto reverse_table = get_table(database, "ReverseScores");
            set_reverse_column_rule(reverse_table, "between 10 and 7", "Reverse score is out of range");
            const auto reverse_ids = reverse_table.add_rows({{std::int32_t{7}}, {std::int32_t{10}}});
            check(reverse_ids.size() == 2,
                  "integer column BETWEEN did not normalize reversed inclusive bounds");
            expect_reverse_column_rejection(reverse_table, std::int32_t{6}, "Reverse score is out of range");
            expect_reverse_column_rejection(reverse_table, std::int32_t{11}, "Reverse score is out of range");

            auto reverse_floating_table = get_table(database, "ReverseFloatingScores");
            set_reverse_column_rule(reverse_floating_table, "between 1e-3 and -1e-3",
                                    "Reverse floating score is out of range");
            const auto reverse_floating_ids = reverse_floating_table.add_rows({{-0.001}, {0.001}});
            check(reverse_floating_ids.size() == 2,
                  "floating column BETWEEN did not normalize reversed inclusive bounds");
            expect_reverse_column_rejection(reverse_floating_table, -0.0011,
                                            "Reverse floating score is out of range");
            expect_reverse_column_rejection(reverse_floating_table, 0.0011,
                                            "Reverse floating score is out of range");

            auto reverse_money_table = get_table(database, "ReverseMoneyScores");
            set_reverse_column_rule(reverse_money_table, "between 1.25 and -1.25",
                                    "Reverse MONEY score is out of range");
            const auto reverse_money_ids = reverse_money_table.add_rows({{DecimalValue{"-1.25"}},
                                                                         {DecimalValue{"1.25"}}});
            check(reverse_money_ids.size() == 2,
                  "MONEY column BETWEEN did not normalize reversed inclusive bounds");
            expect_reverse_column_rejection(reverse_money_table, DecimalValue{"-1.2501"},
                                            "Reverse MONEY score is out of range");
            expect_reverse_column_rejection(reverse_money_table, DecimalValue{"1.2501"},
                                            "Reverse MONEY score is out of range");
        }

        auto reopened = Database::open(path, DatabaseOpenMode::read_write);
        auto table = get_table(reopened, "Scores");
        check(table.row_count_header() == 3, "column BETWEEN failures changed the row count");
        auto reverse_table = get_table(reopened, "ReverseScores");
        check(reverse_table.row_count_header() == 2,
              "reversed column BETWEEN failures changed the row count");
        try {
            reverse_table.add_row({std::int32_t{6}});
            throw std::runtime_error("reversed column BETWEEN was lost after reopen");
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::constraint_violation &&
                      std::string(error.what()) == "Reverse score is out of range",
                  "reopened reversed column BETWEEN did not match its saved rule");
        }
        check(get_table(reopened, "ReverseFloatingScores").row_count_header() == 2 &&
                  get_table(reopened, "ReverseMoneyScores").row_count_header() == 2,
              "reversed floating/MONEY column BETWEEN failures changed the row count");
        try {
            table.add_row({std::int32_t{11}});
            throw std::runtime_error("saved column BETWEEN validation was lost after reopen");
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::constraint_violation &&
                      std::string(error.what()) == "Score must be between 7 and 10",
                  "reopened column BETWEEN validation did not match its saved rule");
        }
    }
}

void test_floating_column_validation(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("floating-column-validation.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("NumericColumns")
                              .add_column("SingleValue", DataType::float32)
                              .add_column("DoubleValue", DataType::float64)});
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            auto table = get_table(database, "NumericColumns");
            std::vector<PropertyMapInfo> maps{{"SingleValue", 1, {}}, {"DoubleValue", 1, {}}};
            const auto set_column_property = [&](const std::string& column_name,
                                                 const std::string& property_name,
                                                 const std::string& value) {
                auto map = std::find_if(maps.begin(), maps.end(), [&](const auto& candidate) {
                    return candidate.name == column_name;
                });
                check(map != maps.end(), "floating validation property map is missing");
                PropertyInfo property{property_name, 0x0a, 0, {}, {}, {}};
                property.set_value(value);
                map->put(std::move(property));
            };
            set_column_property("SingleValue", "ValidationRule", "between -1.25 and +1.5e1");
            set_column_property("SingleValue", "ValidationText", "SINGLE value is out of range");
            set_column_property("DoubleValue", "ValidationRule", ">= -2.5e1");
            set_column_property("DoubleValue", "ValidationText", "DOUBLE value is too small");
            table.save_property_maps(maps);

            const auto ids = table.add_rows({{-1.25F, -25.0}, {15.0F, 0.0}});
            check(ids.size() == 2, "floating column validation rejected inclusive valid values");
            const auto before_rejected_rows = read_bytes(path);
            const std::vector<std::pair<std::vector<Value>, std::string>> invalid_rows{
                {{-1.5F, 0.0}, "SingleValue"},
                {{15.5F, 0.0}, "SingleValue"},
                {{0.0F, -25.01}, "DoubleValue"},
            };
            for (const auto& [values, column_name] : invalid_rows) {
                try {
                    table.add_row(values);
                    throw std::runtime_error("floating column validation accepted an out-of-range value");
                } catch (const DatabaseError& error) {
                    const auto expected_message = column_name == "SingleValue"
                                                      ? "SINGLE value is out of range"
                                                      : "DOUBLE value is too small";
                    check(error.code() == DatabaseErrorCode::constraint_violation &&
                              std::string(error.what()) == expected_message &&
                              error.context().column_name == column_name,
                          "floating column validation lost its message or column context");
                }
                check(read_bytes(path) == before_rejected_rows,
                      "rejected floating column validation row modified the database");
            }

            set_column_property("SingleValue", "ValidationRule", ">= Infinity");
            table.save_property_maps(maps);
            const auto before_unsupported_operand = read_bytes(path);
            expect_error(DatabaseErrorCode::unsupported_operation, [&] {
                (void)table.add_row({0.0F, 0.0});
            });
            check(read_bytes(path) == before_unsupported_operand,
                  "an unsupported floating validation operand modified the database");
        }

        auto reopened = Database::open(path, DatabaseOpenMode::read_write);
        auto table = get_table(reopened, "NumericColumns");
        check(table.row_count_header() == 2,
              "floating column validation failures changed the saved row count");
        const auto maps = table.property_maps();
        const auto single_map = std::find_if(maps.begin(), maps.end(), [](const auto& map) {
            return map.name == "SingleValue";
        });
        const auto* saved_rule = single_map == maps.end() ? nullptr : single_map->find("ValidationRule");
        check(saved_rule && std::get<std::string>(saved_rule->value) == ">= Infinity",
              "floating column validation rule did not persist after reopen");
    }
}

void test_money_validation(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("money-validation.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("MoneyRules")
                              .add_column("RangeValue", DataType::money)
                              .add_column("ThresholdValue", DataType::money)});
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            auto table = get_table(database, "MoneyRules");
            std::vector<PropertyMapInfo> column_maps{{"RangeValue", 1, {}}, {"ThresholdValue", 1, {}}};
            const auto set_column_property = [&](const std::string& column_name,
                                                 const std::string& property_name,
                                                 const std::string& value) {
                auto map = std::find_if(column_maps.begin(), column_maps.end(), [&](const auto& candidate) {
                    return candidate.name == column_name;
                });
                check(map != column_maps.end(), "MONEY validation property map is missing");
                PropertyInfo property{property_name, 0x0a, 0, {}, {}, {}};
                property.set_value(value);
                map->put(std::move(property));
            };
            set_column_property("RangeValue", "ValidationRule", "between -1.2500 and +1.5e1");
            set_column_property("RangeValue", "ValidationText", "MONEY value is out of range");
            set_column_property("ThresholdValue", "ValidationRule", ">= 1e-5");
            set_column_property("ThresholdValue", "ValidationText", "MONEY value is below threshold");

            PropertyMapInfo table_map{"", 0, {}};
            PropertyInfo table_rule{"ValidationRule", 0x0a, 0, {}, {}, {}};
            table_rule.set_value("[RangeValue] >= -2.0 AND [ThresholdValue] < 2e0");
            table_map.put(std::move(table_rule));
            PropertyInfo table_text{"ValidationText", 0x0a, 0, {}, {}, {}};
            table_text.set_value("MONEY table thresholds failed");
            table_map.put(std::move(table_text));
            column_maps.push_back(std::move(table_map));
            table.save_property_maps(column_maps);

            const auto ids = table.add_rows({
                {DecimalValue{"-1.25"}, DecimalValue{"0.0001"}},
                {DecimalValue{"15.0000"}, DecimalValue{"1.9999"}},
            });
            check(ids.size() == 2, "MONEY validation rejected inclusive bounds or exact decimal thresholds");

            const auto before_rejected_rows = read_bytes(path);
            const auto expect_money_rule_failure = [&](std::vector<Value> values,
                                                       const std::string& expected_message,
                                                       const std::optional<std::string>& expected_column) {
                try {
                    table.add_row(values);
                    throw std::runtime_error("MONEY validation accepted a row outside its rule: " +
                                             expected_message);
                } catch (const DatabaseError& error) {
                    const bool expected_context = expected_column
                        ? error.context().column_name == expected_column
                        : error.context().table_name == "MoneyRules" && !error.context().column_name;
                    const auto actual_column = error.context().column_name.value_or("<none>");
                    check(error.code() == DatabaseErrorCode::constraint_violation &&
                              std::string(error.what()) == expected_message && expected_context,
                          "MONEY validation expected [" + expected_message + "] / [" +
                              expected_column.value_or("table") + "] but received [" + error.what() +
                              "] / [" + actual_column + "]");
                }
                check(read_bytes(path) == before_rejected_rows,
                      "rejected MONEY validation row modified the database");
            };
            expect_money_rule_failure({DecimalValue{"-1.2501"}, DecimalValue{"0.0001"}},
                                      "MONEY value is out of range", "RangeValue");
            expect_money_rule_failure({DecimalValue{"0"}, DecimalValue{"0"}},
                                      "MONEY value is below threshold", "ThresholdValue");
            expect_money_rule_failure({DecimalValue{"0"}, DecimalValue{"2.1"}},
                                      "MONEY table thresholds failed", std::nullopt);
        }

        auto reopened = Database::open(path, DatabaseOpenMode::read_write);
        auto table = get_table(reopened, "MoneyRules");
        check(table.row_count_header() == 2, "rejected MONEY validation rows changed the saved row count");
        const auto column_maps = table.property_maps();
        const auto range_map = std::find_if(column_maps.begin(), column_maps.end(), [](const auto& map) {
            return map.name == "RangeValue";
        });
        const auto* saved_column_rule = range_map == column_maps.end()
            ? nullptr : range_map->find("ValidationRule");
        check(saved_column_rule &&
                  std::get<std::string>(saved_column_rule->value) == "between -1.2500 and +1.5e1",
              "MONEY column validation rule did not persist after reopen");
        const auto table_maps = table.property_maps();
        const auto table_map = std::find_if(table_maps.begin(), table_maps.end(), [](const auto& map) {
            return map.name.empty();
        });
        const auto* saved_table_rule = table_map == table_maps.end()
            ? nullptr : table_map->find("ValidationRule");
        check(saved_table_rule &&
                  std::get<std::string>(saved_table_rule->value) ==
                      "[RangeValue] >= -2.0 AND [ThresholdValue] < 2e0",
              "MONEY table validation rule did not persist after reopen");
    }
}

void test_table_validation_between(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("table-validation-between.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("IntegerBetween").add_column("Value", DataType::int32),
                          TableBuilder("FloatingBetween").add_column("Value", DataType::float64),
                          TableBuilder("MoneyBetween").add_column("Value", DataType::money)});
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            const auto set_table_rule = [](Table& table, const std::string& rule_text) {
                PropertyMapInfo map{"", 0, {}};
                PropertyInfo rule{"ValidationRule", 0x0a, 0, {}, {}, {}};
                rule.set_value(rule_text);
                map.put(std::move(rule));
                PropertyInfo message{"ValidationText", 0x0a, 0, {}, {}, {}};
                message.set_value("Numeric BETWEEN failed");
                map.put(std::move(message));
                table.save_property_maps({std::move(map)});
            };
            const auto expect_table_rejection = [&](Table& table, Value value) {
                const auto before = read_bytes(path);
                try {
                    table.add_row({std::move(value)});
                    throw std::runtime_error("table BETWEEN accepted an out-of-range value");
                } catch (const DatabaseError& error) {
                    check(error.code() == DatabaseErrorCode::constraint_violation &&
                              std::string(error.what()) == "Numeric BETWEEN failed" &&
                              error.context().table_name == table.name(),
                          "table BETWEEN lost its message or table context");
                }
                check(read_bytes(path) == before,
                      "rejected table BETWEEN value modified the database");
            };

            auto integer_table = get_table(database, "IntegerBetween");
            set_table_rule(integer_table, "[Value] BETWEEN +2 AND -2");
            const auto integer_ids = integer_table.add_rows({{std::int32_t{-2}}, {std::int32_t{2}}});
            check(integer_ids.size() == 2, "integer table BETWEEN rejected reversed inclusive bounds");
            expect_table_rejection(integer_table, std::int32_t{-3});
            expect_table_rejection(integer_table, std::int32_t{3});

            auto floating_table = get_table(database, "FloatingBetween");
            set_table_rule(floating_table, "[Value] BETWEEN 1e-3 AND -1e-3");
            const auto floating_ids = floating_table.add_rows({{-0.001}, {0.001}});
            check(floating_ids.size() == 2, "floating table BETWEEN rejected reversed inclusive bounds");
            expect_table_rejection(floating_table, -0.0011);
            expect_table_rejection(floating_table, 0.0011);

            auto money_table = get_table(database, "MoneyBetween");
            set_table_rule(money_table, "[Value] BETWEEN 1.25 AND -1.25");
            const auto money_ids = money_table.add_rows({{DecimalValue{"-1.25"}},
                                                         {DecimalValue{"1.25"}}});
            check(money_ids.size() == 2, "MONEY table BETWEEN rejected reversed inclusive bounds");
            expect_table_rejection(money_table, DecimalValue{"-1.2501"});
            expect_table_rejection(money_table, DecimalValue{"1.2501"});
        }

        auto reopened = Database::open(path, DatabaseOpenMode::read_write);
        for (const auto& table_name : {"IntegerBetween", "FloatingBetween", "MoneyBetween"}) {
            auto table = get_table(reopened, table_name);
            check(table.row_count_header() == 2,
                  "rejected table BETWEEN values changed the saved row count");
            const auto before = read_bytes(path);
            try {
                if (table_name == std::string_view("IntegerBetween")) {
                    table.add_row({std::int32_t{3}});
                } else if (table_name == std::string_view("FloatingBetween")) {
                    table.add_row({0.0011});
                } else {
                    table.add_row({DecimalValue{"1.2501"}});
                }
                throw std::runtime_error("table BETWEEN rule was lost after reopen");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "Numeric BETWEEN failed",
                      "reopened table BETWEEN did not use the saved rule");
            }
            check(read_bytes(path) == before,
                  "reopened table BETWEEN failure modified the database");
        }
    }
}

void test_table_validation_rule(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("table-validation-rule.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("Rules")
                              .add_column("data1", DataType::int32)
                              .add_column("data2", DataType::int32),
                          TableBuilder("TextRules").add_column("Name", DataType::text),
                          TableBuilder("FloatRules")
                              .add_column("SingleValue", DataType::float32)
                              .add_column("DoubleValue", DataType::float64)});
        std::vector<RowId> ids;
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            auto table = get_table(database, "Rules");
            PropertyMapInfo map{"", 0, {}};
            const auto set_text_property = [&](const std::string& name, std::string value) {
                const auto property = std::find_if(map.properties.begin(), map.properties.end(),
                                                   [&](const auto& item) { return item.name == name; });
                if (property == map.properties.end()) {
                    PropertyInfo item{name, 0x0a, 0, {}, {}, {}};
                    item.set_value(std::move(value));
                    map.properties.push_back(std::move(item));
                } else {
                    property->set_value(std::move(value));
                }
            };
            set_text_property("ValidationRule", "([data1] > 10) and ([data2] < 100)");
            set_text_property("ValidationText", "You failed");
            table.save_property_maps({map});

            ids.push_back(table.add_row({std::int32_t{42}, std::int32_t{8}}));
            const auto before_rejected_values = read_bytes(path);
            for (const auto& invalid_row : {std::vector<Value>{std::int32_t{1}, std::int32_t{8}},
                                            std::vector<Value>{std::int32_t{42}, std::int32_t{200}}}) {
                try {
                    table.add_row(invalid_row);
                    throw std::runtime_error("table validation accepted a row violating one AND condition");
                } catch (const DatabaseError& error) {
                    check(error.code() == DatabaseErrorCode::constraint_violation &&
                              std::string(error.what()) == "You failed" &&
                              error.context().table_name == "Rules",
                          "table validation lost its message or table context");
                }
                check(read_bytes(path) == before_rejected_values,
                      "rejected table validation insert modified the database");
            }

            set_text_property("ValidationRule", "([data1] > 10and [data2] < 100)");
            table.save_property_maps({map});
            const auto before_unsupported_rule = read_bytes(path);
            try {
                table.add_row({std::int32_t{42}, std::int32_t{8}});
                throw std::runtime_error("table validation accepted a malformed AND token");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::unsupported_operation,
                      "malformed table validation syntax did not report unsupported operation");
            }
            check(read_bytes(path) == before_unsupported_rule,
                  "unsupported table validation syntax modified the database");

            set_text_property("ValidationRule",
                              "[data1] > 10 OR [data2] > 90 AND [data1] < 0");
            table.save_property_maps({map});
            table.add_row({std::int32_t{42}, std::int32_t{8}});
            table.add_row({std::int32_t{-1}, std::int32_t{95}});
            const auto before_rejected_or = read_bytes(path);
            try {
                table.add_row({std::int32_t{1}, std::int32_t{95}});
                throw std::runtime_error("table validation ignored AND precedence within an OR expression");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "You failed",
                      "table validation OR rule did not report its configured constraint");
            }
            check(read_bytes(path) == before_rejected_or,
                  "rejected OR validation row modified the database");

            set_text_property("ValidationRule", "([data1] IS NULL) OR ([data2] is not null)");
            table.save_property_maps({map});
            table.add_row({std::monostate{}, std::monostate{}});
            table.add_row({std::int32_t{1}, std::int32_t{8}});
            const auto before_rejected_null = read_bytes(path);
            try {
                table.add_row({std::int32_t{42}, std::monostate{}});
                throw std::runtime_error("table validation ignored IS NULL / IS NOT NULL predicates");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "You failed",
                      "table validation NULL predicate did not report its configured constraint");
            }
            check(read_bytes(path) == before_rejected_null,
                  "rejected NULL predicate row modified the database");

            set_text_property("ValidationRule", "([data1] > 10or [data2] > 90)");
            table.save_property_maps({map});
            const auto before_unsupported_or = read_bytes(path);
            try {
                table.add_row({std::int32_t{42}, std::int32_t{8}});
                throw std::runtime_error("table validation accepted a malformed OR token");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::unsupported_operation,
                      "malformed table validation OR syntax did not report unsupported operation");
            }
            check(read_bytes(path) == before_unsupported_or,
                  "unsupported OR validation syntax modified the database");

            set_text_property("ValidationRule", "([data1] > 10) and ([data2] < 100)");
            table.save_property_maps({map});

            const auto before_rejected_update = read_bytes(path);
            try {
                table.update_row(ids.front(), {std::int32_t{1}, std::int32_t{8}});
                throw std::runtime_error("table validation accepted an invalid update");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "You failed",
                      "table validation update did not report its configured constraint");
            }
            check(read_bytes(path) == before_rejected_update,
                  "rejected table validation update modified the database");

            ids.push_back(table.add_row({std::int32_t{54}, std::int32_t{9}}));
            set_text_property("ValidationRule", "[data2] < 100");
            set_text_property("ValidationText", "Too big");
            table.save_property_maps({map});
            ids.push_back(table.add_row({std::int32_t{1}, std::int32_t{9}}));
            const auto before_rejected_immediate_rule = read_bytes(path);
            try {
                table.add_row({std::int32_t{42}, std::int32_t{200}});
                throw std::runtime_error("updated table validation rule accepted an invalid row");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "Too big",
                      "updated table validation rule did not immediately use its new message");
            }
            check(read_bytes(path) == before_rejected_immediate_rule,
                  "rejected row after table validation update modified the database");
        }

        auto reopened = Database::open(path, DatabaseOpenMode::read_write);
        auto table = get_table(reopened, "Rules");
        check(table.row_count_header() == 7, "table validation failures changed the row count");

        auto text_table = get_table(reopened, "TextRules");
        PropertyMapInfo text_map{"", 0, {}};
        const auto set_text_property = [&](const std::string& name, std::string value) {
            const auto property = std::find_if(text_map.properties.begin(), text_map.properties.end(),
                                               [&](const auto& item) { return item.name == name; });
            if (property == text_map.properties.end()) {
                PropertyInfo item{name, 0x0a, 0x01, {}, {}, {}};
                item.set_value(std::move(value));
                text_map.properties.push_back(std::move(item));
            } else {
                property->set_value(std::move(value));
            }
        };
        set_text_property("ValidationRule", "[Name] IS NULL");
        set_text_property("ValidationText", "Name must be empty");
        text_table.save_property_maps({text_map});
        text_table.add_row({std::monostate{}});
        const auto before_rejected_text = read_bytes(path);
        try {
            text_table.add_row({std::string{"named"}});
            throw std::runtime_error("table IS NULL validation did not accept/reject a text value");
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::constraint_violation &&
                      std::string(error.what()) == "Name must be empty",
                  "table IS NULL validation lost its text-column error");
        }
        check(read_bytes(path) == before_rejected_text,
              "rejected text IS NULL row modified the database");

        set_text_property("ValidationRule", "[Name] IS NOT NULL");
        text_table.save_property_maps({text_map});
        text_table.add_row({std::string{"named"}});
        const auto before_rejected_text_null = read_bytes(path);
        try {
            text_table.add_row({std::monostate{}});
            throw std::runtime_error("table IS NOT NULL validation did not reject a NULL text value");
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::constraint_violation &&
                      std::string(error.what()) == "Name must be empty",
                  "table IS NOT NULL validation lost its text-column error");
        }
        check(read_bytes(path) == before_rejected_text_null,
              "rejected text IS NOT NULL row modified the database");

        auto float_table = get_table(reopened, "FloatRules");
        PropertyMapInfo float_map{"", 0, {}};
        const auto set_float_property = [&](const std::string& name, std::string value) {
            const auto property = std::find_if(float_map.properties.begin(), float_map.properties.end(),
                                               [&](const auto& item) { return item.name == name; });
            if (property == float_map.properties.end()) {
                PropertyInfo item{name, 0x0a, 0x01, {}, {}, {}};
                item.set_value(std::move(value));
                float_map.properties.push_back(std::move(item));
            } else {
                property->set_value(std::move(value));
            }
        };
        set_float_property("ValidationRule", "[SingleValue] >= -1.25 AND [DoubleValue] < 2.5e1");
        set_float_property("ValidationText", "Numeric thresholds failed");
        float_table.save_property_maps({float_map});
        const auto float_ids = float_table.add_rows({{-1.0F, 24.5}, {-1.25F, 24.99}, {0.0F, 24.99}});
        check(float_ids.size() == 3, "table validation rejected valid SINGLE/DOUBLE values");
        const auto before_rejected_float = read_bytes(path);
        for (const auto& invalid_row : {std::vector<Value>{-1.5F, 1.0},
                                        std::vector<Value>{0.0F, 25.0}}) {
            try {
                float_table.add_row(invalid_row);
                throw std::runtime_error("table validation accepted an out-of-range floating value");
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::constraint_violation &&
                          std::string(error.what()) == "Numeric thresholds failed" &&
                          error.context().table_name == "FloatRules",
                      "floating table validation lost its message or table context");
            }
            check(read_bytes(path) == before_rejected_float,
                  "rejected floating table validation row modified the database");
        }

        const auto before_rejected_reopened_values = read_bytes(path);
        try {
            table.add_row({std::int32_t{42}, std::int32_t{200}});
            throw std::runtime_error("saved table validation rule was lost after reopen");
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::constraint_violation &&
                      std::string(error.what()) == "Too big",
                  "reopened table validation did not use the saved rule and message");
        }
        check(read_bytes(path) == before_rejected_reopened_values,
              "rejected reopened table validation modified the database");
    }
}

void test_column_validator_factory(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("column-validator.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format,
                         {TableBuilder("Scores")
                              .add_column("Score", DataType::int32)
                              .add_column("Note", DataType::text)});
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            database.set_column_validator_factory(
                [](std::string_view table_name, const ColumnInfo& column) -> ColumnValidator {
                    if (table_name != "Scores" || column.name != "Score") {
                        return {};
                    }
                    return [](const ColumnInfo& validated_column, const Value& value) -> Value {
                        check(validated_column.type == DataType::int32,
                              "column validator received incorrect column metadata");
                        const auto* score = std::get_if<std::int32_t>(&value);
                        if (!score || *score < 0) {
                            throw std::invalid_argument("Score must be a nonnegative integer");
                        }
                        return std::int32_t{*score * 10};
                    };
                });
            auto table = get_table(database, "Scores");
            const auto ids = table.add_rows({{std::int32_t{2}, std::string("first")},
                                             {std::int32_t{3}, std::string("second")}});
            check(ids.size() == 2, "column validator batch insert returned the wrong row count");
            auto rows = table.rows();
            const auto first = rows.next();
            const auto second = rows.next();
            const auto* first_score = first ? std::get_if<std::int32_t>(&first->at(0)) : nullptr;
            const auto* second_score = second ? std::get_if<std::int32_t>(&second->at(0)) : nullptr;
            check(first_score && *first_score == 20 && second_score && *second_score == 30,
                  "column validator did not replace inserted values");

            const auto before_rejected_batch = read_bytes(path);
            try {
                table.add_rows({{std::int32_t{4}, std::string("not committed")},
                                {std::int32_t{-1}, std::string("rejected")}});
                throw std::runtime_error("column validator accepted a negative batch value");
            } catch (const std::invalid_argument& error) {
                check(std::string(error.what()) == "Score must be a nonnegative integer",
                      "column validator rejection lost its exception");
            }
            check(read_bytes(path) == before_rejected_batch,
                  "column validator batch rejection modified the database");

            table.update_row(ids.front(), {std::int32_t{5}, std::string("updated")});
            const auto before_rejected_update = read_bytes(path);
            try {
                table.update_row(ids.front(), {std::int32_t{-2}, std::string("not committed")});
                throw std::runtime_error("column validator accepted a negative update value");
            } catch (const std::invalid_argument& error) {
                check(std::string(error.what()) == "Score must be a nonnegative integer",
                      "column validator update rejection lost its exception");
            }
            check(read_bytes(path) == before_rejected_update,
                  "column validator update rejection modified the database");
            const auto updated = table.rows().next();
            const auto* updated_score = updated ? std::get_if<std::int32_t>(&updated->at(0)) : nullptr;
            const auto* updated_note = updated ? std::get_if<std::string>(&updated->at(1)) : nullptr;
            check(updated_score && *updated_score == 50 && updated_note && *updated_note == "updated",
                  "column validator update did not persist its replacement value");
        }
        const auto reopened = Database::open(path);
        const auto row = get_table(reopened, "Scores").rows().next();
        const auto* reopened_score = row ? std::get_if<std::int32_t>(&row->at(0)) : nullptr;
        check(reopened_score && *reopened_score == 50,
              "column validator replacement did not survive reopening");
    }
}

void test_create_file_link(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const auto path = directory.file("links.mdb");
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        Database::create(path, format, {TableBuilder("Local").add_column("Id", DataType::int32)});
        {
            auto database = Database::open(path, DatabaseOpenMode::read_write);
            database.create_linked_table("Alias", "unopened-target.mdb", "Remote");
            expect_error(DatabaseErrorCode::unsupported_operation, [&] {
                database.create_linked_table("ALIAS", "other.mdb", "Remote");
            });
            expect_error(DatabaseErrorCode::unsupported_operation, [&] {
                database.create_linked_table("local", "other.mdb", "Remote");
            });
            check(database.linked_tables().size() == 1, "new link was not visible to the writable handle");
        }
        const auto database = Database::open(path);
        const auto links = database.linked_tables();
        check(links.size() == 1 && links.front().kind == LinkedTableKind::file &&
                  links.front().name == "Alias" && links.front().target_database == "unopened-target.mdb" &&
                  links.front().target_table == "Remote",
              "link metadata did not survive reopening");
        const auto tables = database.tables();
        const auto system_table = [&](std::string_view name) -> const Table& {
            const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto& table) {
                return table.name() == name;
            });
            check(found != tables.end(), "system table was missing");
            return *found;
        };
        std::optional<std::int32_t> linked_id;
        auto objects = system_table("MSysObjects").rows();
        while (const auto row = objects.next()) {
            const auto* name = std::get_if<std::string>(&row->at(2));
            if (name && *name == "Alias") {
                linked_id = std::get<std::int32_t>(row->at(0));
                break;
            }
        }
        check(linked_id.has_value(), "link row has no object id");
        auto entries = system_table("MSysACEs").rows();
        bool found_access = false;
        while (const auto row = entries.next()) {
            const auto* id = std::get_if<std::int32_t>(&row->at(0));
            if (id && *id == *linked_id) {
                found_access = true;
                check(!std::get<BlobValue>(row->at(1)).bytes.empty() &&
                          std::get<std::int32_t>(row->at(2)) == 1048575 &&
                          !std::get<bool>(row->at(3)),
                      "link access entry has incorrect SID, mask or inheritance");
            }
        }
        check(found_access, "the link did not receive an access control entry");
    }
}

void append_odbc_link_fixture(const std::filesystem::path& path) {
    using namespace rea_jackcess;
    auto file = detail::PageFile::open(path);
    auto schema = detail::read_table_schema_catalog(file);
    const auto catalog = std::find_if(schema.tables.begin(), schema.tables.end(), [](const auto& table) {
        return table.definition_page_number == 2 && table.name == "MSysObjects";
    });
    check(catalog != schema.tables.end(), "ODBC fixture could not find MSysObjects");
    const auto objects = detail::read_system_catalog_objects(file, catalog->definition, std::nullopt);
    const auto tables_container = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
        return object.name == "Tables" && object.type == 3 && object.id;
    });
    check(tables_container != objects.end(), "ODBC fixture could not find the Tables catalog container");

    std::int32_t largest_synthetic_id = std::numeric_limits<std::int32_t>::min();
    for (const auto& object : objects) {
        if (object.id && *object.id < 0) {
            largest_synthetic_id = std::max(largest_synthetic_id, *object.id);
        }
    }
    check(largest_synthetic_id != -1, "ODBC fixture has no free synthetic object identifier");

    std::vector<Value> row(catalog->definition.columns.size());
    const auto set = [&](std::string_view name, Value value) {
        const auto column = std::find_if(catalog->definition.columns.begin(), catalog->definition.columns.end(),
                                         [&](const auto& candidate) { return candidate.name == name; });
        check(column != catalog->definition.columns.end(),
              "ODBC fixture is missing MSysObjects column " + std::string(name));
        row.at(column->column_number) = std::move(value);
    };
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto date_serial = 25569.0 + std::chrono::duration<double>(now).count() / 86400.0;
    set("Id", largest_synthetic_id + 1);
    set("ParentId", *tables_container->id);
    set("Name", std::string("OdbcAlias"));
    set("Type", std::int16_t{4});
    set("Flags", std::int32_t{0});
    set("DateCreate", date_serial);
    set("DateUpdate", date_serial);
    set("Owner", BlobValue{{0xa6, 0x33}});
    set("ForeignName", std::string("Remote"));
    set("Connect", MemoValue{"DSN=Fixture;UID=test;PWD=secret;", {}});

    bool write_state_valid = true;
    std::recursive_mutex write_mutex;
    detail::Jet4WriteContext context{file, schema, true, write_state_valid, write_mutex};
    const auto index = static_cast<std::size_t>(catalog - schema.tables.begin());
    detail::append_jet4_catalog_rows(context, index, {row});
}

void test_open_odbc_linked_table_custom_source(const TemporaryDirectory& directory,
                                              rea_jackcess::DatabaseFormat format) {
    using namespace rea_jackcess;
    const auto suffix = format == DatabaseFormat::v2000 ? "v2000" : "v2003";
    const auto path = directory.file(std::string("odbc-link-") + suffix + ".mdb");
    {
        const auto created = Database::create(
            path, format, {TableBuilder("Local").add_column("Id", DataType::int32)});
        (void)created;
    }
    append_odbc_link_fixture(path);

    auto database = Database::open(path);
    const auto links = database.linked_tables();
    check(links.size() == 1 && links.front().name == "OdbcAlias" &&
              links.front().kind == LinkedTableKind::odbc && !links.front().target_database &&
              links.front().target_table == "Remote" &&
              links.front().connection_string == "DSN=Fixture;UID=test;PWD=secret;",
          "ODBC linked-table metadata did not round-trip from the catalog");

    expect_error(DatabaseErrorCode::unsupported_operation, [&] {
        (void)database.open_linked_table(
            "OdbcAlias", [](const std::filesystem::path&, const LinkedTableInfo&) {
                throw std::runtime_error("ODBC link incorrectly invoked the file resolver");
                return std::filesystem::path{};
            });
    });

    std::size_t source_resolver_calls = 0;
    std::size_t file_resolver_calls = 0;
    std::size_t load_calls = 0;
    class FixtureOdbcSource final : public CustomLinkedSource {
    public:
        explicit FixtureOdbcSource(std::size_t& calls) : calls_(calls) {}

    protected:
        std::optional<LinkedTableSnapshot> load_table_impl(std::string_view table_name) override {
            ++calls_;
            if (table_name != "Remote") {
                return std::nullopt;
            }
            return LinkedTableSnapshot{
                TableBuilder("Remote").add_column("Id", DataType::int32)
                                       .add_column("Description", DataType::text),
                {{std::int32_t{17}, std::string("from ODBC source")}}};
        }

    private:
        std::size_t& calls_;
    };

    const auto custom_resolver = [&](const std::filesystem::path& source, const LinkedTableInfo& link) {
        ++source_resolver_calls;
        check(source == path && link.kind == LinkedTableKind::odbc && link.name == "OdbcAlias" &&
                  !link.target_database && link.target_table == "Remote" &&
                  link.connection_string == "DSN=Fixture;UID=test;PWD=secret;",
              "custom resolver did not receive the ODBC link's stored source metadata");
        return std::make_shared<FixtureOdbcSource>(load_calls);
    };
    const auto file_resolver = [&](const std::filesystem::path&, const LinkedTableInfo&) {
        ++file_resolver_calls;
        return std::filesystem::path{};
    };

    const auto linked = database.open_linked_table("OdbcAlias", file_resolver, custom_resolver);
    check(linked && linked->name() == "Remote" && source_resolver_calls == 1 &&
              file_resolver_calls == 0 && load_calls == 1,
          "custom source did not resolve the ODBC linked table exactly once");
    const auto row = linked->rows().next();
    check(row && std::get<std::int32_t>(row->at(0)) == 17 &&
              std::get<std::string>(row->at(1)) == "from ODBC source",
          "the ODBC source snapshot was not materialized as a readable linked table");

    const auto linked_again = database.open_linked_table("OdbcAlias", file_resolver, custom_resolver);
    check(linked_again && source_resolver_calls == 1 && file_resolver_calls == 0 && load_calls == 1,
          "the ODBC source or materialized table was not cached");
}

void test_open_linked_table_chain_and_cycle(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        const auto root_path = directory.file(format == DatabaseFormat::v2000 ? "link-root-v2000.mdb"
                                                                                : "link-root-v2003.mdb");
        const auto middle_path = directory.file(format == DatabaseFormat::v2000 ? "link-middle-v2000.mdb"
                                                                                  : "link-middle-v2003.mdb");
        const auto leaf_path = directory.file(format == DatabaseFormat::v2000 ? "link-leaf-v2000.mdb"
                                                                                : "link-leaf-v2003.mdb");
        const auto missing_path = directory.file(format == DatabaseFormat::v2000
                                                     ? "link-missing-v2000.mdb"
                                                     : "link-missing-v2003.mdb");
        const auto make_link_database = [&](const std::filesystem::path& path) {
            Database::create(path, format,
                             {TableBuilder("Placeholder").add_column("Id", DataType::int32)});
        };
        make_link_database(root_path);
        make_link_database(middle_path);
        Database::create(leaf_path, format,
                         {TableBuilder("Remote")
                              .add_column("Id", DataType::int32)
                              .add_column("Label", DataType::text)});
        {
            auto leaf = Database::open(leaf_path, DatabaseOpenMode::read_write);
            auto remote = get_table(leaf, "Remote");
            remote.add_row({std::int32_t{42}, std::string("resolved")});
        }
        {
            auto middle = Database::open(middle_path, DatabaseOpenMode::read_write);
            middle.create_linked_table("Nested", leaf_path.string(), "Remote");
        }
        {
            auto root = Database::open(root_path, DatabaseOpenMode::read_write);
            root.create_linked_table("Alias", middle_path.string(), "Nested");
            root.create_linked_table("AliasCopy", middle_path.string(), "Nested");
            root.create_linked_table("Unavailable", missing_path.string(), "Remote");
        }

        std::optional<Table> resolved;
        std::vector<std::string> resolved_links;
        {
            auto root = Database::open(root_path);
            resolved = root.open_linked_table(
                "Alias", [&](const std::filesystem::path& source, const LinkedTableInfo& link) {
                    if (resolved_links.empty()) {
                        check(source == root_path && link.name == "Alias",
                              "the first resolver call did not receive the root link context");
                    } else {
                        check(source == middle_path && link.name == "Nested",
                              "the nested resolver call did not receive the middle link context");
                    }
                    check(link.kind == LinkedTableKind::file && link.target_database.has_value(),
                          "the resolver received incomplete file-link metadata");
                    resolved_links.push_back(link.name);
                    return std::filesystem::path(*link.target_database);
                });
            std::size_t repeated_resolver_calls = 0;
            const auto copy = root.open_linked_table(
                "AliasCopy", [&](const std::filesystem::path&, const LinkedTableInfo& link) {
                    ++repeated_resolver_calls;
                    return std::filesystem::path(*link.target_database);
                });
            check(copy.name() == "Remote" && repeated_resolver_calls == 0,
                  "a second alias did not reuse its cached linked database");
        }
        check(resolved_links == std::vector<std::string>({"Alias", "Nested"}),
              "a linked-table chain did not invoke the resolver once per file hop");
        check(resolved && resolved->name() == "Remote",
              "the linked-table chain did not return the final table");
        {
            const auto root = Database::open(root_path);
            bool reported_missing_target = false;
            try {
                (void)root.open_linked_table(
                    "Unavailable", [](const std::filesystem::path&, const LinkedTableInfo& link) {
                        return std::filesystem::path(*link.target_database);
                    });
            } catch (const DatabaseError& error) {
                reported_missing_target = error.code() == DatabaseErrorCode::io_error &&
                                          error.path() == missing_path;
            }
            check(reported_missing_target,
                  "an unavailable linked database did not report an I/O error with its target path");
        }
        const auto row = resolved->rows().next();
        check(row && std::get<std::int32_t>(row->at(0)) == 42 &&
                  std::get<std::string>(row->at(1)) == "resolved",
              "the resolved table lost its database state after its source handle closed");
        expect_error(DatabaseErrorCode::unsupported_operation, [&] {
            resolved->add_row({std::int32_t{43}, std::string("read-only")});
        });
        resolved.reset();
        {
            auto writable_root = Database::open(root_path, DatabaseOpenMode::read_write);
            auto writable_remote = writable_root.open_linked_table(
                "Alias", [](const std::filesystem::path&, const LinkedTableInfo& link) {
                    return std::filesystem::path(*link.target_database);
                });
            writable_remote.add_row({std::int32_t{43}, std::string("written-through")});
        }
        {
            const auto leaf = Database::open(leaf_path);
            auto rows = get_table(leaf, "Remote").rows();
            const auto first = rows.next();
            const auto second = rows.next();
            check(first && second && std::get<std::int32_t>(second->at(0)) == 43 &&
                      std::get<std::string>(second->at(1)) == "written-through",
                  "a writable source did not open linked tables for write-through updates");
        }

        const auto cycle_root_path = directory.file(format == DatabaseFormat::v2000 ? "cycle-root-v2000.mdb"
                                                                                      : "cycle-root-v2003.mdb");
        const auto cycle_middle_path = directory.file(format == DatabaseFormat::v2000
                                                          ? "cycle-middle-v2000.mdb"
                                                          : "cycle-middle-v2003.mdb");
        make_link_database(cycle_root_path);
        make_link_database(cycle_middle_path);
        {
            auto root = Database::open(cycle_root_path, DatabaseOpenMode::read_write);
            root.create_linked_table("Alias", cycle_middle_path.string(), "Nested");
        }
        {
            auto middle = Database::open(cycle_middle_path, DatabaseOpenMode::read_write);
            middle.create_linked_table("Nested", cycle_root_path.string(), "Alias");
        }
        auto cycle_root = Database::open(cycle_root_path);
        expect_error(DatabaseErrorCode::unsupported_operation, [&] {
            (void)cycle_root.open_linked_table(
                "Alias", [](const std::filesystem::path&, const LinkedTableInfo& link) {
                    return std::filesystem::path(*link.target_database);
                });
        });
    }
}

void test_open_custom_linked_table_snapshots(const TemporaryDirectory& directory,
                                             rea_jackcess::DatabaseFormat format) {
    using namespace rea_jackcess;
    const auto suffix = format == DatabaseFormat::v2000 ? "v2000" : "v2003";
    const auto root_path = directory.file(std::string("custom-link-root-") + suffix + ".mdb");
    const auto fallback_path = directory.file(std::string("custom-link-fallback-") + suffix + ".mdb");
    Database::create(root_path, format,
                      {TableBuilder("Placeholder").add_column("Id", DataType::int32)});
    Database::create(fallback_path, format,
                      {TableBuilder("Remote").add_column("Id", DataType::int32)});
    {
        auto fallback = Database::open(fallback_path, DatabaseOpenMode::read_write);
        get_table(fallback, "Remote").add_row({std::int32_t{99}});
    }
    {
        auto root = Database::open(root_path, DatabaseOpenMode::read_write);
        root.create_linked_table("Custom", "custom://fixture", "Remote");
        root.create_linked_table("CustomCopy", "custom://fixture", "Remote");
        root.create_linked_table("CustomOther", "custom://fixture", "Other");
        root.create_linked_table("CustomMissing", "custom://fixture", "Missing");
        root.create_linked_table("Fallback", fallback_path.string(), "Remote");
        root.create_linked_table("FallbackCopy", fallback_path.string(), "Remote");
    }

    std::size_t remote_callback_calls = 0;
    std::size_t other_callback_calls = 0;
    std::size_t missing_callback_calls = 0;
    std::size_t custom_source_close_calls = 0;
    std::size_t custom_source_resolver_calls = 0;
    std::size_t file_resolver_calls = 0;
    class FixtureCustomSource final : public CustomLinkedSource {
    public:
        FixtureCustomSource(std::size_t& remote_calls,
                            std::size_t& other_calls,
                            std::size_t& missing_calls,
                            std::size_t& close_calls)
            : remote_calls_(remote_calls),
              other_calls_(other_calls),
              missing_calls_(missing_calls),
              close_calls_(close_calls) {}

        ~FixtureCustomSource() override {
            ++close_calls_;
        }

    protected:
        std::optional<LinkedTableSnapshot> load_table_impl(std::string_view table_name) override {
            if (table_name == "Remote") {
                ++remote_calls_;
                return LinkedTableSnapshot{
                    TableBuilder("Remote").add_column("Id", DataType::int32)
                                           .add_column("Label", DataType::text),
                    {{std::int32_t{42}, std::string("custom")}}};
            }
            if (table_name == "Other") {
                ++other_calls_;
                return LinkedTableSnapshot{
                    TableBuilder("Other").add_column("Id", DataType::int32),
                    {{std::int32_t{7}}}};
            }
            ++missing_calls_;
            return std::nullopt;
        }

    private:
        std::size_t& remote_calls_;
        std::size_t& other_calls_;
        std::size_t& missing_calls_;
        std::size_t& close_calls_;
    };

    std::optional<Table> retained;
    {
        const auto root = Database::open(root_path);
        const auto file_resolver = [&](const std::filesystem::path&, const LinkedTableInfo& link) {
            ++file_resolver_calls;
            return std::filesystem::path(*link.target_database);
        };
        const auto custom_resolver = [&](const std::filesystem::path& source,
                                         const LinkedTableInfo& link)
            -> std::shared_ptr<CustomLinkedSource> {
            ++custom_source_resolver_calls;
            check(source == root_path, "custom resolver received the wrong source database");
            if (link.target_database == "custom://fixture") {
                return std::make_shared<FixtureCustomSource>(remote_callback_calls,
                                                             other_callback_calls,
                                                             missing_callback_calls,
                                                             custom_source_close_calls);
            }
            return {};
        };

        retained = root.open_linked_table("Custom", file_resolver, custom_resolver);
        const auto custom_copy = root.open_linked_table("CustomCopy", file_resolver, custom_resolver);
        const auto other = root.open_linked_table("CustomOther", LinkedTableResolver{}, custom_resolver);
        const auto fallback = root.open_linked_table("Fallback", file_resolver, custom_resolver);
        const auto fallback_copy = root.open_linked_table("FallbackCopy", file_resolver, custom_resolver);
        const auto missing = root.open_linked_table("CustomMissing", file_resolver, custom_resolver);
        check(!missing, "an absent table in a custom source did not return an empty result");
        check(custom_copy && custom_copy->name() == "Remote" && other && other->name() == "Other" &&
                  fallback && fallback->name() == "Remote" && fallback_copy &&
                  fallback_copy->name() == "Remote",
              "custom or fallback resolver returned the wrong target table");
        const auto other_row = other->rows().next();
        const auto fallback_row = fallback->rows().next();
        check(other_row && std::get<std::int32_t>(other_row->at(0)) == 7,
              "custom resolver did not materialize the second target table");
        check(fallback_row && std::get<std::int32_t>(fallback_row->at(0)) == 99,
              "a declined custom source did not fall back to the file resolver");
        check(remote_callback_calls == 1 && other_callback_calls == 1 && missing_callback_calls == 1 &&
                  custom_source_resolver_calls == 2 && file_resolver_calls == 1,
              "custom source/table caching or file resolver fallback invoked callbacks unexpectedly");
        check(custom_source_close_calls == 0,
              "custom source closed while linked tables still retained it");
    }

    check(custom_source_close_calls == 0,
          "custom source closed when its source database was destroyed despite a retained linked table");
    check(retained && retained->name() == "Remote",
          "custom linked table did not survive destruction of its source database");
    const auto retained_row = retained->rows().next();
    check(retained_row && std::get<std::int32_t>(retained_row->at(0)) == 42 &&
              std::get<std::string>(retained_row->at(1)) == "custom",
          "custom linked table lost its materialized data after source closure");
    expect_error(DatabaseErrorCode::unsupported_operation, [&] {
        retained->add_row({std::int32_t{43}, std::string("read-only")});
    });
    retained.reset();
    check(custom_source_close_calls == 1,
          "custom source was not closed after its final linked table was released");

    std::size_t writable_source_rows = 0;
    std::size_t unused_other_rows = 0;
    std::size_t unused_missing_rows = 0;
    std::size_t writable_source_close_calls = 0;
    {
        auto writable_root = Database::open(root_path, DatabaseOpenMode::read_write);
        auto table = writable_root.open_linked_table(
            "Custom", LinkedTableResolver{}, [&](const std::filesystem::path&, const LinkedTableInfo&) {
                return std::make_shared<FixtureCustomSource>(writable_source_rows,
                                                             unused_other_rows,
                                                             unused_missing_rows,
                                                             writable_source_close_calls);
            });
        check(table.has_value(), "writable custom resolver did not return its table");
        table->add_row({std::int32_t{43}, std::string("temporary write")});
        auto rows = table->rows();
        check(rows.next().has_value() && rows.next().has_value() && writable_source_rows == 1,
              "a writable source did not expose a writable custom snapshot table");
    }
    check(writable_source_close_calls == 1,
          "writable custom source was not released with its linked database");
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

void test_create_and_write_boolean_columns(const TemporaryDirectory& directory) {
    const auto path = directory.file("boolean-rows.mdb");
    rea_jackcess::TableBuilder table_builder("Booleans");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Flag", rea_jackcess::DataType::boolean)
        .add_column("Maybe", rea_jackcess::DataType::boolean)
        .add_column("Tail", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Booleans";
        });
        check(table != tables.end() && table->columns().size() == 4,
              "new BOOLEAN table did not preserve its four-column schema");
        const auto& flag = table->columns()[1];
        const auto& maybe = table->columns()[2];
        check(flag.type == rea_jackcess::DataType::boolean && flag.fixed && flag.size == 1 &&
                  flag.raw_type_code == 0x01 && flag.nullable == std::optional<bool>(true),
              "new BOOLEAN column did not carry the Jet4 bit layout");
        check(maybe.type == rea_jackcess::DataType::boolean && maybe.fixed && maybe.size == 1,
              "second BOOLEAN column did not keep the Jet4 bit layout");
        check(!table->rows().next(), "new BOOLEAN table unexpectedly contains rows");
    }

    rea_jackcess::RowId first_row_id{};
    rea_jackcess::RowId second_row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Booleans";
        });
        check(table != tables.end(), "read-write database did not expose table Booleans");

        const auto row_ids = table->add_rows({
            {std::int32_t{1}, true, false, std::int32_t{10}},
            {std::int32_t{2}, false, true, std::int32_t{-5}},
            {std::int32_t{3}, std::monostate{}, std::monostate{}, std::int32_t{0}},
        });
        check(row_ids.size() == 3 && table->row_count_header() == 3,
              "BOOLEAN batch insertion returned the wrong row count");
        first_row_id = row_ids[0];
        second_row_id = row_ids[1];

        auto rows = table->rows();
        const auto first = rows.next();
        check(first && first->id() == first_row_id && std::get<bool>(first->at(1)) &&
                  !std::get<bool>(first->at(2)) && std::get<std::int32_t>(first->at(3)) == 10,
              "BOOLEAN TRUE/FALSE encoding changed the stored row");
        const auto second = rows.next();
        check(second && !std::get<bool>(second->at(1)) && std::get<bool>(second->at(2)),
              "BOOLEAN FALSE/TRUE encoding changed the stored row");
        const auto third = rows.next();
        check(third && !std::get<bool>(third->at(1)) && !std::get<bool>(third->at(2)) &&
                  std::get<std::int32_t>(third->at(3)) == 0,
              "a NULL BOOLEAN did not read back as FALSE");
        check(!rows.next(), "BOOLEAN batch insertion returned an unexpected extra row");

        const auto before_invalid_write = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table->add_row(
                {std::int32_t{4}, std::int32_t{1}, false, std::int32_t{0}});
        });
        check(read_bytes(path) == before_invalid_write,
              "a mismatched BOOLEAN value type changed the database before validation failed");

        table->update_row(first_row_id,
                          {std::int32_t{1}, false, false, std::int32_t{11}});
        table->update_row(second_row_id,
                          {std::int32_t{2}, true, true, std::int32_t{-5}});
        check(table->row_count_header() == 3, "BOOLEAN update changed the table row count");
        auto updated_rows = table->rows();
        const auto updated_first = updated_rows.next();
        check(updated_first && updated_first->id() == first_row_id &&
                  !std::get<bool>(updated_first->at(1)) &&
                  std::get<std::int32_t>(updated_first->at(3)) == 11,
              "BOOLEAN update did not clear the TRUE bit or preserve the neighbouring LONG");
        const auto updated_second = updated_rows.next();
        check(updated_second && updated_second->id() == second_row_id &&
                  std::get<bool>(updated_second->at(1)) && std::get<bool>(updated_second->at(2)),
              "BOOLEAN update did not set the TRUE bits");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Booleans";
    });
    check(table != tables.end() && table->row_count_header() == 3,
          "reopened BOOLEAN table has an incorrect row count");
    const std::vector<std::pair<bool, bool>> expected{{false, false}, {true, true}, {false, false}};
    auto rows = table->rows();
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto row = rows.next();
        check(row && std::get<bool>(row->at(1)) == expected[index].first &&
                  std::get<bool>(row->at(2)) == expected[index].second,
              "reopened BOOLEAN table did not preserve its bit values");
    }
    check(!rows.next(), "reopened BOOLEAN table returned an unexpected extra row");
}

void test_boolean_columns_across_null_mask_bytes(const TemporaryDirectory& directory) {
    const auto path = directory.file("boolean-wide-rows.mdb");
    rea_jackcess::TableBuilder table_builder("Wide");
    for (int index = 0; index < 8; ++index) {
        table_builder.add_column("C" + std::to_string(index), rea_jackcess::DataType::int32);
    }
    table_builder.add_column("Flag", rea_jackcess::DataType::boolean)
        .add_column("Tail", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Wide";
        });
        check(table != tables.end() && table->columns().size() == 10,
              "read-write database did not expose the wide BOOLEAN table");
        (void)table->add_rows({
            {std::int32_t{10}, std::int32_t{11}, std::int32_t{12}, std::int32_t{13},
             std::int32_t{14}, std::int32_t{15}, std::int32_t{16}, std::int32_t{17}, true,
             std::int32_t{99}},
            {std::int32_t{20}, std::int32_t{21}, std::int32_t{22}, std::int32_t{23},
             std::int32_t{24}, std::int32_t{25}, std::int32_t{26}, std::int32_t{27}, false,
             std::int32_t{98}},
        });
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Wide";
    });
    check(table != tables.end(), "reopened database did not expose the wide BOOLEAN table");
    auto rows = table->rows();
    const auto first = rows.next();
    check(first && std::get<bool>(first->at(8)) && std::get<std::int32_t>(first->at(9)) == 99 &&
              std::get<std::int32_t>(first->at(0)) == 10,
          "a BOOLEAN in the second NULL mask byte did not read back as TRUE");
    const auto second = rows.next();
    check(second && !std::get<bool>(second->at(8)) && std::get<std::int32_t>(second->at(9)) == 98,
          "a FALSE BOOLEAN in the second NULL mask byte set an unexpected bit");
    check(!rows.next(), "wide BOOLEAN table returned an unexpected extra row");
}

void test_boolean_only_table(const TemporaryDirectory& directory) {
    const auto path = directory.file("boolean-only-rows.mdb");
    rea_jackcess::TableBuilder table_builder("OnlyFlag");
    table_builder.add_column("Flag", rea_jackcess::DataType::boolean);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "OnlyFlag";
        });
        check(table != tables.end(), "read-write database did not expose table OnlyFlag");
        (void)table->add_rows({{true}, {false}, {std::monostate{}}});
        check(table->row_count_header() == 3, "boolean-only table did not accept three rows");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "OnlyFlag";
    });
    check(table != tables.end(), "reopened database did not expose table OnlyFlag");
    const std::vector<bool> expected{true, false, false};
    auto rows = table->rows();
    for (const bool value : expected) {
        const auto row = rows.next();
        check(row && std::get<bool>(row->at(0)) == value,
              "boolean-only table did not preserve its bit values");
    }
    check(!rows.next(), "boolean-only table returned an unexpected extra row");
}

void test_boolean_with_long_value_columns(const TemporaryDirectory& directory) {
    const auto path = directory.file("boolean-long-value-rows.mdb");
    rea_jackcess::TableBuilder table_builder("Mixed");
    table_builder.add_column("Flag", rea_jackcess::DataType::boolean)
        .add_column("Memo", rea_jackcess::DataType::memo)
        .add_column("Tail", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Mixed";
        });
        check(table != tables.end(), "read-write database did not expose table Mixed");
        const auto row_ids = table->add_rows({
            {true, rea_jackcess::MemoValue{std::string(12000, 'M'), {}}, std::int32_t{7}},
            {false, rea_jackcess::MemoValue{"", {}}, std::int32_t{-7}},
        });
        table->update_row(row_ids.front(),
                          {false, rea_jackcess::MemoValue{std::string(20000, 'U'), {}},
                           std::int32_t{7}});
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Mixed";
    });
    check(table != tables.end(), "reopened database did not expose table Mixed");
    auto rows = table->rows();
    const auto first = rows.next();
    check(first && !std::get<bool>(first->at(0)) &&
              std::get<rea_jackcess::MemoValue>(first->at(1)).text.size() == 20000 &&
              std::get<std::int32_t>(first->at(2)) == 7,
          "a BOOLEAN next to a MEMO column corrupted the long-value layout");
    const auto second = rows.next();
    check(second && !std::get<bool>(second->at(0)) &&
              std::get<rea_jackcess::MemoValue>(second->at(1)).text.empty(),
          "a FALSE BOOLEAN next to an empty MEMO did not read back correctly");
    check(!rows.next(), "mixed BOOLEAN/MEMO table returned an unexpected extra row");
}

void test_general_legacy_key_encoding() {
    using rea_jackcess::detail::encode_jet4_general_legacy_key;
    using Bytes = std::vector<std::uint8_t>;
    check(encode_jet4_general_legacy_key(u"A  ", false) == Bytes({0x7f, 0x4a, 1, 0}),
          "General Legacy did not fold case and trim trailing spaces");
    check(encode_jet4_general_legacy_key(u"\u00e1", true) == Bytes({0x80, 0xb5, 0xfe, 0xf1, 0xff, 0}),
          "General Legacy descending accent key differed from Jackcess");
    check(encode_jet4_general_legacy_key(u"'", false) == Bytes({0x7f, 1, 1, 1, 1, 0x80, 7, 6, 0x80, 0}),
          "General Legacy punctuation key differed from Jackcess");
    check(encode_jet4_general_legacy_key(u"\u0301", false) == Bytes({0x7f, 1, 0x0c, 0}),
          "General Legacy combining mark key differed from Jackcess");
    check(encode_jet4_general_legacy_key(u"\u3042", false) ==
              Bytes({0x7f, 0x7f, 2, 1, 1, 1, 0xff, 2, 0x80, 0xff, 0x80, 0}),
          "General Legacy kana key differed from Jackcess");
    check(encode_jet4_general_legacy_key(u"\U0001f680", false) == Bytes({0x7f, 1, 0}),
          "General Legacy assigned a weight to surrogate code units");
}

void test_text_sort_order(const TemporaryDirectory& directory) {
    const auto path = directory.file("text-sort-order.mdb");
    rea_jackcess::TableBuilder builder("SortOrder");
    builder.add_column("Label", rea_jackcess::DataType::text)
        .add_column("Payload", rea_jackcess::DataType::binary);
    (void)rea_jackcess::Database::create(
        path, rea_jackcess::DatabaseFormat::v2000, {builder});

    const auto check_order = [&](std::uint16_t lcid, std::uint8_t version) {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
        const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [](const auto& item) {
            return item.name == "SortOrder";
        });
        check(table != catalog.tables.end(), "sort order fixture did not expose its table");
        const auto& columns = table->definition.columns;
        const auto& order = columns.at(0).text_sort_order;
        check(order && order->lcid == lcid && order->version == version,
              "Jet4 TEXT column sort order was decoded incorrectly");
        check(!columns.at(1).text_sort_order, "BINARY column reported a text sort order");
    };

    constexpr std::uint64_t first_column = 63;
    mutate_table_definition_u16(path, "SortOrder", first_column + 11, 0);
    mutate_table_definition_u16(path, "SortOrder", first_column + 13, 0x0100);
    check_order(1033, 0);
    mutate_table_definition_u16(path, "SortOrder", first_column + 11, 1055);
    check_order(1055, 1);
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

void test_memo_and_ole_long_value_writes(const TemporaryDirectory& directory,
                                        rea_jackcess::DatabaseFormat format) {
    const auto path = directory.file(format == rea_jackcess::DatabaseFormat::v2000
                                         ? "long-values-v2000.mdb"
                                         : "long-values-v2003.mdb");
    rea_jackcess::TableBuilder table_builder("LongValues");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Memo", rea_jackcess::DataType::memo)
        .add_column("Note", rea_jackcess::DataType::text)
        .add_column("Payload", rea_jackcess::DataType::ole);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            format,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    std::vector<std::uint8_t> initial_payload(5000, 0xa5);
    std::vector<std::uint8_t> updated_payload(8192, 0x6c);
    const std::string initial_memo(12000, 'M');
    const std::string updated_memo(20000, 'U');
    std::vector<rea_jackcess::RowId> row_ids;
    std::set<std::uint64_t> original_memo_pages;
    std::set<std::uint64_t> original_ole_pages;
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

        row_ids = table->add_rows({
            {rea_jackcess::Value{std::int32_t{1}},
             rea_jackcess::Value{rea_jackcess::MemoValue{initial_memo, {}}},
             rea_jackcess::Value{std::string("tail")},
             rea_jackcess::Value{rea_jackcess::BlobValue{initial_payload}}},
        });
        check(row_ids.size() == 1, "initial Memo/OLE row was not appended");
        original_memo_pages = read_long_value_owned_pages(path, "LongValues", "Memo");
        original_ole_pages = read_long_value_owned_pages(path, "LongValues", "Payload");
        check(!original_memo_pages.empty() && !original_ole_pages.empty(),
              "initial Memo/OLE values did not allocate long-value pages");

        const auto additional_row_ids = table->add_rows({
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
        row_ids.insert(row_ids.end(), additional_row_ids.begin(), additional_row_ids.end());
        check(row_ids.size() == 4 && table->row_count_header() == 4,
              "Memo/OLE append did not preserve row ids and row count");

        table->update_row(row_ids.front(),
                          {rea_jackcess::Value{std::int32_t{1}},
                           rea_jackcess::Value{rea_jackcess::MemoValue{updated_memo, {}}},
                           rea_jackcess::Value{std::string("tail")},
                           rea_jackcess::Value{rea_jackcess::BlobValue{updated_payload}}});

        const auto memo_pages_after_update = read_long_value_owned_pages(path, "LongValues", "Memo");
        const auto ole_pages_after_update = read_long_value_owned_pages(path, "LongValues", "Payload");
        const auto memo_free_pages_after_update = read_long_value_free_pages(path, "LongValues", "Memo");
        const auto ole_free_pages_after_update = read_long_value_free_pages(path, "LongValues", "Payload");
        std::set<std::uint64_t> released_memo_pages;
        std::set<std::uint64_t> retained_memo_pages;
        for (const auto page : original_memo_pages) {
            if (memo_pages_after_update.count(page) == 0) {
                released_memo_pages.insert(page);
                check(memo_free_pages_after_update.count(page) == 0,
                      "an unreferenced Memo page remained in the column free-space usage map");
                check(global_page_is_free(path, page),
                      "an unreferenced Memo page was not returned to the global usage map");
            } else {
                retained_memo_pages.insert(page);
                check(!global_page_is_free(path, page),
                      "a Memo page still referenced by another row was marked globally free");
            }
        }
        check(!released_memo_pages.empty() && !retained_memo_pages.empty(),
              "Memo update did not reclaim unreferenced pages while preserving a shared page");
        for (const auto page : original_ole_pages) {
            check(ole_pages_after_update.count(page) == 0 && ole_free_pages_after_update.count(page) == 0 &&
                      global_page_is_free(path, page),
                  "OLE update did not reclaim an unreferenced long-value page");
        }

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

    const auto memo_pages_before_delete = read_long_value_owned_pages(path, "LongValues", "Memo");
    const auto ole_pages_before_delete = read_long_value_owned_pages(path, "LongValues", "Payload");
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto writable_tables = writable.tables();
        const auto writable_table = std::find_if(writable_tables.begin(), writable_tables.end(), [](const auto& item) {
            return item.name() == "LongValues";
        });
        check(writable_table != writable_tables.end(), "writable database lost the Memo/OLE table");
        writable_table->delete_row(row_ids.front());
    }
    const auto memo_pages_after_delete = read_long_value_owned_pages(path, "LongValues", "Memo");
    const auto ole_pages_after_delete = read_long_value_owned_pages(path, "LongValues", "Payload");
    const auto memo_free_pages_after_delete = read_long_value_free_pages(path, "LongValues", "Memo");
    const auto ole_free_pages_after_delete = read_long_value_free_pages(path, "LongValues", "Payload");
    check(memo_pages_after_delete.size() < memo_pages_before_delete.size() && ole_pages_after_delete.empty(),
          "deleting a row did not release its unreferenced long-value pages");
    for (const auto page : memo_pages_before_delete) {
        if (memo_pages_after_delete.count(page) == 0) {
            check(memo_free_pages_after_delete.count(page) == 0,
                  "a deleted row's unreferenced Memo page remained in the column free-space usage map");
            check(global_page_is_free(path, page),
                  "a deleted row's unreferenced Memo page was not marked globally free");
        }
    }
    for (const auto page : ole_pages_before_delete) {
        check(ole_pages_after_delete.count(page) == 0 && ole_free_pages_after_delete.count(page) == 0,
              "a deleted row's unreferenced OLE page remained in a column usage map");
        check(global_page_is_free(path, page),
              "a deleted row's unreferenced OLE page was not marked globally free");
    }
}

void test_read_error_handler(const TemporaryDirectory& directory,
                             rea_jackcess::DatabaseFormat format) {
    namespace detail = rea_jackcess::detail;
    using namespace rea_jackcess;
    const auto path = directory.file(format == DatabaseFormat::v2000
                                         ? "read-error-handler-v2000.mdb"
                                         : "read-error-handler-v2003.mdb");
    Database::create(path, format,
                     {TableBuilder("BrokenMemo")
                          .add_column("Id", DataType::int32)
                          .add_column("Memo", DataType::memo)
                          .add_index(IndexBuilder("ById", "Id"))});
    RowId expected_row_id{};
    std::vector<std::uint8_t> corrupted_descriptor;
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = get_table(database, "BrokenMemo");
        expected_row_id = table.add_row({std::int32_t{7}, MemoValue{std::string(12000, 'M'), {}}});
    }

    {
        const auto file = detail::PageFile::open(path);
        const auto catalog = detail::read_table_schema_catalog(file);
        const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [](const auto& item) {
            return item.name == "BrokenMemo";
        });
        check(table != catalog.tables.end(), "read-error fixture lost its table definition");
        detail::TableRowReader reader(file, table->name, table->definition);
        const auto row = reader.next();
        const auto memo_column = std::find_if(table->definition.columns.begin(),
                                              table->definition.columns.end(), [](const auto& item) {
            return item.name == "Memo";
        });
        check(row && memo_column != table->definition.columns.end() &&
                  row->page_number == expected_row_id.page_number &&
                  row->row_number == expected_row_id.row_number,
              "read-error fixture could not locate its row and Memo column");
        const auto& descriptor = row->columns.at(memo_column->column_number).bytes;
        check(descriptor && descriptor->size() == 12,
              "read-error fixture did not create an external Memo descriptor");
        corrupted_descriptor = *descriptor;
        corrupted_descriptor[5] = 0xff;
        corrupted_descriptor[6] = 0xff;
        corrupted_descriptor[7] = 0xff;

        auto page = file.read_page(row->page_number);
        const auto data_page = detail::read_data_page(file, row->page_number);
        const auto& physical_row = data_page.rows.at(row->row_number);
        const auto first = page.begin() + physical_row.start_offset;
        const auto last = page.begin() + physical_row.end_offset;
        const auto descriptor_position = std::search(first, last, descriptor->begin(), descriptor->end());
        check(descriptor_position != last,
              "read-error fixture could not locate the Memo descriptor bytes in its data page");
        const auto offset = static_cast<std::size_t>(descriptor_position - page.begin());
        std::copy(corrupted_descriptor.begin(), corrupted_descriptor.end(),
                  page.begin() + static_cast<std::ptrdiff_t>(offset));
        auto writer = detail::PageFileWriter::open(path);
        writer.write_page(row->page_number, page);
        writer.flush();
    }

    auto database = Database::open(path);
    auto table = get_table(database, "BrokenMemo");
    try {
        (void)table.rows().next();
        throw std::runtime_error("default row error handling accepted a broken Memo pointer");
    } catch (const DatabaseError& error) {
        check(error.code() == DatabaseErrorCode::corrupted_file &&
                  error.context().table_name == "BrokenMemo" &&
                  error.context().column_name == "Memo" &&
                  error.context().row_number == expected_row_id.row_number,
              "default row error handling lost corruption context");
    }

    const auto verify_replacement = [&](std::string expected_text) {
        ErrorHandler handler = [&, expected_text = std::move(expected_text)](
                                   std::string_view table_name,
                                   const ColumnInfo& column,
                                   const std::optional<std::vector<std::uint8_t>>& column_data,
                                   RowId row_id,
                                   const std::exception& error) -> Value {
            check(table_name == "BrokenMemo" && column.name == "Memo" &&
                      column.type == DataType::memo && column_data &&
                      *column_data == corrupted_descriptor && row_id == expected_row_id &&
                      std::string(error.what()).find("Long-value pointer references an invalid page") !=
                          std::string::npos,
                  "row error handler did not receive column data and location context");
            return MemoValue{expected_text, {}};
        };
        return handler;
    };

    database.set_error_handler(verify_replacement("database replacement"));
    auto database_row = table.rows().next();
    check(database_row && std::get<MemoValue>(database_row->at(1)).text == "database replacement",
          "database error handler did not replace a bad column value");
    const auto indexed_row = table.find_row_by_index("ById", std::int32_t{7});
    check(indexed_row && std::get<MemoValue>(indexed_row->at(1)).text == "database replacement",
          "indexed row lookup bypassed the database error handler");

    table.set_error_handler(verify_replacement("table replacement"));
    auto table_row = table.rows().next();
    check(table_row && std::get<MemoValue>(table_row->at(1)).text == "table replacement",
          "table error handler did not override the database handler");

    auto cursor = table.rows();
    cursor.set_error_handler(verify_replacement("cursor replacement"));
    auto cursor_row = cursor.next();
    check(cursor_row && std::get<MemoValue>(cursor_row->at(1)).text == "cursor replacement",
          "cursor error handler did not override the table handler");

    cursor.set_error_handler({});
    auto inherited_row = table.rows().next();
    check(inherited_row && std::get<MemoValue>(inherited_row->at(1)).text == "table replacement",
          "clearing a cursor error handler did not restore table-level inheritance");
    table.set_error_handler({});
    auto reset_table_row = table.rows().next();
    check(reset_table_row && std::get<MemoValue>(reset_table_row->at(1)).text == "database replacement",
          "clearing a table error handler did not restore database-level inheritance");
}

void test_long_value_release_after_global_map_promotion(const TemporaryDirectory& directory,
                                                        rea_jackcess::DatabaseFormat format) {
    namespace detail = rea_jackcess::detail;
    const auto path = directory.file(format == rea_jackcess::DatabaseFormat::v2000
                                         ? "long-value-global-promotion-v2000.mdb"
                                         : "long-value-global-promotion-v2003.mdb");
    rea_jackcess::TableBuilder builder("LongValuePromotion");
    builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Memo", rea_jackcess::DataType::memo);
    (void)rea_jackcess::Database::create(path, format, {builder});

    const std::string initial_value(12000, 'A');
    const std::string replacement_value(20000, 'B');
    rea_jackcess::RowId row_id{};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(writable, "LongValuePromotion");
        row_id = table.add_row({std::int32_t{1}, rea_jackcess::MemoValue{initial_value, {}}});
    }

    const auto original_pages = read_long_value_owned_pages(path, "LongValuePromotion", "Memo");
    check(!original_pages.empty(), "global-map promotion fixture did not allocate old Memo pages");
    std::uint64_t frontier = 0;
    std::uint64_t page_size = 0;
    {
        const auto file = detail::PageFile::open(path);
        page_size = file.format().page_size;
        const auto declaration = detail::parse_data_page(file, 1, file.read_page(1));
        const auto& global_map = declaration.rows.at(0).bytes;
        check(global_map.at(0) == 0, "global-map promotion fixture did not start with an inline map");
        const auto first_page = detail::read_u32(global_map, 1);
        frontier = first_page + (global_map.size() - 5) * 8;
        check(frontier > file.page_count(), "global-map promotion boundary is already in the file");
    }
    // Make the replacement LVAL pages cross the inline global-map boundary.
    std::filesystem::resize_file(path, frontier * page_size);

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(writable, "LongValuePromotion");
        table.update_row(row_id, {std::int32_t{1}, rea_jackcess::MemoValue{replacement_value, {}}});
    }

    const auto file = detail::PageFile::open(path);
    const auto declaration = detail::parse_data_page(file, 1, file.read_page(1));
    check(declaration.rows.at(0).bytes.at(0) == 1,
          "long-value update did not promote the global map to reference bitmaps");
    const auto owned_after_update = read_long_value_owned_pages(path, "LongValuePromotion", "Memo");
    const auto free_after_update = read_long_value_free_pages(path, "LongValuePromotion", "Memo");
    for (const auto page : original_pages) {
        check(owned_after_update.count(page) == 0 && free_after_update.count(page) == 0 &&
                  global_page_is_free(path, page),
              "global-map promotion did not free a replaced Memo page in every usage map");
    }
    auto reopened = rea_jackcess::Database::open(path);
    const auto table = get_table(reopened, "LongValuePromotion");
    auto rows = table.rows();
    const auto row = rows.next();
    check(row && row->id() == row_id &&
              std::get<rea_jackcess::MemoValue>(row->at(1)).text == replacement_value && !rows.next(),
          "global-map promotion changed the updated Memo row");
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

void test_index_key_types_and_rejections(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-key-types.mdb");
    rea_jackcess::TableBuilder table_builder("KeyTypes");
    table_builder.add_column("Flag", rea_jackcess::DataType::boolean)
        .add_column("Small", rea_jackcess::DataType::int16)
        .add_column("Ratio", rea_jackcess::DataType::float64)
        .add_index(rea_jackcess::IndexBuilder("IX_Flag", "Flag"))
        .add_index(rea_jackcess::IndexBuilder("IX_Small", "Small"))
        .add_index(rea_jackcess::IndexBuilder("IX_Ratio", "Ratio"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "KeyTypes";
        });
        check(table != tables.end(), "read-write database did not expose table KeyTypes");
        (void)table->add_rows({
            {true, std::int16_t{2}, 1.5},
            {false, std::int16_t{1}, -2.5},
            {std::monostate{}, std::monostate{}, std::monostate{}},
        });
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "KeyTypes";
    });
    check(table != tables.end() && table->row_count_header() == 3,
          "reopened key-type table has an unexpected row count");
    const auto expect_order = [&](const std::string& index_name,
                                  const std::vector<bool>& expected_flags) {
        auto cursor = table->rows_by_index(index_name);
        for (const bool flag : expected_flags) {
            const auto row = cursor.next();
            check(row && std::get<bool>(row->at(0)) == flag,
                  "index " + index_name + " did not return the expected key order");
        }
        check(!cursor.next(), "index " + index_name + " returned an unexpected extra entry");
    };
    // A NULL BOOLEAN is stored as an unset mask bit, so it reads back as FALSE and sorts with
    // the FALSE keys; TRUE (0x7f 0x00) sorts before FALSE (0x7f 0xff).
    expect_order("IX_Flag", {true, false, false});
    const auto expect_small = [&](const std::string& index_name,
                                  const std::vector<int>& expected_ids) {
        auto cursor = table->rows_by_index(index_name);
        for (const int id : expected_ids) {
            const auto row = cursor.next();
            check(static_cast<bool>(row), "index " + index_name + " returned too few entries");
            const auto small = std::get_if<std::int16_t>(&row->at(1));
            if (id == 0) {
                check(small == nullptr, "index " + index_name + " did not start with its NULL key");
            } else {
                check(small != nullptr && *small == static_cast<std::int16_t>(id),
                      "index " + index_name + " did not return the expected key order");
            }
        }
        check(!cursor.next(), "index " + index_name + " returned an unexpected extra entry");
    };
    expect_small("IX_Small", {0, 1, 2});
    {
        auto cursor = table->rows_by_index("IX_Ratio");
        const auto null_row = cursor.next();
        check(null_row && std::holds_alternative<std::monostate>(null_row->at(2)),
              "DOUBLE index did not start with its NULL key");
        const auto negative = cursor.next();
        check(negative && std::get<double>(negative->at(2)) == -2.5,
              "DOUBLE index did not order negative keys first");
        const auto positive = cursor.next();
        check(positive && std::get<double>(positive->at(2)) == 1.5,
              "DOUBLE index did not order positive keys after negative ones");
        check(!cursor.next(), "DOUBLE index returned an unexpected extra entry");
    }
}

rea_jackcess::Table get_table(const rea_jackcess::Database& database, const std::string& name) {
    const auto tables = database.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    check(table != tables.end(), "database did not expose " + name);
    return *table;
}

void test_index_null_constraints(const TemporaryDirectory& directory) {
    using rea_jackcess::Value;
    for (const std::uint16_t flags : {0x02, 0x08, 0x0a, 0x00}) {
        const auto path = directory.file("index-constraint-" + std::to_string(flags) + ".mdb");
        rea_jackcess::TableBuilder builder("Constrained");
        rea_jackcess::IndexBuilder index("IX_Id", "Id", false, true);
        index.with_required((flags & 0x08) != 0).with_ignore_nulls((flags & 0x02) != 0);
        if (flags == 0) {
            index.with_primary_key();
        }
        builder.add_column("Id", rea_jackcess::DataType::int32).add_index(index);
        (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});

        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "Constrained");
        const auto id = table.add_row({std::int32_t{17}});
        if ((flags & 0x02) != 0) {
            const auto ignored = table.add_row({Value{}});
            const auto removed = table.add_row({Value{}});
            table.delete_row(removed);
            check(!table.find_row_by_index("IX_Id", Value{}),
                  "IgnoreNulls index exposes an omitted key");
            table.update_row(ignored, {std::int32_t{18}});
            table.update_row(id, {Value{}});
            const auto hit = table.find_row_by_index("IX_Id", std::int32_t{18});
            check(hit && hit->id() == ignored && !table.find_row_by_index("IX_Id", std::int32_t{17}),
                  "IgnoreNulls index did not maintain both NULL transitions");
            auto scan = table.rows_by_index("IX_Id");
            check(scan.next().has_value() && !scan.next() && table.row_count_header() == 2,
                  "IgnoreNulls changed the table row count or included a NULL entry");
        } else {
            const auto before = read_bytes(path);
            expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
                (void)table.add_rows({{std::int32_t{18}}, {Value{}}});
            });
            expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
                table.update_row(id, {Value{}});
            });
            if (flags == 0) {
                expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
                    (void)table.add_row({std::int32_t{17}});
                });
            }
            check(read_bytes(path) == before && table.row_count_header() == 1,
                  "an index constraint failure changed the file");
        }
    }
}

void test_create_composite_primary_key(const TemporaryDirectory& directory) {
    using rea_jackcess::IndexBuilder;
    using rea_jackcess::IndexColumnBuilder;
    using rea_jackcess::Value;
    const auto path = directory.file("composite-primary-key.mdb");
    rea_jackcess::TableBuilder builder("PrimaryTable");
    builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("No", rea_jackcess::DataType::int32)
        .add_index(IndexBuilder("IX_Pk", std::vector<IndexColumnBuilder>{{"Id"}, {"No", false}})
                       .with_primary_key());
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2003, {builder});
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "PrimaryTable");
        const auto& index = table.indexes().at(0);
        check(index.primary_key && index.unique && index.required && index.columns.size() == 2 &&
                  index.columns[0].ascending && !index.columns[1].ascending,
              "composite primary key metadata did not persist");
        const auto ids = table.add_rows({{1, 2}, {1, 3}});
        const auto before = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)table.add_row({1, Value{}});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
            (void)table.add_row({1, 2});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            table.update_row(ids[0], {Value{}, 2});
        });
        check(read_bytes(path) == before, "primary key failure changed the file");
        table.update_row(ids[0], {2, 2});
        const auto found = table.find_row_by_index("IX_Pk", std::vector<Value>{2, 2});
        check(found && found->id() == ids[0], "primary key update lost its row");
    }
    builder.add_index(IndexBuilder("SecondKey", "No").with_primary_key());
    const auto rejected = directory.file("multiple-primary-keys.mdb");
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)rea_jackcess::Database::create(rejected, rea_jackcess::DatabaseFormat::v2000, {builder});
    });
    check(!std::filesystem::exists(rejected), "a table with two primary keys was published");
}

void test_composite_index_maintenance(const TemporaryDirectory& directory,
                                      rea_jackcess::DatabaseFormat format = rea_jackcess::DatabaseFormat::v2000) {
    using rea_jackcess::Value;
    const auto path = directory.file(format == rea_jackcess::DatabaseFormat::v2000
                                         ? "composite-maintenance.mdb" : "composite-maintenance-v2003.mdb");
    rea_jackcess::TableBuilder builder("Composite");
    builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("No", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder(
            "IX_Pair", std::vector<rea_jackcess::IndexColumnBuilder>{{"Id"}, {"No", false}}, true));
    (void)rea_jackcess::Database::create(path, format, {builder});
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "Composite");
        check(table.indexes().at(0).columns.size() == 2 &&
                  !table.indexes().at(0).columns.at(1).ascending, "invalid composite fixture");
        const auto ids = table.add_rows({
            {2, 1}, {1, 2}, {1, 3}, {Value{}, 9}, {Value{}, Value{}}, {Value{}, Value{}},
        });
        const auto before = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
            (void)table.add_row({Value{}, 9});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
            table.update_row(ids[0], {1, 2});
        });
        check(read_bytes(path) == before, "composite unique rejection changed the file");
        table.update_row(ids[0], {1, 4});
        table.delete_row(ids[4]);
        table.delete_row(ids[2]);
    }

    const auto db = rea_jackcess::Database::open(path);
    const auto table = get_table(db, "Composite");
    auto rows = table.rows_by_index("IX_Pair");
    const auto part_null = rows.next();
    const auto all_null = rows.next();
    const auto high = rows.next();
    const auto low = rows.next();
    check(part_null && all_null && high && low && !rows.next() &&
              std::holds_alternative<std::monostate>(part_null->at(0)) &&
              std::get<std::int32_t>(part_null->at(1)) == 9 &&
              std::holds_alternative<std::monostate>(all_null->at(0)) &&
              std::holds_alternative<std::monostate>(all_null->at(1)) &&
              std::get<std::int32_t>(high->at(0)) == 1 && std::get<std::int32_t>(high->at(1)) == 4 &&
              std::get<std::int32_t>(low->at(0)) == 1 && std::get<std::int32_t>(low->at(1)) == 2,
          "composite update or delete produced the wrong index order");

    const auto hit = table.find_row_by_index("IX_Pair", std::vector<Value>{1, 4});
    check(hit && hit->id() == high->id() &&
              !table.find_row_by_index("IX_Pair", std::vector<Value>{1, 3}),
          "composite exact lookup returned the wrong row");
    auto seek = table.rows_by_index_from("IX_Pair", std::vector<Value>{1, 3});
    const auto after_missing = seek.next();
    check(after_missing && after_missing->id() == low->id(), "composite seek used the wrong direction");
    auto range = table.rows_by_index_range("IX_Pair", std::vector<Value>{Value{}, Value{}},
                                           std::vector<Value>{1, 2});
    const auto range_start = range.next();
    const auto range_end = range.next();
    check(range_start && range_start->id() == all_null->id() &&
              range_end && range_end->id() == high->id() && !range.next(),
          "composite bounds did not use a half-open range");
    auto reverse = table.rows_by_index_range_reverse("IX_Pair", std::vector<Value>{Value{}, Value{}},
                                                     std::vector<Value>{1, 2});
    const auto reverse_first = reverse.next();
    const auto reverse_second = reverse.next();
    check(reverse_first && reverse_first->id() == high->id() &&
              reverse_second && reverse_second->id() == all_null->id() && !reverse.next(),
          "reverse composite range did not use the same bounds");
    auto prefix = table.rows_by_index_prefix("IX_Pair", {1}, true);
    const auto first_match = prefix.next();
    const auto second_match = prefix.next();
    check(first_match && first_match->id() == low->id() &&
              second_match && second_match->id() == high->id() && !prefix.next(),
          "reverse composite prefix scan matched the wrong rows");
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)table.find_row_by_index("IX_Pair", std::vector<Value>{1});
    });
}

void test_binary_index_prefix(const TemporaryDirectory& directory) {
    using rea_jackcess::BlobValue;
    using rea_jackcess::Value;
    const auto path = directory.file("binary-index-prefix.mdb");
    rea_jackcess::TableBuilder builder("BinaryPrefix");
    builder.add_column("Payload", rea_jackcess::DataType::binary)
        .add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder(
            "IX_Pair", std::vector<rea_jackcess::IndexColumnBuilder>{{"Payload", false}, {"Id"}}));
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        (void)get_table(db, "BinaryPrefix").add_rows({
            {BlobValue{{}}, 1}, {BlobValue{{0x01}}, 2}, {BlobValue{{}}, 3},
            {Value{}, 4}, {Value{}, 5},
        });
    }
    const auto db = rea_jackcess::Database::open(path);
    const auto table = get_table(db, "BinaryPrefix");
    auto empty = table.rows_by_index_prefix("IX_Pair", {BlobValue{{}}});
    const auto first = empty.next();
    const auto second = empty.next();
    check(first && std::get<std::int32_t>(first->at(1)) == 1 &&
              second && std::get<std::int32_t>(second->at(1)) == 3 && !empty.next(),
          "empty BINARY prefix matched a nonempty BINARY key");
    auto nulls = table.rows_by_index_prefix("IX_Pair", {Value{}}, true);
    const auto last = nulls.next();
    const auto previous = nulls.next();
    check(last && std::get<std::int32_t>(last->at(1)) == 5 &&
              previous && std::get<std::int32_t>(previous->at(1)) == 4 && !nulls.next(),
          "reverse descending NULL prefix did not scan its unbounded upper range");
}

void test_invalid_composite_index_definitions(const TemporaryDirectory& directory) {
    const auto path = directory.file("invalid-composite.mdb");
    using IndexColumns = std::vector<rea_jackcess::IndexColumnBuilder>;
    for (const auto& columns : {IndexColumns{}, IndexColumns{{"Id"}, {"Id"}},
                                IndexColumns{{"Id"}, {"Missing"}}, IndexColumns(11, {"Id"})}) {
        rea_jackcess::TableBuilder builder("InvalidComposite");
        builder.add_column("Id", rea_jackcess::DataType::int32)
            .add_index(rea_jackcess::IndexBuilder("IX_Pair", columns));
        expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
            (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});
        });
        check(!std::filesystem::exists(path), "invalid composite index was published");
    }
}

void test_text_index_keys(const TemporaryDirectory& directory) {
    using rea_jackcess::Value;
    const auto path = directory.file("index-text-key.mdb");
    rea_jackcess::TableBuilder builder("TextIndexed");
    builder.add_column("Name", rea_jackcess::DataType::text)
        .add_index(rea_jackcess::IndexBuilder("IX_Asc", "Name", true))
        .add_index(rea_jackcess::IndexBuilder("IX_Desc", "Name", true, true));
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2003, {builder});
    const Value apple = std::string("Apple");
    const Value accented = std::string("\xc3\x81pple");
    const Value banana = std::string("banana");
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "TextIndexed");
        const auto ids = table.add_rows({{banana}, {accented}, {apple}, {Value{}}, {Value{}}});
        const auto before = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
            (void)table.add_row({std::string("APPLE   ")});
        });
        check(read_bytes(path) == before, "TEXT unique rejection modified the file");
        table.update_row(ids[2], {std::string("APPLE ")});
        table.update_row(ids[0], {std::string("Cherry")});
        table.delete_row(ids[1]);
    }
    const auto reopened = rea_jackcess::Database::open(path);
    const auto table = get_table(reopened, "TextIndexed");
    for (const char* name : {"IX_Asc", "IX_Desc"}) {
        const auto row = table.find_row_by_index(name, apple);
        check(row && std::get<std::string>(row->at(0)) == "APPLE " &&
                  !table.find_row_by_index(name, accented) && !table.find_row_by_index(name, banana),
              "TEXT index lookup missed its equivalent key or found a stale key");
    }
    auto range = table.rows_by_index_range_reverse("IX_Desc", std::string("CHERRY"), Value{});
    const auto first = range.next();
    const auto second = range.next();
    check(first && second && std::get<std::string>(first->at(0)) == "APPLE " &&
              std::get<std::string>(second->at(0)) == "Cherry" && !range.next(),
          "descending TEXT index reverse range did not respect its bounds");

    constexpr std::uint64_t first_column = 63 + 2 * 12;
    mutate_table_definition_u16(path, "TextIndexed", first_column + 13, 0);
    mutate_table_definition_u16(path, "TextIndexed", first_column + 11, 1039);
    auto other_order = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
    auto other_table = get_table(other_order, "TextIndexed");
    const auto before = read_bytes(path);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)other_table.find_row_by_index("IX_Asc", apple);
    });
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)other_table.add_row({apple});
    });
    check(read_bytes(path) == before, "Unsupported TEXT sort order modified the file");
}

void test_text_key_code_tables() {
    using rea_jackcess::detail::encode_jet4_general97_key;
    using rea_jackcess::detail::encode_jet4_general_key;
    using rea_jackcess::detail::encode_jet4_general_legacy_key;

    check(encode_jet4_general_legacy_key(u"a", false) ==
              std::vector<std::uint8_t>{0x7f, 0x4a, 0x01, 0x00},
          "General Legacy text key changed while adding other code tables");
    check(encode_jet4_general_key(u"a", false) ==
              std::vector<std::uint8_t>{0x7f, 0x0e, 0x02, 0x01, 0x00},
          "General text key did not use the pinned version 1 weights");

    const auto surrogate_pair = [](std::uint32_t code_point) {
        code_point -= 0x10000;
        return std::u16string{
            static_cast<char16_t>(0xd800 + (code_point >> 10)),
            static_cast<char16_t>(0xdc00 + (code_point & 0x3ff)),
        };
    };
    const auto general_surrogate_key = [&](std::uint32_t code_point) {
        return encode_jet4_general_key(surrogate_pair(code_point) + u"aa", false);
    };
    check(general_surrogate_key(0x10000) ==
              std::vector<std::uint8_t>{0x7f, 0xb0, 0x02, 0xb4, 0xf8, 0x0e, 0x02, 0x0e,
                                        0x02, 0x01, 0x3f, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess plane 1 surrogate vector");
    check(general_surrogate_key(0x1f600) ==
              std::vector<std::uint8_t>{0x7f, 0xb0, 0x3f, 0xb6, 0xfc, 0x0e, 0x02, 0x0e,
                                        0x02, 0x01, 0x3f, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess surrogate vector");
    check(general_surrogate_key(0x20000) ==
              std::vector<std::uint8_t>{0x7f, 0xfe, 0x02, 0xb4, 0xf8, 0x0e, 0x02, 0x0e,
                                        0x02, 0x01, 0x3e, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess plane 2 surrogate vector");
    check(general_surrogate_key(0x2ffff) ==
              std::vector<std::uint8_t>{0x7f, 0xfe, 0x41, 0xb8, 0xff, 0x0e, 0x02, 0x0e,
                                        0x02, 0x01, 0x3e, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess plane 2 boundary vector");
    check(general_surrogate_key(0x30000) ==
              std::vector<std::uint8_t>{0x7f, 0xb4, 0xf8, 0x0e, 0x02, 0x0e, 0x02, 0x01,
                                        0x3f, 0x00},
          "General text key did not match the fixed Jackcess unweighted high-surrogate vector");
    check(general_surrogate_key(0xeffff) ==
              std::vector<std::uint8_t>{0x7f, 0xb8, 0xff, 0x0e, 0x02, 0x0e, 0x02, 0x01,
                                        0x3f, 0x00},
          "General text key did not match the fixed Jackcess unweighted boundary vector");
    check(general_surrogate_key(0xf0000) ==
              std::vector<std::uint8_t>{0x7f, 0xff, 0x02, 0xb4, 0xf8, 0x0e, 0x02, 0x0e,
                                        0x02, 0x01, 0x3e, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess high-plane surrogate vector");
    check(general_surrogate_key(0x10ffff) ==
              std::vector<std::uint8_t>{0x7f, 0xff, 0x81, 0xb8, 0xff, 0x0e, 0x02, 0x0e,
                                        0x02, 0x01, 0x3e, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess maximum-code-point vector");
    check(encode_jet4_general_key(u"a" + surrogate_pair(0x20000) + u"a", false) ==
              std::vector<std::uint8_t>{0x7f, 0x0e, 0x02, 0xfe, 0x02, 0xb4, 0xf8, 0x0e,
                                        0x02, 0x01, 0x02, 0x3e, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess middle surrogate vector");
    check(encode_jet4_general_key(u"aa" + surrogate_pair(0x30000) + u"a", false) ==
              std::vector<std::uint8_t>{0x7f, 0x0e, 0x02, 0x0e, 0x02, 0xb4, 0xf8, 0x0e,
                                        0x02, 0x01, 0x02, 0x02, 0x3f, 0x00},
          "General text key did not match the fixed Jackcess unweighted middle vector");

    check(encode_jet4_general_key(u"a", true) ==
              std::vector<std::uint8_t>{0x80, 0xf1, 0xfd, 0xfe, 0xff, 0x00},
          "General descending text key did not invert the pinned key payload");

    check(encode_jet4_general97_key(u"a", false) ==
              std::vector<std::uint8_t>{0x7f, 0x60, 0x00},
          "General 97 significant-character encoding changed");
    check(encode_jet4_general97_key(u"\u00e9", false) ==
              std::vector<std::uint8_t>{0x7f, 0x66, 0x04, 0x00},
          "General 97 extra-nibble encoding changed");
    check(encode_jet4_general97_key(u"\u00e9", true) ==
              std::vector<std::uint8_t>{0x80, 0x99, 0xfb, 0xff},
          "General 97 descending text key did not invert the payload");
    check(encode_jet4_general97_key(u"\u0152", false) ==
              encode_jet4_general97_key(std::u16string{static_cast<char16_t>(0x008c)}, false),
          "General 97 extended-character mapping was not applied");
    check(encode_jet4_general97_key(surrogate_pair(0x1f600), false) ==
              std::vector<std::uint8_t>{0x7f, 0x00},
          "General 97 did not ignore unsupported supplementary characters");
}

void test_supported_text_sort_orders(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;
    const std::vector<TextSortOrder> sort_orders{
        {1033, 0},
        {1033, 1},
        {1033, -1},
        {1049, 0},
        {1055, 0},
        {1058, 0},
    };
    const std::vector<std::string> values{
        "Alpha", "Bravo", "cafe", u8"Éclair", u8"İstanbul",
    };

    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        const auto format_suffix = format == DatabaseFormat::v2000 ? "v2000" : "v2003";
        for (const auto& sort_order : sort_orders) {
            const auto path = directory.file("text-sort-" + std::to_string(sort_order.lcid) + "-" +
                                             std::to_string(static_cast<int>(sort_order.version)) + "-" +
                                             format_suffix + ".mdb");
            ColumnBuilder name_column("Name", DataType::text);
            name_column.with_text_sort_order(sort_order.lcid, sort_order.version);
            TableBuilder builder("LocaleIndex");
            builder.add_column(std::move(name_column))
                .add_index(IndexBuilder("IX_Name", "Name", true))
                .add_index(IndexBuilder("IX_NameDesc", "Name", true, true));
            (void)Database::create(path, format, {builder});

            {
                const auto file = detail::PageFile::open(path);
                const auto catalog = detail::read_table_schema_catalog(file);
                const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [](const auto& item) {
                    return item.name == "LocaleIndex";
                });
                check(table != catalog.tables.end() && table->definition.columns.front().text_sort_order &&
                          table->definition.columns.front().text_sort_order->lcid == sort_order.lcid &&
                          table->definition.columns.front().text_sort_order->version ==
                              static_cast<std::uint8_t>(sort_order.version),
                      "created TEXT column did not preserve its requested sort order");
            }

            std::vector<RowId> row_ids;
            {
                auto writable = Database::open(path, DatabaseOpenMode::read_write);
                auto table = get_table(writable, "LocaleIndex");
                std::vector<std::vector<Value>> rows;
                for (const auto& value : values) {
                    rows.push_back({value});
                }
                row_ids = table.add_rows(rows);
                check(row_ids.size() == values.size(),
                      "supported TEXT sort order did not append every row");
                for (const auto& index_name : {"IX_Name", "IX_NameDesc"}) {
                    for (const auto& value : values) {
                        const auto row = table.find_row_by_index(index_name, Value{value});
                        check(row && std::get<std::string>(row->at(0)) == value,
                              "supported TEXT sort order did not find an inserted row");
                    }
                }
                table.update_row(row_ids.front(), {std::string("Aaron")});
                for (const auto& index_name : {"IX_Name", "IX_NameDesc"}) {
                    check(table.find_row_by_index(index_name, Value{std::string("Aaron")}).has_value() &&
                              !table.find_row_by_index(index_name, Value{values.front()}),
                          "supported TEXT sort order did not replace a changed index key");
                }
                table.delete_row(row_ids.at(1));
                for (const auto& index_name : {"IX_Name", "IX_NameDesc"}) {
                    check(!table.find_row_by_index(index_name, Value{values.at(1)}),
                          "supported TEXT sort order retained a deleted index key");
                }
            }

            const auto reopened = Database::open(path);
            const auto table = get_table(reopened, "LocaleIndex");
            for (const auto& index_name : {"IX_Name", "IX_NameDesc"}) {
                check(table.find_row_by_index(index_name, Value{std::string("Aaron")}).has_value() &&
                          !table.find_row_by_index(index_name, Value{values.front()}) &&
                          !table.find_row_by_index(index_name, Value{values.at(1)}),
                      "supported TEXT sort order index updates did not survive reopening");
            }
        }
    }

    const auto unsupported_path = directory.file("text-sort-1039-unsupported.mdb");
    ColumnBuilder unsupported_column("Name", DataType::text);
    unsupported_column.with_text_sort_order(1039, 0);
    TableBuilder unsupported_builder("UnsupportedLocale");
    unsupported_builder.add_column(std::move(unsupported_column))
        .add_index(IndexBuilder("IX_Name", "Name", true));
    expect_error(DatabaseErrorCode::unsupported_operation, [&] {
        (void)Database::create(unsupported_path, DatabaseFormat::v2000, {unsupported_builder});
    });
    check(!std::filesystem::exists(unsupported_path),
          "unsupported TEXT sort order published a table with an invalid index");
}

void test_memo_index_support(const TemporaryDirectory& directory) {
    using namespace rea_jackcess;

    std::vector<std::uint8_t> digest_input(520);
    for (std::size_t index = 0; index < digest_input.size(); ++index) {
        digest_input[index] = static_cast<std::uint8_t>(index * 37 + 11);
    }
    const auto digest_key = detail::truncate_jet4_index_key(digest_input);
    check(digest_key.size() == detail::jet4_max_index_key_length &&
              std::equal(digest_input.begin(), digest_input.begin() + 508, digest_key.begin()) &&
              digest_key[508] == 0xca && digest_key[509] == 0x4b,
          "long index key truncation did not match Jackcess 5.2.0 CRC-16 bytes");
    const std::vector<std::uint8_t> boundary_key(digest_input.begin(), digest_input.begin() + 510);
    check(detail::truncate_jet4_index_key(boundary_key) == boundary_key,
          "an index key at the 510-byte limit was unnecessarily truncated");

    const std::string long_a = std::string(700, 'x') + "A";
    const std::string long_b = std::string(700, 'y') + "B";
    const auto key_a = detail::truncate_jet4_index_key(
        detail::encode_jet4_general_legacy_key(std::u16string(700, u'x') + u'A', false));
    const auto key_b = detail::truncate_jet4_index_key(
        detail::encode_jet4_general_legacy_key(std::u16string(700, u'y') + u'B', false));
    check(key_a != key_b, "long Memo values collapsed to the same truncated index key");
    check(detail::encode_jet4_general_legacy_key(std::u16string(700, u'x') + u'A', false) ==
              detail::encode_jet4_general_legacy_key(std::u16string(700, u'x') + u'B', false),
          "General Legacy Memo key did not follow Jackcess's 255-character text-index limit");
    const MemoValue raw_text{"ignored text", {'C', 0, 'h', 0, 'a', 0, 'r', 0, 'l', 0, 'i', 0, 'e', 0}};

    for (const auto format : {DatabaseFormat::v2000, DatabaseFormat::v2003}) {
        const auto suffix = format == DatabaseFormat::v2000 ? "v2000" : "v2003";
        const auto path = directory.file(std::string("memo-index-") + suffix + ".mdb");
        TableBuilder builder("MemoIndex");
        builder.add_column("Id", DataType::int32)
            .add_column("Note", DataType::memo)
            .add_index(IndexBuilder("IX_Note", "Note", true))
            .add_index(IndexBuilder("IX_NoteDesc", "Note", true, true));
        (void)Database::create(path, format, {builder});

        std::vector<RowId> row_ids;
        {
            auto writable = Database::open(path, DatabaseOpenMode::read_write);
            auto table = get_table(writable, "MemoIndex");
            row_ids = table.add_rows({
                {std::int32_t{1}, MemoValue{"Alpha", {}}},
                {std::int32_t{2}, MemoValue{"Bravo", {}}},
                {std::int32_t{3}, raw_text},
                {std::int32_t{4}, MemoValue{long_a, {}}},
                {std::int32_t{5}, MemoValue{long_b, {}}},
            });
            check(row_ids.size() == 5, "MEMO indexes did not accept all inserted long values");
            for (const auto& index_name : {"IX_Note", "IX_NoteDesc"}) {
                check(table.find_row_by_index(index_name, Value{std::string("Alpha")}).has_value() &&
                          table.find_row_by_index(index_name, Value{MemoValue{"Bravo", {}}}).has_value() &&
                          table.find_row_by_index(index_name, Value{std::string("Charlie")}).has_value() &&
                          table.find_row_by_index(index_name, Value{MemoValue{long_a, {}}}).has_value() &&
                          table.find_row_by_index(index_name, Value{MemoValue{long_b, {}}}).has_value(),
                      "MEMO index lookup failed for text, raw, or truncated long keys");
            }

            table.update_row(row_ids[1], {std::int32_t{2}, MemoValue{"Delta", {}}});
            for (const auto& index_name : {"IX_Note", "IX_NoteDesc"}) {
                check(table.find_row_by_index(index_name, Value{std::string("Delta")}).has_value() &&
                          !table.find_row_by_index(index_name, Value{std::string("Bravo")}).has_value(),
                      "MEMO index update did not replace the old key");
            }
            table.delete_row(row_ids[2]);
            for (const auto& index_name : {"IX_Note", "IX_NoteDesc"}) {
                check(!table.find_row_by_index(index_name, Value{std::string("Charlie")}).has_value(),
                      "MEMO index delete retained the removed key");
            }

            expect_error(DatabaseErrorCode::constraint_violation, [&] {
                table.add_row({std::int32_t{6}, MemoValue{"Alpha", {}}});
            });
            expect_error(DatabaseErrorCode::constraint_violation, [&] {
                table.add_row({std::int32_t{7}, MemoValue{std::string(700, 'x') + "Other", {}}});
            });
            std::size_t table_rows = 0;
            auto table_cursor = table.rows();
            while (table_cursor.next()) {
                ++table_rows;
            }
            check(table_rows == 4, "duplicate unique MEMO key changed the table row count");

            for (const auto& index_name : {"IX_Note", "IX_NoteDesc"}) {
                std::set<std::int32_t> indexed_ids;
                auto rows = table.rows_by_index(index_name);
                while (const auto row = rows.next()) {
                    indexed_ids.insert(std::get<std::int32_t>(row->at(0)));
                }
                check(indexed_ids == std::set<std::int32_t>({1, 2, 4, 5}),
                      "MEMO index scan lost or duplicated a row");
            }
        }

        const auto reopened = Database::open(path);
        const auto table = get_table(reopened, "MemoIndex");
        std::size_t reopened_rows = 0;
        auto reopened_cursor = table.rows();
        while (reopened_cursor.next()) {
            ++reopened_rows;
        }
        check(reopened_rows == 4 && table.find_row_by_index("IX_Note", Value{long_a}).has_value() &&
                  table.find_row_by_index("IX_NoteDesc", Value{std::string("Delta")}).has_value() &&
                  !table.find_row_by_index("IX_Note", Value{std::string("Charlie")}).has_value(),
              "MEMO index updates did not survive reopening");
    }

    const auto existing_path = directory.file("memo-index-existing.mdb");
    (void)Database::create(existing_path, DatabaseFormat::v2000,
                           {TableBuilder("ExistingMemoIndex")
                                .add_column("Id", DataType::int32)
                                .add_column("Note", DataType::memo)});
    {
        auto writable = Database::open(existing_path, DatabaseOpenMode::read_write);
        auto table = get_table(writable, "ExistingMemoIndex");
        table.add_rows({{std::int32_t{1}, MemoValue{"Before", {}}},
                        {std::int32_t{2}, MemoValue{long_a, {}}}});
        table.add_index(IndexBuilder("IX_Note", "Note", true));
        check(table.find_row_by_index("IX_Note", Value{long_a}).has_value() &&
                  table.find_row_by_index("IX_Note", Value{std::string("Before")}).has_value(),
              "adding an index to an existing MEMO column did not backfill existing rows");
    }
    check(get_table(Database::open(existing_path), "ExistingMemoIndex")
                  .find_row_by_index("IX_Note", Value{long_a}).has_value(),
          "a backfilled MEMO index was not readable after reopening");
}

void test_binary_index_keys(const TemporaryDirectory& directory) {
    using rea_jackcess::BlobValue;
    using rea_jackcess::Value;
    const auto path = directory.file("index-binary-key.mdb");
    rea_jackcess::TableBuilder builder("BinaryIndexed");
    builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Payload", rea_jackcess::DataType::binary)
        .add_index(rea_jackcess::IndexBuilder("IX_Asc", "Payload", true))
        .add_index(rea_jackcess::IndexBuilder("IX_Desc", "Payload", true, true));
    {
        (void)rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{builder});
    }

    const Value empty = BlobValue{};
    const Value one = BlobValue{{0x00}};
    const Value eight = BlobValue{std::vector<std::uint8_t>(8, 0x00)};
    const Value nine = BlobValue{std::vector<std::uint8_t>(9, 0x00)};
    const Value maximum = BlobValue{std::vector<std::uint8_t>(255, 0xff)};
    const Value replacement = BlobValue{{0x80, 0xff}};
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(writable, "BinaryIndexed");
        const auto ids = table.add_rows({
            {std::int32_t{4}, nine}, {std::int32_t{1}, empty}, {std::int32_t{5}, maximum},
            {std::int32_t{3}, eight}, {std::int32_t{0}, Value{}}, {std::int32_t{2}, one}});
        const auto before = read_bytes(path);
        expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
            (void)table.add_row({std::int32_t{6}, nine});
        });
        expect_error(rea_jackcess::DatabaseErrorCode::constraint_violation, [&] {
            table.update_row(ids[5], {std::int32_t{2}, maximum});
        });
        check(read_bytes(path) == before, "BINARY unique-key rejection modified the file");
        table.update_row(ids[5], {std::int32_t{2}, replacement});
        table.delete_row(ids[2]);
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto table = get_table(reopened, "BinaryIndexed");
    const auto read_ids = [](auto cursor) {
        std::vector<std::int32_t> ids;
        while (const auto row = cursor.next()) {
            ids.push_back(std::get<std::int32_t>(row->at(0)));
        }
        return ids;
    };
    check(read_ids(table.rows_by_index("IX_Asc")) == std::vector<std::int32_t>({0, 1, 3, 4, 2}),
          "ascending BINARY index order is incorrect");
    check(read_ids(table.rows_by_index("IX_Desc")) == std::vector<std::int32_t>({1, 2, 4, 3, 0}),
          "descending BINARY index order is incorrect");
    check(read_ids(table.rows_by_index_from("IX_Desc", nine)) == std::vector<std::int32_t>({4, 3, 0}),
          "descending BINARY seek did not find the suffix");
    check(read_ids(table.rows_by_index_range_reverse("IX_Asc", eight, replacement)) ==
              std::vector<std::int32_t>({4, 3}),
          "reverse BINARY range did not respect the encoded bounds");
    for (const auto* name : {"IX_Asc", "IX_Desc"}) {
        const auto hit = table.find_row_by_index(name, nine);
        check(hit && std::get<std::int32_t>(hit->at(0)) == 4 &&
                  !table.find_row_by_index(name, one) && !table.find_row_by_index(name, maximum),
              "BINARY index lookup missed an entry or found a stale key");
    }
}

void test_index_prefix_contraction(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-prefix.mdb");
    rea_jackcess::TableBuilder builder("SharedPrefix");
    builder.add_column("Name", rea_jackcess::DataType::text)
        .add_index(rea_jackcess::IndexBuilder("IX_Name", "Name", true));
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});

    const auto prefix_length = [&](std::uint32_t page_number) {
        const auto file = rea_jackcess::DatabaseFile::open(path);
        const auto bytes = file.read_page(page_number);
        return rea_jackcess::detail::read_u16(bytes, 24);
    };
    const std::string stem(220, 'a');
    constexpr int count = 200;
    std::uint32_t root = 0;
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "SharedPrefix");
        root = table.indexes().at(0).root_page_number;
        std::vector<std::vector<rea_jackcess::Value>> rows;
        for (int index = 0; index < count; ++index) {
            rows.push_back({stem + std::to_string(1000 + index)});
        }
        (void)table.add_rows(rows);
        check(prefix_length(root) >= stem.size(),
              "entries with a common prefix did not fit into a compressed root leaf");
    }
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "SharedPrefix");
        const auto outlier = table.add_row({std::string("z")});
        const auto file = rea_jackcess::DatabaseFile::open(path);
        const auto bytes = file.read_page(root);
        check(bytes[0] == 0x03, "contracting the prefix did not split the root leaf");
        const auto tail = rea_jackcess::detail::read_u32(bytes, 20);
        check(tail != 0 && file.read_page(tail)[0] == 0x04,
              "split root did not store its last leaf as a child tail");
        const auto hit = table.find_row_by_index("IX_Name", std::string("Z"));
        check(hit && hit->id() == outlier, "prefix contraction made the new leaf unreachable");
        table.delete_row(outlier);
    }

    const auto db = rea_jackcess::Database::open(path);
    const auto table = get_table(db, "SharedPrefix");
    auto scan = table.rows_by_index("IX_Name");
    for (int index = 0; index < count; ++index) {
        const auto row = scan.next();
        check(row && std::get<std::string>(row->at(0)) == stem + std::to_string(1000 + index),
              "compression and split did not preserve text index order");
    }
    check(!scan.next(), "prefix contraction test found an extra row");

    auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
    auto writable_table = get_table(writable, "SharedPrefix");
    for (int key = count - 1; key >= count / 2; --key) {
        const auto row = writable_table.find_row_by_index("IX_Name", stem + std::to_string(1000 + key));
        check(row.has_value(), "tail leaf lost a row before deletion");
        writable_table.delete_row(row->id());
        const auto file = rea_jackcess::DatabaseFile::open(path);
        if (rea_jackcess::detail::read_u32(file.read_page(root), 20) == 0) {
            check(writable_table.row_count_header() == static_cast<std::uint32_t>(key),
                  "clearing the tail changed the wrong number of rows");
            return;
        }
    }
    check(false, "empty tail leaf was not removed from the root");
}

void test_append_with_reference_usage_map(const TemporaryDirectory& directory) {
    namespace detail = rea_jackcess::detail;
    const auto path = directory.file("reference-usage-map.mdb");
    rea_jackcess::TableBuilder builder("Referenced");
    builder.add_column("Payload", rea_jackcess::DataType::binary);
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});

    const auto owned_reference = [](const detail::PageFile& file) {
        const auto catalog = detail::read_table_schema_catalog(file);
        const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [](const auto& item) {
            return item.name == "Referenced";
        });
        check(table != catalog.tables.end(), "reference map fixture has no user table");
        return table->definition.owned_pages;
    };
    std::uint64_t bitmap_number;
    {
        const auto file = detail::PageFile::open(path);
        const auto owned = owned_reference(file);
        bitmap_number = file.page_count();
        auto declaration_page = file.read_page(owned.page_number);
        const auto parsed = detail::parse_data_page(file, owned.page_number, declaration_page);
        const auto& declaration = parsed.rows.at(owned.row_number);
        check(detail::read_usage_map_pages(file, owned).empty(), "reference map fixture must start empty");
        const auto start = declaration.start_offset;
        std::fill_n(declaration_page.begin() + start, declaration.bytes.size(), 0);
        declaration_page[start] = 1;
        detail::write_u32(declaration_page, start + 1, static_cast<std::uint32_t>(bitmap_number));

        std::vector<std::uint8_t> bitmap(file.format().page_size, 0);
        bitmap[0] = 0x05;
        bitmap[1] = 0x01;
        auto writer = detail::PageFileWriter::open(path);
        check(writer.append_page(bitmap) == bitmap_number, "fixture appended its bitmap at the wrong page");
        writer.write_page(owned.page_number, declaration_page);
        detail::GlobalUsagePageUpdates global_updates;
        detail::mark_global_pages_allocated(file, global_updates, {bitmap_number});
        for (const auto& update : global_updates) {
            writer.write_page(update.first, update.second);
        }
        writer.flush();
    }

    std::vector<std::uint64_t> appended;
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "Referenced");
        const rea_jackcess::BlobValue payload{std::vector<std::uint8_t>(255, 0xaa)};
        for (const auto& id : table.add_rows(std::vector<std::vector<rea_jackcess::Value>>(50, {payload}))) {
            if (appended.empty() || appended.back() != id.page_number) {
                appended.push_back(id.page_number);
            }
        }
        check(appended.size() > 1, "reference map fixture did not allocate multiple row pages");
    }
    const auto file = detail::PageFile::open(path);
    const auto owned = owned_reference(file);
    const auto declaration = detail::parse_data_page(file, owned.page_number, file.read_page(owned.page_number));
    check(declaration.rows.at(owned.row_number).bytes[0] == 1 &&
              detail::read_u32(declaration.rows.at(owned.row_number).bytes, 1) == bitmap_number,
          "row insertion replaced the reference map or its bitmap");
    check(detail::read_usage_map_pages(file, owned) == appended,
          "reference bitmap does not own exactly the appended row pages");
    const auto db = rea_jackcess::Database::open(path);
    check(get_table(db, "Referenced").row_count_header() == 50,
          "reopening the reference map fixture lost appended rows");
}

void test_binary_index_split(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-binary-split.mdb");
    rea_jackcess::TableBuilder builder("BinarySplit");
    builder.add_column("Payload", rea_jackcess::DataType::binary)
        .add_index(rea_jackcess::IndexBuilder("IX_Payload", "Payload"));
    (void)rea_jackcess::Database::create(
        path, rea_jackcess::DatabaseFormat::v2003,
        std::vector<rea_jackcess::TableBuilder>{builder});

    using rea_jackcess::BlobValue;
    using rea_jackcess::Value;
    std::vector<std::vector<Value>> rows(19, {BlobValue{}});
    for (std::uint8_t key = 1; key <= 30; ++key) {
        rows.push_back({BlobValue{std::vector<std::uint8_t>(255, key)}});
    }
    auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
    auto table = get_table(db, "BinarySplit");
    (void)table.add_rows(rows);
    const auto duplicate_ids = table.add_rows(std::vector<std::vector<Value>>(800, {BlobValue{}}));
    table.delete_row(duplicate_ids.back());
    auto cursor = table.rows_by_index("IX_Payload");
    for (std::size_t index = 0; index < 818; ++index) {
        const auto row = cursor.next();
        check(row && std::get<BlobValue>(row->at(0)).bytes.empty(),
              "BINARY index lost a duplicate after splitting leaves");
    }
    for (std::uint8_t key = 1; key <= 30; ++key) {
        const auto row = cursor.next();
        check(row && std::get<BlobValue>(row->at(0)).bytes == std::vector<std::uint8_t>(255, key),
              "BINARY index lost or reordered a long key after splitting leaves");
    }
    check(!cursor.next(), "BINARY index returned an extra row after deleting a duplicate");
}

void test_unique_index_keys(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-unique.mdb");
    rea_jackcess::TableBuilder table_builder("UniqueKeys");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id", true));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "UniqueKeys";
        });
        check(table != tables.end(), "read-write database did not expose table UniqueKeys");
        check(table->indexes()[0].unique, "unique index flag was not persisted on create");
        row_ids = table->add_rows({{std::int32_t{1}}, {std::int32_t{2}}});
        const auto before_insert = read_bytes(path);
        bool insert_rejected = false;
        try {
            (void)table->add_row({std::int32_t{1}});
        } catch (const std::exception&) {
            insert_rejected = true;
        }
        check(insert_rejected, "duplicate insert was not rejected by the unique index");
        check(read_bytes(path) == before_insert,
              "rejected duplicate index key changed the database during insert");
        const auto before_update = read_bytes(path);
        bool update_rejected = false;
        try {
            table->update_row(row_ids[1], {std::int32_t{1}});
        } catch (const std::exception&) {
            update_rejected = true;
        }
        check(update_rejected, "duplicate update was not rejected by the unique index");
        check(read_bytes(path) == before_update,
              "rejected duplicate index key changed the database during update");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "UniqueKeys";
    });
    check(table != tables.end() && table->indexes()[0].unique,
          "unique index flag was not persisted");
    check(table->row_count_header() == 2 && table->indexes()[0].unique_entry_count == 2,
          "unique index rejections changed the row or distinct-key count");
    std::vector<std::int32_t> keys;
    auto cursor = table->rows_by_index("IX_Id");
    while (const auto row = cursor.next()) {
        keys.push_back(std::get<std::int32_t>(row->at(0)));
    }
    check(keys == std::vector<std::int32_t>({1, 2}),
          "unique index lost or reordered its entries");
}

void test_descending_index_keys(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-desc.mdb");
    rea_jackcess::TableBuilder table_builder("Desc");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id", false, true));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Desc";
        });
        check(table != tables.end(), "read-write database did not expose table Desc");
        (void)table->add_rows({{std::int32_t{3}}, {std::int32_t{1}}, {std::int32_t{2}}});
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Desc";
    });
    check(table != tables.end(), "reopened database did not expose table Desc");
    check(!table->indexes()[0].columns[0].ascending,
          "descending index flag was not persisted on create");

    const auto read_keys = [&](const auto& cursor_source) {
        std::vector<std::int32_t> keys;
        auto cursor = cursor_source();
        while (const auto row = cursor.next()) {
            keys.push_back(std::get<std::int32_t>(row->at(0)));
        }
        return keys;
    };
    check(read_keys([&] { return table->rows_by_index("IX_Id"); }) ==
              std::vector<std::int32_t>({3, 2, 1}),
          "descending index did not return keys high to low");
    check(read_keys([&] { return table->rows_by_index_reverse("IX_Id"); }) ==
              std::vector<std::int32_t>({1, 2, 3}),
          "reverse scan of a descending index did not return keys low to high");

    const auto hit = table->find_row_by_index("IX_Id", std::int32_t{2});
    check(hit && std::get<std::int32_t>(hit->at(0)) == 2,
          "descending index key lookup missed an existing key");
    auto from = table->rows_by_index_from("IX_Id", std::int32_t{2});
    std::vector<std::int32_t> suffix;
    while (const auto row = from.next()) {
        suffix.push_back(std::get<std::int32_t>(row->at(0)));
    }
    check(suffix == std::vector<std::int32_t>({2, 1}),
          "descending index seek did not return the lower suffix");
}

void test_guid_index_keys(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-guid.mdb");
    rea_jackcess::TableBuilder table_builder("GuidIndexed");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Uuid", rea_jackcess::DataType::guid)
        .add_index(rea_jackcess::IndexBuilder("IX_Uuid", "Uuid"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    // Jet4 stores the first three UUID fields little-endian, so index order is the stored byte
    // order and not the textual order: 0000...0001 < 0000...ff00 < 0100...0000 < ffff...ffff.
    const std::string a = "00000000-0000-0000-0000-000000000001";
    const std::string b = "01000000-0000-0000-0000-000000000000";
    const std::string c = "00000000-0000-0000-0000-00000000ff00";
    const std::string d = "ffffffff-ffff-ffff-ffff-ffffffffffff";
    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "GuidIndexed";
        });
        check(table != tables.end(), "read-write database did not expose table GuidIndexed");
        row_ids = table->add_rows({
            {std::int32_t{1}, rea_jackcess::GuidValue{b}},
            {std::int32_t{2}, rea_jackcess::GuidValue{d}},
            {std::int32_t{3}, rea_jackcess::GuidValue{a}},
            {std::int32_t{4}, rea_jackcess::GuidValue{c}},
        });
        check(row_ids.size() == 4, "inserting GUID index rows failed");
        // Move the largest value down between A and C, then delete the last row.
        table->update_row(row_ids[1],
                          {std::int32_t{2}, rea_jackcess::GuidValue{"00000000-0000-0000-0000-0000000000ff"}});
        table->delete_row(row_ids[3]);
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "GuidIndexed";
    });
    check(table != tables.end(), "reopened database did not expose table GuidIndexed");
    check(table->row_count_header() == 3, "GUID index maintenance changed the row count");
    check(table->indexes()[0].unique_entry_count == 3,
          "GUID index distinct-key count is incorrect");

    std::vector<std::string> order;
    auto cursor = table->rows_by_index("IX_Uuid");
    while (const auto row = cursor.next()) {
        order.push_back(std::get<rea_jackcess::GuidValue>(row->at(1)).text);
    }
    // A < 00..ff (moved row) < B, and the deleted row is gone.
    const std::vector<std::string> expected{"{" + a + "}",
                                            "{00000000-0000-0000-0000-0000000000ff}",
                                            "{" + b + "}"};
    check(order == expected, "GUID index did not return the stored byte order");

    const auto hit = table->find_row_by_index("IX_Uuid", rea_jackcess::GuidValue{a});
    check(hit && std::get<std::int32_t>(hit->at(0)) == 3,
          "GUID key lookup did not find the inserted row");
    check(!table->find_row_by_index("IX_Uuid", rea_jackcess::GuidValue{d}),
          "GUID key lookup found the deleted row");
    auto seek = table->rows_by_index_from("IX_Uuid", rea_jackcess::GuidValue{b});
    const auto first = seek.next();
    check(first && std::get<rea_jackcess::GuidValue>(first->at(1)).text == "{" + b + "}",
          "GUID seek did not start at the requested key");
}

void install_reference_global_map(const std::filesystem::path& path) {
    namespace detail = rea_jackcess::detail;
    const auto file = detail::PageFile::open(path);
    auto page = file.read_page(1);
    const auto data = detail::parse_data_page(file, 1, page);
    const auto& declaration = data.rows.at(0);
    check(declaration.bytes.at(0) == 0 && detail::read_u32(declaration.bytes, 1) == 0,
          "global map fixture needs an inline bitmap starting at page zero");
    const auto bitmap_number = file.page_count();
    std::vector<std::uint8_t> bitmap(file.format().page_size, 0xff);
    bitmap[0] = 0x05;
    bitmap[1] = 1;
    bitmap[2] = bitmap[3] = 0;
    std::copy(declaration.bytes.begin() + 5, declaration.bytes.end(), bitmap.begin() + 4);
    bitmap[4 + bitmap_number / 8] &= static_cast<std::uint8_t>(~(1u << (bitmap_number % 8)));
    auto start = page.begin() + declaration.start_offset;
    std::fill_n(start, declaration.bytes.size(), 0);
    *start = 1;
    detail::write_u32(page, declaration.start_offset + 1, static_cast<std::uint32_t>(bitmap_number));

    auto writer = detail::PageFileWriter::open(path);
    check(writer.append_page(bitmap) == bitmap_number, "fixture bitmap allocation changed page number");
    writer.write_page(1, page);
    writer.flush();
}

void test_global_map_growth(const TemporaryDirectory& directory, bool already_reference) {
    namespace detail = rea_jackcess::detail;
    const auto path = directory.file(already_reference ? "global-slot-growth.mdb" : "global-promotion.mdb");
    rea_jackcess::TableBuilder builder("GlobalGrowth");
    builder.add_column("Id", rea_jackcess::DataType::int32);
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});
    if (already_reference) {
        install_reference_global_map(path);
    }

    std::uint64_t frontier;
    std::uint64_t page_size;
    {
        const auto file = detail::PageFile::open(path);
        page_size = file.format().page_size;
        const auto declaration = detail::parse_data_page(file, 1, file.read_page(1)).rows.at(0).bytes;
        frontier = already_reference ? (page_size - 4) * 8 : (declaration.size() - 5) * 8;
        check(frontier > file.page_count(), "fixture does not reach a bitmap boundary");
    }
    // Make the very next append cross the boundary without inserting unrelated table rows.
    std::filesystem::resize_file(path, frontier * page_size);
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto table = get_table(db, "GlobalGrowth");
        check(table.add_row({std::int32_t{41}}).page_number == frontier,
              "bitmap growth changed the allocated data page");
        (void)table.add_row({std::int32_t{42}});
    }
    const auto file = detail::PageFile::open(path);
    const auto bytes = detail::parse_data_page(file, 1, file.read_page(1)).rows.at(0).bytes;
    check(bytes.at(0) == 1, "global bitmap did not use reference representation after growth");
    const auto bitmap_for = [&](std::uint64_t number) {
        const auto capacity = (page_size - 4) * 8;
        return detail::read_u32(bytes, 1 + number / capacity * 4);
    };
    const auto is_free = [&](std::uint64_t number) {
        return global_page_is_free(path, number);
    };
    check(file.page_count() == frontier + 2 && bitmap_for(frontier) == frontier + 1,
          "global map growth did not append exactly one new bitmap after the data page");
    check(!is_free(1) && !is_free(frontier) && !is_free(frontier + 1) && is_free(frontier + 2),
          "global map did not account for its own bitmap and the new data page");
    check(is_free(frontier - 1), "global map growth discarded an existing free bit");
    const auto db = rea_jackcess::Database::open(path);
    auto rows = get_table(db, "GlobalGrowth").rows();
    const auto first = rows.next();
    const auto second = rows.next();
    check(first && second && std::get<std::int32_t>(first->at(0)) == 41 &&
              std::get<std::int32_t>(second->at(0)) == 42 && !rows.next(),
          "reopening after global map growth did not preserve the rows");
}

void test_owned_map_growth(const TemporaryDirectory& directory) {
    namespace detail = rea_jackcess::detail;
    const auto path = directory.file("owned-map-growth.mdb");
    rea_jackcess::TableBuilder builder("OwnedGrowth");
    builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Note", rea_jackcess::DataType::memo)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));
    (void)rea_jackcess::Database::create(path, rea_jackcess::DatabaseFormat::v2000, {builder});
    const auto memo_for = [](std::int32_t id) {
        return rea_jackcess::MemoValue{std::string(996, 'm') + std::to_string(1000 + id), {}};
    };
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        (void)get_table(db, "OwnedGrowth").add_row({std::int32_t{0}, memo_for(0)});
    }
    constexpr std::uint64_t page_size = 4096;
    constexpr std::uint64_t frontier = (page_size - 4) * 8;
    std::filesystem::resize_file(path, frontier * page_size);
    {
        auto db = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        std::vector<std::vector<rea_jackcess::Value>> rows;
        for (std::int32_t i = 1; i <= 700; ++i) {
            rows.push_back({i, memo_for(i)});
        }
        (void)get_table(db, "OwnedGrowth").add_rows(rows);
    }

    const auto file = detail::PageFile::open(path);
    const auto catalog = detail::read_table_schema_catalog(file);
    const auto table = std::find_if(catalog.tables.begin(), catalog.tables.end(), [](const auto& item) {
        return item.name == "OwnedGrowth";
    });
    check(table != catalog.tables.end(), "owned map fixture did not expose its table");
    const auto& definition = table->definition;
    const auto global = detail::parse_data_page(file, 1, file.read_page(1)).rows.at(0).bytes;
    check(global.at(0) == 1, "global map did not grow with owned maps");
    for (const auto& reference : {definition.owned_pages, definition.physical_indexes.at(0).owned_pages,
                                  definition.columns.at(1).long_value_owned_pages.value()}) {
        const auto declaration = detail::parse_data_page(file, reference.page_number,
                                                        file.read_page(reference.page_number))
                                     .rows.at(reference.row_number).bytes;
        check(declaration.at(0) == 1, "owned map did not promote: row " + std::to_string(reference.row_number));
        const auto owned = detail::read_usage_map_pages(file, reference);
        check(!owned.empty() && owned.front() < frontier && owned.back() >= frontier,
              "owned map lost pages on one side of the reference boundary");
        for (std::uint64_t slot : {0, 1}) {
            const auto bitmap = detail::read_u32(declaration, 1 + 4 * slot);
            check(bitmap >= frontier && bitmap < file.page_count(),
                  "owned map did not append and retain both reference bitmaps");
            const auto global_bitmap = file.read_page(detail::read_u32(global, 1 + 4 * (bitmap / frontier)));
            const auto bit = bitmap % frontier;
            check((global_bitmap.at(4 + bit / 8) & (1u << (bit % 8))) == 0,
                  "an owned-map bitmap remains free in the global map");
        }
    }
    const auto db = rea_jackcess::Database::open(path);
    auto scan = get_table(db, "OwnedGrowth").rows_by_index("IX_Id");
    for (std::int32_t i = 0; i <= 700; ++i) {
        const auto row = scan.next();
        check(row && std::get<std::int32_t>(row->at(0)) == i &&
                  std::get<rea_jackcess::MemoValue>(row->at(1)).text == memo_for(i).text,
              "growing owned maps lost an index entry or long value");
    }
    check(!scan.next(), "owned map growth added an unexpected row");
}

void test_index_leaf_merge_on_delete(const TemporaryDirectory& directory, bool reference_map) {
    const auto path = directory.file(reference_map ? "index-merge-reference.mdb" : "index-merge.mdb");
    rea_jackcess::TableBuilder table_builder("Merge");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    if (reference_map) {
        install_reference_global_map(path);
    }

    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Merge";
        });
        check(table != tables.end(), "read-write database did not expose table Merge");
        std::vector<std::vector<rea_jackcess::Value>> rows;
        rows.reserve(700);
        for (std::int32_t value = 1; value <= 700; ++value) {
            rows.push_back({value});
        }
        row_ids = table->add_rows(rows);
    }

    const auto read_index_state = [&]() {
        struct State {
            std::uint64_t page_count;
            std::vector<std::uint64_t> owned_pages;
            std::vector<std::uint64_t> free_pages;
        };
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
        const auto table = std::find_if(catalog.tables.begin(),
                                        catalog.tables.end(),
                                        [](const auto& candidate) {
                                            return candidate.name == "Merge";
                                        });
        if (table == catalog.tables.end()) {
            throw std::runtime_error("internal catalog did not expose table Merge");
        }
        State state{file.page_count(), {}, {}};
        for (const auto& physical : table->definition.physical_indexes) {
            const auto owned = rea_jackcess::detail::read_usage_map_pages(file, physical.owned_pages);
            state.owned_pages.insert(state.owned_pages.end(), owned.begin(), owned.end());
        }
        // The global usage map stores 1 for a free page, so it cannot go through
        // read_usage_map_pages, which rejects pages beyond the current end of file.
        const auto global_page = file.read_page(1);
        const auto parsed = rea_jackcess::detail::parse_data_page(file, 1, global_page);
        const auto& declaration = parsed.rows.at(0);
        check(!declaration.deleted && !declaration.overflow && declaration.bytes.size() >= 5,
              "invalid global map in merge fixture");
        const auto& row = declaration.bytes;
        check(row[0] == (reference_map ? 1 : 0), "global map fixture changed representation");
        const auto bitmap = reference_map
            ? file.read_page(rea_jackcess::detail::read_u32(row, 1)) : row;
        const auto offset = reference_map ? 4 : 5;
        const std::uint64_t first_page = reference_map ? 0 : rea_jackcess::detail::read_u32(row, 1);
        for (std::uint64_t number = first_page; number < file.page_count(); ++number) {
            const auto bit = number - first_page;
            if ((bitmap.at(offset + bit / 8) & (1u << (bit % 8))) != 0) {
                state.free_pages.push_back(number);
            }
        }
        return state;
    };

    const auto before_delete = read_index_state();
    for (const auto owned : before_delete.owned_pages) {
        check(std::find(before_delete.free_pages.begin(), before_delete.free_pages.end(), owned) ==
                  before_delete.free_pages.end(),
              "a page owned by the index remains free in the global bitmap");
    }
    check(before_delete.owned_pages.size() > 1,
          "a 700-key index did not own several leaf pages");

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Merge";
        });
        check(table != tables.end(), "read-write database did not expose table Merge");
        // Draining the lowest leaf must unlink and release it instead of failing on its last key.
        for (std::size_t index = 0; index < 400; ++index) {
            table->delete_row(row_ids[index]);
        }
        check(table->row_count_header() == 300,
              "draining a leaf produced the wrong row count");
    }

    const auto after_delete = read_index_state();
    check(after_delete.page_count == before_delete.page_count,
          "leaf merging changed the database page count");
    check(after_delete.owned_pages.size() < before_delete.owned_pages.size(),
          "leaf merging did not drop the emptied leaf from the index usage map");
    std::vector<std::uint64_t> released;
    for (const std::uint64_t page : before_delete.owned_pages) {
        if (std::find(after_delete.owned_pages.begin(), after_delete.owned_pages.end(), page) ==
            after_delete.owned_pages.end()) {
            released.push_back(page);
        }
    }
    check(!released.empty(), "no index page was released by leaf merging");
    for (const std::uint64_t page : released) {
        check(std::find(after_delete.free_pages.begin(), after_delete.free_pages.end(), page) !=
                  after_delete.free_pages.end(),
              "a released index page is not marked free in the global usage map");
    }

    {
        auto reopened = rea_jackcess::Database::open(path);
        const auto tables = reopened.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Merge";
        });
        check(table != tables.end(), "reopened database did not expose table Merge");
        check(table->row_count_header() == 300, "reopened merged table has the wrong row count");
        check(table->indexes()[0].unique_entry_count == 300,
              "reopened merged index has the wrong distinct-key count");
        std::vector<std::int32_t> keys;
        auto cursor = table->rows_by_index("IX_Id");
        while (const auto row = cursor.next()) {
            keys.push_back(std::get<std::int32_t>(row->at(0)));
        }
        check(keys.size() == 300 && keys.front() == 401 && keys.back() == 700,
              "the merged index lost or reordered entries");
        check(std::is_sorted(keys.begin(), keys.end()),
              "the merged index is not sorted");
    }

    // The tree must keep working after the merge: adding rows raises a new leaf again.
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Merge";
        });
        check(table != tables.end(), "read-write database did not expose table Merge");
        std::vector<std::vector<rea_jackcess::Value>> rows;
        for (std::int32_t value = 701; value <= 1100; ++value) {
            rows.push_back({value});
        }
        (void)table->add_rows(rows);
    }
    {
        auto reopened = rea_jackcess::Database::open(path);
        const auto tables = reopened.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Merge";
        });
        check(table != tables.end(), "reopened database did not expose table Merge");
        std::vector<std::int32_t> keys;
        auto cursor = table->rows_by_index("IX_Id");
        while (const auto row = cursor.next()) {
            keys.push_back(std::get<std::int32_t>(row->at(0)));
        }
        check(keys.size() == 700 && keys.front() == 401 && keys.back() == 1100 &&
                  std::is_sorted(keys.begin(), keys.end()),
              "the index did not grow correctly after a leaf merge");
    }
}

void test_index_seek_and_range(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-seek.mdb");
    rea_jackcess::TableBuilder table_builder("Seek");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Seek";
        });
        check(table != tables.end(), "read-write database did not expose table Seek");
        std::vector<std::vector<rea_jackcess::Value>> rows;
        rows.reserve(700);
        for (std::int32_t value = 1; value <= 700; ++value) {
            rows.push_back({value});
        }
        (void)table->add_rows(rows);
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Seek";
    });
    check(table != tables.end(), "reopened database did not expose table Seek");

    const auto collect = [](rea_jackcess::RowCursor cursor) {
        std::vector<std::int32_t> keys;
        while (const auto row = cursor.next()) {
            keys.push_back(std::get<std::int32_t>(row->at(0)));
        }
        return keys;
    };

    {
        // A seek inside the tree must land on the right leaf of the two-level index.
        const auto keys = collect(table->rows_by_index_from("IX_Id", std::int32_t{350}));
        check(keys.size() == 351, "the seek cursor did not start at the requested key");
        check(keys.front() == 350 && keys.back() == 700,
              "the seek cursor returned the wrong key range");
        check(std::is_sorted(keys.begin(), keys.end()),
              "the seek cursor returned keys out of order");
    }
    {
        const auto keys = collect(table->rows_by_index_from("IX_Id", std::int32_t{0}));
        check(keys.size() == 700 && keys.front() == 1,
              "a seek below the smallest key did not start at the first entry");
    }
    {
        const auto keys = collect(table->rows_by_index_from("IX_Id", std::int32_t{701}));
        check(keys.empty(), "a seek beyond the largest key returned rows");
    }
    {
        const auto keys = collect(table->rows_by_index_range("IX_Id",
                                                             std::int32_t{100},
                                                             std::int32_t{110}));
        check(keys.size() == 10 && keys.front() == 100 && keys.back() == 109,
              "the range cursor did not honour the exclusive end bound");
    }
    {
        const auto keys = collect(table->rows_by_index_range("IX_Id",
                                                             std::int32_t{700},
                                                             std::int32_t{1000}));
        check(keys.size() == 1 && keys.front() == 700,
              "a range whose end is beyond the index returned the wrong rows");
    }
    {
        const auto row = table->find_row_by_index("IX_Id", std::int32_t{700});
        check(row && std::get<std::int32_t>(row->at(0)) == 700,
              "exact index lookup did not find an existing key");
        check(!table->find_row_by_index("IX_Id", std::int32_t{701}),
              "exact index lookup found a missing key");
        check(!table->find_row_by_index("IX_Id", std::int32_t{0}),
              "exact index lookup matched the wrong key");
    }

    // Duplicate keys must resolve to the first entry, and NULL keys sort first.
    const auto duplicate_path = directory.file("index-seek-duplicates.mdb");
    rea_jackcess::TableBuilder duplicate_builder("Dup");
    duplicate_builder.add_column("Code", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Code", "Code"));
    {
        const auto created = rea_jackcess::Database::create(
            duplicate_path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{duplicate_builder});
        (void)created;
    }
    {
        auto writable = rea_jackcess::Database::open(duplicate_path,
                                                     rea_jackcess::DatabaseOpenMode::read_write);
        auto writable_tables = writable.tables();
        const auto dup_table = std::find_if(writable_tables.begin(),
                                            writable_tables.end(),
                                            [](const auto& candidate) {
                                                return candidate.name() == "Dup";
                                            });
        check(dup_table != writable_tables.end(), "read-write database did not expose table Dup");
        (void)dup_table->add_rows({{std::int32_t{7}},
                                   {std::monostate{}},
                                   {std::int32_t{5}},
                                   {std::int32_t{5}}});
    }
    {
        auto duplicate_database = rea_jackcess::Database::open(duplicate_path);
        const auto duplicate_tables = duplicate_database.tables();
        const auto dup_table = std::find_if(duplicate_tables.begin(),
                                            duplicate_tables.end(),
                                            [](const auto& candidate) {
                                                return candidate.name() == "Dup";
                                            });
        check(dup_table != duplicate_tables.end(), "reopened database did not expose table Dup");
        std::vector<rea_jackcess::Value> from_null;
        for (auto cursor = dup_table->rows_by_index_from("IX_Code", std::monostate());;) {
            const auto row = cursor.next();
            if (!row) {
                break;
            }
            from_null.push_back(row->at(0));
        }
        check(from_null.size() == 4 &&
                  std::holds_alternative<std::monostate>(from_null.front()) &&
                  std::get<std::int32_t>(from_null[1]) == 5 &&
                  std::get<std::int32_t>(from_null[3]) == 7,
              "a NULL seek bound did not start at the NULL key");
        const auto five_range = collect(dup_table->rows_by_index_range("IX_Code",
                                                                      std::int32_t{5},
                                                                      std::int32_t{7}));
        check(five_range.size() == 2 && five_range[0] == 5 && five_range[1] == 5,
              "the range cursor did not return both duplicate keys");
        check(dup_table->find_row_by_index("IX_Code", std::int32_t{7}) != std::nullopt,
              "exact lookup missed the last distinct key");
        check(!dup_table->find_row_by_index("IX_Code", std::int32_t{6}),
              "exact lookup matched a key that is not in the index");
    }

    // TEXT keys cannot be declared on a new table at all, so lookup rejection for existing TEXT
    // indexes is covered by the fixed real sample in real_sample_test.cpp.
}

void test_index_delete_all_rows(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-delete-all.mdb");
    rea_jackcess::TableBuilder table_builder("Drain");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Drain";
        });
        check(table != tables.end(), "read-write database did not expose table Drain");
        const auto row_ids = table->add_rows({{std::int32_t{5}}, {std::int32_t{6}}});
        table->delete_row(row_ids[0]);
        // Emptying the single root leaf must stay legal: that is the state a new index starts in.
        table->delete_row(row_ids[1]);
        check(table->row_count_header() == 0, "deleting every indexed row left a row count");
        // The same table must accept rows again after being drained.
        (void)table->add_rows({{std::int32_t{7}}});
    }
    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Drain";
    });
    check(table != tables.end(), "reopened database did not expose table Drain");
    check(table->row_count_header() == 1, "reopened drained table has an unexpected row count");
    check(table->indexes()[0].unique_entry_count == 1,
          "reopened drained index has an unexpected distinct-key count");
    auto cursor = table->rows_by_index("IX_Id");
    const auto row = cursor.next();
    check(row && std::get<std::int32_t>(row->at(0)) == 7 && !cursor.next(),
          "the index did not expose exactly the row added after draining");
}

void test_index_split_and_multi_leaf_maintenance(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-split.mdb");
    rea_jackcess::TableBuilder table_builder("Crowded");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));
    std::uint64_t pages_before_write = 0;
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
        pages_before_write = rea_jackcess::DatabaseFile::open(path).page_count();
    }

    // A single Jet4 leaf holds about 450 four-byte LONG keys, so this batch must split leaves.
    std::vector<std::vector<rea_jackcess::Value>> rows;
    for (std::int32_t value = 1; value <= 700; ++value) {
        rows.push_back({value});
    }
    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Crowded";
        });
        check(table != tables.end(), "read-write database did not expose table Crowded");
        row_ids = table->add_rows(rows);
        check(row_ids.size() == rows.size() && table->row_count_header() == 700,
              "splitting index leaves lost rows from the batch");
    }
    const std::uint64_t pages_after_write = rea_jackcess::DatabaseFile::open(path).page_count();
    check(pages_after_write > pages_before_write + 2,
          "index leaf splits did not allocate new index pages");

    const auto read_keys = [&](const std::string& index_name) {
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Crowded";
        });
        check(table != tables.end(), "reopened database did not expose table Crowded");
        std::vector<std::int32_t> keys;
        auto cursor = table->rows_by_index(index_name);
        while (const auto row = cursor.next()) {
            keys.push_back(std::get<std::int32_t>(row->at(0)));
        }
        return keys;
    };

    {
        const auto keys = read_keys("IX_Id");
        check(keys.size() == 700, "the split index did not return every entry");
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Crowded";
        });
        check(table != tables.end() && table->indexes()[0].unique_entry_count == 700,
              "splitting index leaves produced the wrong distinct-key count");
        for (std::size_t index = 0; index < keys.size(); ++index) {
            check(keys[index] == static_cast<std::int32_t>(index + 1),
                  "the split index did not return entries in ascending order");
        }
    }

    // Reverse cursors must cross leaf pages backwards and keep the same encoded-key bounds.
    {
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Crowded";
        });
        check(table != tables.end(), "reopened database did not expose table Crowded");
        std::vector<std::int32_t> reversed;
        auto cursor = table->rows_by_index_reverse("IX_Id");
        while (const auto row = cursor.next()) {
            reversed.push_back(std::get<std::int32_t>(row->at(0)));
        }
        check(reversed.size() == 700, "reverse index scan lost entries across leaf pages");
        for (std::size_t index = 0; index < reversed.size(); ++index) {
            check(reversed[index] == static_cast<std::int32_t>(700 - index),
                  "reverse index scan did not return descending keys");
        }
        std::vector<std::int32_t> range;
        auto ranged = table->rows_by_index_range_reverse("IX_Id", std::int32_t{350},
                                                        std::int32_t{400});
        while (const auto row = ranged.next()) {
            range.push_back(std::get<std::int32_t>(row->at(0)));
        }
        check(range.size() == 50, "reverse range scan returned the wrong entry count");
        for (std::size_t index = 0; index < range.size(); ++index) {
            check(range[index] == static_cast<std::int32_t>(399 - index),
                  "reverse range scan did not return [start, stop) in descending order");
        }
    }

    // Removing and re-adding keys must address the right leaf of the multi-page tree.
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Crowded";
        });
        check(table != tables.end(), "read-write database did not expose table Crowded");
        table->delete_row(row_ids[349]);
        table->delete_row(row_ids[0]);
        (void)table->add_rows({{std::int32_t{350}}, {std::int32_t{1}}, {std::int32_t{701}}});
        check(table->row_count_header() == 701, "multi-leaf maintenance changed the row count");
    }
    {
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Crowded";
        });
        check(table != tables.end(), "reopened database did not expose table Crowded");
        check(table->indexes()[0].unique_entry_count == 701,
              "multi-leaf maintenance produced the wrong distinct-key count");
        std::vector<std::int32_t> keys;
        auto cursor = table->rows_by_index("IX_Id");
        while (const auto row = cursor.next()) {
            keys.push_back(std::get<std::int32_t>(row->at(0)));
        }
        check(keys.size() == 701, "multi-leaf maintenance lost index entries");
        for (std::size_t index = 0; index < keys.size(); ++index) {
            check(keys[index] == static_cast<std::int32_t>(index + 1),
                  "multi-leaf maintenance broke the index order");
        }
    }
}

std::string sequential_guid(std::uint32_t number) {
    static const char digits[] = "0123456789abcdef";
    std::string tail(12, '0');
    for (std::size_t index = tail.size(); index-- > 0;) {
        tail[index] = digits[number & 0x0f];
        number >>= 4;
    }
    // Put the changing bytes first on disk, in big-endian number order.
    return "0000" + tail.substr(10, 2) + tail.substr(8, 2) + "-0000-0000-0000-000000000000";
}

void test_index_three_level_tree(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-three-level.mdb");
    rea_jackcess::TableBuilder table_builder("DeepTree");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Uuid", rea_jackcess::DataType::guid)
        .add_index(rea_jackcess::IndexBuilder("IX_Uuid", "Uuid"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    // These GUID keys share only a short page prefix, so roughly 22k rows force both leaf splits and
    // repeated node splits until the root reaches level 2.
    constexpr std::uint32_t row_total = 22000;
    std::vector<std::vector<rea_jackcess::Value>> rows;
    rows.reserve(row_total);
    for (std::uint32_t number = 0; number < row_total; ++number) {
        rows.push_back({static_cast<std::int32_t>(number),
                        rea_jackcess::GuidValue{sequential_guid(number)}});
    }
    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "DeepTree";
        });
        check(table != tables.end(), "read-write database did not expose table DeepTree");
        row_ids = table->add_rows(rows);
        check(row_ids.size() == row_total, "three-level index insert lost rows");
    }

    const auto read_ids = [&]() {
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "DeepTree";
        });
        check(table != tables.end(), "reopened database did not expose table DeepTree");
        std::vector<std::int32_t> ids;
        auto cursor = table->rows_by_index("IX_Uuid");
        while (const auto row = cursor.next()) {
            ids.push_back(std::get<std::int32_t>(row->at(0)));
        }
        return ids;
    };

    {
        const auto ids = read_ids();
        check(ids.size() == row_total, "three-level index did not return every entry");
        for (std::uint32_t number = 0; number < row_total; ++number) {
            check(ids[number] == static_cast<std::int32_t>(number),
                  "three-level index did not keep stored GUID byte order");
        }
        auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "DeepTree";
        });
        check(table != tables.end() && table->indexes()[0].unique_entry_count == row_total,
              "three-level index has the wrong distinct-key count");
        const auto file = rea_jackcess::DatabaseFile::open(path);
        const auto root = file.read_page(table->indexes()[0].root_page_number);
        check(root[0] == 0x03 && root[26] >= 2,
              "22k GUID entries did not grow the index root to level 2 or deeper");
        const auto hit = table->find_row_by_index(
            "IX_Uuid", rea_jackcess::GuidValue{sequential_guid(row_total / 2)});
        check(hit && std::get<std::int32_t>(hit->at(0)) == static_cast<std::int32_t>(row_total / 2),
              "three-level index key lookup addressed the wrong leaf");
    }

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "DeepTree";
        });
        check(table != tables.end(), "read-write database did not expose table DeepTree");
        for (std::uint32_t number = 0; number < row_total; number += 4000) {
            table->delete_row(row_ids[number]);
        }
        table->delete_row(row_ids[row_total - 1]);
    }

    {
        const auto ids = read_ids();
        check(ids.size() == row_total - 7, "three-level index delete did not remove entries");
        std::size_t next = 0;
        for (std::uint32_t number = 0; number < row_total; ++number) {
            if (number % 4000 == 0 || number == row_total - 1) {
                continue;
            }
            check(next < ids.size() && ids[next++] == static_cast<std::int32_t>(number),
                  "three-level index delete broke the remaining key order");
        }
        check(next == ids.size(), "three-level index delete left extra entries");
    }
}

void test_money_index_keys(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-money.mdb");
    rea_jackcess::TableBuilder table_builder("MoneyIndexed");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Amount", rea_jackcess::DataType::money)
        .add_index(rea_jackcess::IndexBuilder("IX_Amount", "Amount"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    std::vector<rea_jackcess::RowId> row_ids;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "MoneyIndexed";
        });
        check(table != tables.end(), "read-write database did not expose table MoneyIndexed");
        row_ids = table->add_rows({
            {std::int32_t{1}, rea_jackcess::DecimalValue{"100.0000"}},
            {std::int32_t{2}, rea_jackcess::DecimalValue{"-0.0001"}},
            {std::int32_t{3}, rea_jackcess::DecimalValue{"-2341234.0345"}},
            {std::int32_t{4}, rea_jackcess::DecimalValue{"0"}},
            {std::int32_t{5}, std::monostate{}},
        });
        check(row_ids.size() == 5, "inserting MONEY index rows failed");
        // Move the largest amount below the others, and remove the NULL key.
        table->update_row(row_ids[0], {std::int32_t{1}, rea_jackcess::DecimalValue{"-5.5"}});
        table->delete_row(row_ids[4]);
    }

    const auto expected_amounts = std::vector<std::string>{
        "-2341234.0345", "-5.5000", "-0.0001", "0.0000"};
    const auto expected_ids = std::vector<std::int32_t>{3, 1, 2, 4};
    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "MoneyIndexed";
    });
    check(table != tables.end(), "reopened database did not expose table MoneyIndexed");
    check(table->row_count_header() == 4, "MONEY index maintenance changed the row count");
    check(table->indexes()[0].unique_entry_count == 4,
          "MONEY index distinct-key count is incorrect");
    auto cursor = table->rows_by_index("IX_Amount");
    for (std::size_t index = 0; index < expected_amounts.size(); ++index) {
        const auto row = cursor.next();
        check(row &&
                  std::get<std::int32_t>(row->at(0)) == expected_ids[index] &&
                  std::get<rea_jackcess::DecimalValue>(row->at(1)).text == expected_amounts[index],
              "MONEY index did not return the expected amount order");
    }
    check(!cursor.next(), "MONEY index returned an unexpected extra entry");
}

void test_index_maintenance_across_row_moves(const TemporaryDirectory& directory) {
    const auto path = directory.file("index-row-moves.mdb");
    rea_jackcess::TableBuilder table_builder("Moves");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Note", rea_jackcess::DataType::text)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    std::int32_t first_id = 0;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Moves";
        });
        check(table != tables.end(), "read-write database did not expose table Moves");
        const auto row_ids = table->add_rows({
            {std::int32_t{7}, std::string("short")},
            {std::int32_t{9}, std::string("other")},
        });
        check(row_ids.size() == 2, "inserting into the move table failed");
        first_id = row_ids.front().row_number;

        // Growing the row rewrites it at the same RowId; the index entry must stay valid and
        // must not be duplicated. Shrinking it back exercises the compaction path.
        table->update_row(row_ids[0], {std::int32_t{7}, std::string(200, 'G')});
        table->update_row(row_ids[0], {std::int32_t{7}, std::string("tiny")});
        (void)first_id;
    }

    {
        auto reopened = rea_jackcess::Database::open(path);
        const auto tables = reopened.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Moves";
        });
        check(table != tables.end(), "reopened database did not expose table Moves");
        check(table->row_count_header() == 2, "row moves changed the table row count");
        check(table->indexes()[0].unique_entry_count == 2,
              "row moves changed the distinct-key count");
        auto cursor = table->rows_by_index("IX_Id");
        const auto first = cursor.next();
        check(first && std::get<std::int32_t>(first->at(0)) == 7 &&
                  std::get<std::string>(first->at(1)) == std::string("tiny"),
              "the moved row did not keep its index entry and value");
        const auto second = cursor.next();
        check(second && std::get<std::int32_t>(second->at(0)) == 9,
              "the untouched row lost its index entry");
        check(!cursor.next(), "the index duplicated the moved entry");
    }

    const auto null_path = directory.file("index-null-keys.mdb");
    rea_jackcess::TableBuilder nullable_table("Nullable");
    nullable_table.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Maybe", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Maybe", "Maybe"));
    {
        const auto created = rea_jackcess::Database::create(
            null_path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{nullable_table});
        (void)created;
    }
    {
        auto writable = rea_jackcess::Database::open(null_path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Nullable";
        });
        check(table != tables.end(), "read-write database did not expose table Nullable");
        const auto row_ids = table->add_rows({
            {std::int32_t{1}, std::monostate{}},
            {std::int32_t{2}, std::int32_t{5}},
        });
        // NULL -> value adds a distinct key.
        table->update_row(row_ids[0], {std::int32_t{1}, std::int32_t{4}});
        // value -> NULL removes it again.
        table->update_row(row_ids[1], {std::int32_t{2}, std::monostate{}});
    }
    {
        auto reopened = rea_jackcess::Database::open(null_path);
        const auto tables = reopened.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Nullable";
        });
        check(table != tables.end(), "reopened database did not expose table Nullable");
        check(table->row_count_header() == 2, "NULL key updates changed the row count");
        check(table->indexes()[0].unique_entry_count == 2,
              "NULL key updates produced the wrong distinct-key count");
        auto cursor = table->rows_by_index("IX_Maybe");
        const auto null_row = cursor.next();
        check(null_row && std::holds_alternative<std::monostate>(null_row->at(1)),
              "the NULL key did not sort first after the update");
        const auto value_row = cursor.next();
        check(value_row && std::get<std::int32_t>(value_row->at(1)) == 4,
              "the replaced NULL key did not become the new value");
        check(!cursor.next(), "NULL key updates left an extra index entry");
    }
}

void test_create_tables_with_user_indexes(const TemporaryDirectory& directory) {
    const auto path = directory.file("indexed-table.mdb");
    rea_jackcess::TableBuilder table_builder("Indexed");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Code", rea_jackcess::DataType::int32)
        .add_column("Name", rea_jackcess::DataType::text)
        .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"))
        .add_index(rea_jackcess::IndexBuilder("IX_Code", "Code"));
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Indexed";
        });
        check(table != tables.end(), "created database did not expose table Indexed");
        check(table->indexes().size() == 2, "created table did not expose both user indexes");
        const auto& id_index = table->indexes()[0];
        const auto& code_index = table->indexes()[1];
        check(id_index.name == "IX_Id" && id_index.columns.size() == 1 &&
                  id_index.columns[0].name == "Id" && id_index.columns[0].ascending &&
                  !id_index.unique && !id_index.required && !id_index.primary_key &&
                  id_index.root_page_number != 0,
              "user index IX_Id did not keep its declared schema");
        check(code_index.name == "IX_Code" && code_index.columns.size() == 1 &&
                  code_index.columns[0].name == "Code" && code_index.columns[0].ascending &&
                  code_index.root_page_number != 0 &&
                  code_index.root_page_number != id_index.root_page_number,
              "user index IX_Code did not keep its declared schema");
        check(!table->rows_by_index("IX_Id").next() && !table->rows_by_index("IX_Code").next(),
              "newly created user indexes unexpectedly contain entries");
        check(!table->rows().next(), "newly created indexed table unexpectedly contains rows");
    }
    check(rea_jackcess::DatabaseFile::open(path).page_count() == 28,
          "a table with two indexes did not append four Jet4 pages");

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Indexed";
        });
        check(table != tables.end(), "read-write database did not expose table Indexed");
        const auto row_ids = table->add_rows({
            {std::int32_t{1}, std::int32_t{30}, std::string("a")},
            {std::int32_t{2}, std::int32_t{10}, std::string("b")},
            {std::int32_t{3}, std::int32_t{20}, std::string("c")},
        });
        check(row_ids.size() == 3 && table->row_count_header() == 3,
              "inserting into an indexed table returned the wrong row count");

        // Changing a key must move the entry; the old key must leave the index.
        table->update_row(row_ids[0], {std::int32_t{1}, std::int32_t{40}, std::string("a")});
        // An update that collides with an existing key keeps both entries but not a new
        // distinct key, so the Jet4 distinct-key count drops.
        table->update_row(row_ids[0], {std::int32_t{1}, std::int32_t{20}, std::string("a")});
        table->delete_row(row_ids[1]);
        check(table->row_count_header() == 2,
              "indexed-table delete did not decrement the row count");
        std::vector<std::int32_t> live_codes;
        for (auto cursor = table->rows_by_index("IX_Code");;) {
            const auto row = cursor.next();
            if (!row) {
                break;
            }
            live_codes.push_back(std::get<std::int32_t>(row->at(1)));
        }
        check(live_codes == std::vector<std::int32_t>({20, 20}),
              "index cursor did not reflect the deleted row");
    }

    {
        auto reopened = rea_jackcess::Database::open(path);
        const auto tables = reopened.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Indexed";
        });
        check(table != tables.end(), "reopened database did not expose table Indexed");
        check(table->row_count_header() == 2,
              "reopened indexed table has an unexpected row count");
        auto cursor = table->rows_by_index("IX_Code");
        const std::vector<std::int32_t> expected_codes{20, 20};
        const std::vector<std::int32_t> expected_ids{1, 3};
        for (std::size_t index = 0; index < expected_codes.size(); ++index) {
            const auto row = cursor.next();
            check(row && std::get<std::int32_t>(row->at(1)) == expected_codes[index] &&
                      std::get<std::int32_t>(row->at(0)) == expected_ids[index],
                  "reopened index order did not match the maintained keys");
        }
        check(!cursor.next(), "reopened index returned an unexpected extra entry");
        // Both remaining rows carry the same key, so the distinct-key count is one.
        check(table->indexes()[1].unique_entry_count == 1,
              "reopened index distinct-key count is incorrect");
        check(table->indexes()[0].unique_entry_count == 2,
              "reopened IX_Id distinct-key count is incorrect");
        auto id_cursor = table->rows_by_index("IX_Id");
        const std::vector<std::int32_t> expected_id_order{1, 3};
        for (const std::int32_t expected_id : expected_id_order) {
            const auto row = id_cursor.next();
            check(row && std::get<std::int32_t>(row->at(0)) == expected_id,
                  "IX_Id did not skip the deleted entry");
        }
        check(!id_cursor.next(), "IX_Id returned an unexpected extra entry");
    }

    const auto v2003_path = directory.file("indexed-table-v2003.mdb");
    {
        const auto created = rea_jackcess::Database::create(
            v2003_path,
            rea_jackcess::DatabaseFormat::v2003,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        const auto tables = created.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Indexed";
        });
        check(table != tables.end() && table->indexes().size() == 2,
              "V2003 table creation did not preserve the user indexes");
        check(!table->rows_by_index("IX_Id").next(),
              "V2003 user index unexpectedly contains entries");
    }

    rea_jackcess::TableBuilder unknown_column("UnknownColumn");
    unknown_column.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Missing", "Missing"));
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)rea_jackcess::Database::create(
            directory.file("index-unknown-column.mdb"),
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{unknown_column});
    });

    rea_jackcess::TableBuilder duplicate_index("DuplicateIndex");
    duplicate_index.add_column("Id", rea_jackcess::DataType::int32)
        .add_index(rea_jackcess::IndexBuilder("IX_Same", "Id"))
        .add_index(rea_jackcess::IndexBuilder("ix_same", "Id"));
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        (void)rea_jackcess::Database::create(
            directory.file("index-duplicate-name.mdb"),
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{duplicate_index});
    });
}

void test_long_value_updates_reuse_free_pages(const TemporaryDirectory& directory) {
    const auto path = directory.file("long-value-reuse.mdb");
    rea_jackcess::TableBuilder table_builder("Reuse");
    table_builder.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Memo", rea_jackcess::DataType::memo)
        .add_column("Tail", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }

    std::uint64_t page_count_after_insert = 0;
    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Reuse";
        });
        check(table != tables.end(), "read-write database did not expose table Reuse");

        const auto row_ids = table->add_rows({
            {std::int32_t{1}, rea_jackcess::MemoValue{std::string(300, 'A'), {}}, std::int32_t{7}},
        });
        check(row_ids.size() == 1, "long-value reuse fixture did not insert its row");
        page_count_after_insert = rea_jackcess::DatabaseFile::open(path).page_count();

        // Each value fits several times on the first LVAL page, so Jackcess-style
        // reuse must keep the file from growing on these updates.
        for (const char fill : {'B', 'C', 'D', 'E'}) {
            table->update_row(row_ids.front(),
                              {std::int32_t{1}, rea_jackcess::MemoValue{std::string(300, fill), {}},
                               std::int32_t{7}});
            check(rea_jackcess::DatabaseFile::open(path).page_count() == page_count_after_insert,
                  "a long-value update appended a page instead of reusing free space");
        }
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Reuse";
    });
    check(table != tables.end() && table->row_count_header() == 1,
          "reopened long-value reuse table has an unexpected row count");
    auto rows = table->rows();
    const auto row = rows.next();
    check(row && std::get<rea_jackcess::MemoValue>(row->at(1)).text == std::string(300, 'E') &&
              std::get<std::int32_t>(row->at(2)) == 7,
          "reopened long-value reuse table did not preserve the updated memo");
    check(!rows.next(), "long-value reuse table returned an unexpected extra row");
    check(rea_jackcess::DatabaseFile::open(path).page_count() == page_count_after_insert,
          "reopening the long-value reuse fixture changed its page count");
}

void test_v2003_table_and_row_writes(const TemporaryDirectory& directory) {
    const auto path = directory.file("v2003-typed-rows.mdb");
    rea_jackcess::TableBuilder typed_table("Typed");
    typed_table.add_column("Id", rea_jackcess::DataType::int32)
        .add_column("Flag", rea_jackcess::DataType::boolean)
        .add_column("Name", rea_jackcess::DataType::text)
        .add_column("Note", rea_jackcess::DataType::memo)
        .add_column("Amount", rea_jackcess::DataType::money);
    rea_jackcess::TableBuilder second_table("Second");
    second_table.add_column("Code", rea_jackcess::DataType::int32);
    {
        const auto created = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2003,
            std::vector<rea_jackcess::TableBuilder>{typed_table, second_table});
        const auto tables = created.tables();
        const auto typed = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Typed";
        });
        const auto second = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Second";
        });
        check(typed != tables.end() && typed->columns().size() == 5,
              "V2003 table creation did not preserve the typed schema");
        check(typed->columns()[1].type == rea_jackcess::DataType::boolean &&
                  typed->columns()[1].size == 1 && typed->columns()[1].fixed,
              "V2003 BOOLEAN column did not keep the Jet4 bit layout");
        check(second != tables.end() && second->columns().size() == 1,
              "V2003 multi-table creation did not expose the second table");
        check(!typed->rows().next() && !second->rows().next(),
              "newly created V2003 tables unexpectedly contain rows");
    }

    {
        auto writable = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto typed = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Typed";
        });
        check(typed != tables.end(), "read-write V2003 database did not expose table Typed");
        const auto row_ids = typed->add_rows({
            {std::int32_t{1}, true, std::string("first"),
             rea_jackcess::MemoValue{std::string(12000, 'M'), {}}, rea_jackcess::DecimalValue{"12.50"}},
            {std::int32_t{2}, false, std::string(""),
             rea_jackcess::MemoValue{"", {}}, rea_jackcess::DecimalValue{"-0.0001"}},
        });
        check(row_ids.size() == 2 && typed->row_count_header() == 2,
              "V2003 row insertion returned the wrong row count");
        typed->update_row(row_ids.front(),
                          {std::int32_t{1}, false, std::string(200, 'u'),
                           rea_jackcess::MemoValue{std::string(9000, 'U'), {}},
                           rea_jackcess::DecimalValue{"922337203685477.5807"}});
        check(typed->row_count_header() == 2, "V2003 update changed the table row count");

        const auto second = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Second";
        });
        check(second != tables.end(), "read-write V2003 database did not expose table Second");
        const auto second_ids = second->add_rows({
            {std::int32_t{1}},
            {std::int32_t{2}},
            {std::int32_t{3}},
        });
        check(second_ids.size() == 3 && second->row_count_header() == 3,
              "V2003 second-table insertion returned the wrong row count");
        second->delete_row(second_ids[1]);
        check(second->row_count_header() == 2, "V2003 row deletion did not update the row count");
    }

    auto reopened = rea_jackcess::Database::open(path);
    const auto tables = reopened.tables();
    const auto typed = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
        return candidate.name() == "Typed";
    });
    check(typed != tables.end() && typed->row_count_header() == 2,
          "reopened V2003 database lost table Typed");
    {
        auto rows = typed->rows();
        const auto first = rows.next();
        check(first && !std::get<bool>(first->at(1)) &&
                  std::get<std::string>(first->at(2)) == std::string(200, 'u') &&
                  std::get<rea_jackcess::MemoValue>(first->at(3)).text == std::string(9000, 'U') &&
                  std::get<rea_jackcess::DecimalValue>(first->at(4)).text == "922337203685477.5807",
              "reopened V2003 row did not preserve the updated values");
        const auto second = rows.next();
        check(second && std::get<std::int32_t>(second->at(0)) == 2 &&
                  !std::get<bool>(second->at(1)) && std::get<std::string>(second->at(2)).empty() &&
                  std::get<rea_jackcess::MemoValue>(second->at(3)).text.empty() &&
                  std::get<rea_jackcess::DecimalValue>(second->at(4)).text == "-0.0001",
              "reopened V2003 row did not preserve the initial values");
        check(!rows.next(), "reopened V2003 table returned an unexpected extra row");
    }
    {
        const auto second = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Second";
        });
        check(second != tables.end() && second->row_count_header() == 2,
              "reopened V2003 database lost the second table row count");
        auto rows = second->rows();
        const auto first = rows.next();
        const auto last = rows.next();
        check(first && std::get<std::int32_t>(first->at(0)) == 1 && last &&
                  std::get<std::int32_t>(last->at(0)) == 3,
              "reopened V2003 second table did not keep the surviving rows");
        check(!rows.next(), "reopened V2003 second table returned an unexpected extra row");
    }
}

void test_unknown_access_version_row_writes_are_rejected(const TemporaryDirectory& directory) {
    // A known but foreign AccessVersion marker is still rejected before any page changes.
    const auto path = directory.file("unknown-access-version-row-write.mdb");
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
    mutate_unique_utf16le_string(path, "08.50", "99.99");
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_version, [&] {
        (void)rea_jackcess::DatabaseFile::open(path).file_format();
    });
    const auto before_write = read_bytes(path);

    {
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
    }
    check(read_bytes(path) == before_write,
          "an unknown Jet4 AccessVersion version modified the database before rejection");

    // A file whose MSysDb carries no readable AccessVersion (ArcGIS-style) is accepted: the
    // Jet4 page layout is already validated from the file header.
    const auto missing_path = directory.file("missing-access-version-row-write.mdb");
    {
        const auto created = rea_jackcess::Database::create(
            missing_path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table_builder});
        (void)created;
    }
    mutate_unique_utf16le_string(missing_path, "AccessVersion", "UnknownVersnX");
    check(rea_jackcess::DatabaseFile::open(missing_path).file_format() == rea_jackcess::FileFormat::generic_jet4,
          "markerless Jet4 was not classified as generic");
    {
        auto writable = rea_jackcess::Database::open(missing_path,
                                                     rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end(), "read-write database did not expose the markerless fixture");
        (void)table->add_row({std::int32_t{7}});
        check(table->row_count_header() == 1,
              "writing to a Jet4 file without AccessVersion did not raise the row count");
    }
    {
        const auto reopened = rea_jackcess::Database::open(missing_path);
        const auto tables = reopened.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "T";
        });
        check(table != tables.end() && table->row_count_header() == 1,
              "the markerless Jet4 file did not keep its inserted row");
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

    rea_jackcess::TableBuilder v2003_unsupported_table("Unsupported");
    v2003_unsupported_table.add_column("Amount", rea_jackcess::DataType::numeric);
    expect_error(rea_jackcess::DatabaseErrorCode::unsupported_operation, [&] {
        auto database = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2003,
            std::vector<rea_jackcess::TableBuilder>{v2003_unsupported_table});
        (void)database;
    });
    check(read_bytes(path) == original_bytes,
          "unsupported V2003 table schema modified the existing destination");
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

// A definition that needs more than one page is laid out across a continuation chain, which the
// reopened catalog has to follow back to the same columns.
void test_multipage_table_definition_is_supported(const TemporaryDirectory& directory) {
    const auto path = directory.file("multipage-table-definition.mdb");
    const auto table = table_with_long_column_names(27);
    {
        const auto database = rea_jackcess::Database::create(
            path,
            rea_jackcess::DatabaseFormat::v2000,
            std::vector<rea_jackcess::TableBuilder>{table});
        const auto tables = database.tables();
        const auto created = std::find_if(tables.begin(), tables.end(), [](const auto& value) {
            return value.name() == "Large";
        });
        check(created != tables.end() && created->columns().size() == 27,
              "multi-page table definition did not preserve every column");
    }

    std::uint32_t next_page = 0;
    std::uint32_t definition_size = 0;
    {
        const auto file = rea_jackcess::detail::PageFile::open(path);
        const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
        const auto created = std::find_if(catalog.tables.begin(), catalog.tables.end(), [](const auto& value) {
            return value.name == "Large";
        });
        check(created != catalog.tables.end(), "multi-page table definition was not catalogued");
        const auto page = file.read_page(created->definition_page_number);
        next_page = static_cast<std::uint32_t>(page[4]) | (static_cast<std::uint32_t>(page[5]) << 8) |
                    (static_cast<std::uint32_t>(page[6]) << 16) |
                    (static_cast<std::uint32_t>(page[7]) << 24);
        definition_size = static_cast<std::uint32_t>(page[8]) |
                          (static_cast<std::uint32_t>(page[9]) << 8) |
                          (static_cast<std::uint32_t>(page[10]) << 16) |
                          (static_cast<std::uint32_t>(page[11]) << 24);
    }
    check(next_page != 0, "the wide table definition did not get a continuation page");
    check(definition_size > 4096, "the wide table definition should exceed one page");

    // Writing a row proves the continuation chain is used by the row writer as well.
    {
        auto database = rea_jackcess::Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
        auto reopened = get_table(database, "Large");
        std::vector<rea_jackcess::Value> row;
        for (std::size_t index = 0; index < reopened.columns().size(); ++index) {
            row.push_back(std::int32_t{static_cast<std::int32_t>(index)});
        }
        reopened.add_row(row);
        check(reopened.row_count_header() == 1, "a row was not appended to the wide table");
    }
    const auto reopened = rea_jackcess::Database::open(path);
    const auto table_again = get_table(reopened, "Large");
    check(table_again.columns().size() == 27 && table_again.row_count_header() == 1,
          "the reopened wide table lost columns or rows");
}

void test_add_index_to_existing_table(const TemporaryDirectory& directory,
                                      rea_jackcess::DatabaseFormat format,
                                      const std::string& filename) {
    using namespace rea_jackcess;
    const auto path = directory.file(filename);
    Database::create(path, format, {TableBuilder("Items").add_column("Key", DataType::int32)
                                                        .add_column("Payload", DataType::memo)});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = get_table(database, "Items");
        std::vector<std::vector<Value>> rows;
        for (std::int32_t value = 1000; value > 0; --value) {
            rows.push_back({value, std::monostate{}});
        }
        const auto ids = table.add_rows(rows);
        const auto duplicate = table.add_row({std::int32_t{50}, std::monostate{}});
        const auto size_before = std::filesystem::file_size(path);
        expect_error(DatabaseErrorCode::constraint_violation, [&] {
            table.add_index(IndexBuilder("NewKey", "Key", true, true));
        });
        check(std::filesystem::file_size(path) == size_before && table.indexes().empty(),
              "creating a unique index over duplicate values changed the database");
        table.delete_row(duplicate);

        table.add_index(IndexBuilder("NewKey", "key", true, true));
        const auto first = table.find_row_by_index("NewKey", std::int32_t{1000});
        check(table.indexes().size() == 1 && first && first->id() == ids.front(),
              "new index did not find pre-existing rows");
        auto cursor = table.rows_by_index("NewKey");
        for (std::size_t position = 0; position < ids.size(); ++position) {
            const auto row = cursor.next();
            check(row && row->id() == ids[position], "new index traversal lost descending key order");
        }
        check(!cursor.next(), "new index traversal returned extra rows");

        const auto size_with_tree = std::filesystem::file_size(path);
        table.add_index(IndexBuilder("AliasKey", "Key", true, true));
        check(table.indexes().size() == 2 &&
              table.indexes()[0].physical_index_number == table.indexes()[1].physical_index_number &&
              std::filesystem::file_size(path) == size_with_tree,
              "equivalent index did not reuse the physical tree");
        expect_error(DatabaseErrorCode::unsupported_operation, [&] {
            table.add_index(IndexBuilder("newkey", "Key"));
        });
        table.update_row(ids.front(), {std::int32_t{1001}, std::monostate{}});
        table.delete_row(ids.back());
    }
    auto database = Database::open(path);
    auto table = get_table(database, "Items");
    check(table.indexes().size() == 2 &&
          table.find_row_by_index("AliasKey", std::int32_t{1001}).has_value() &&
          !table.find_row_by_index("NewKey", std::int32_t{1}),
          "new index descriptors or maintained entries did not survive reopen");
    expect_error(DatabaseErrorCode::unsupported_operation, [&] {
        table.add_index(IndexBuilder("ReadOnlyKey", "Key"));
    });
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_create_formats_and_replace(directory);
        test_save_database_properties(directory);
        test_save_table_properties(directory);
        test_numeric_column_defaults(directory);
        test_column_validation_between(directory);
        test_floating_column_validation(directory);
        test_money_validation(directory);
        test_table_validation_between(directory);
        test_table_validation_rule(directory);
        test_column_validator_factory(directory);
        test_create_file_link(directory);
        test_open_odbc_linked_table_custom_source(directory, rea_jackcess::DatabaseFormat::v2000);
        test_open_odbc_linked_table_custom_source(directory, rea_jackcess::DatabaseFormat::v2003);
        test_open_linked_table_chain_and_cycle(directory);
        test_open_custom_linked_table_snapshots(directory, rea_jackcess::DatabaseFormat::v2000);
        test_open_custom_linked_table_snapshots(directory, rea_jackcess::DatabaseFormat::v2003);
        test_missing_parent(directory);
        test_create_empty_tables(directory);
        test_add_rows_to_unindexed_long_table(directory);
        test_add_rows_with_fixed_numeric_types(directory);
        test_create_and_write_money_columns(directory);
        test_create_and_write_guid_columns(directory);
        test_create_and_write_boolean_columns(directory);
        test_boolean_columns_across_null_mask_bytes(directory);
        test_boolean_only_table(directory);
        test_boolean_with_long_value_columns(directory);
        test_general_legacy_key_encoding();
        test_text_sort_order(directory);
        test_add_rows_with_text_and_binary_columns(directory);
        test_memo_and_ole_long_value_writes(directory, rea_jackcess::DatabaseFormat::v2000);
        test_memo_and_ole_long_value_writes(directory, rea_jackcess::DatabaseFormat::v2003);
        test_read_error_handler(directory, rea_jackcess::DatabaseFormat::v2000);
        test_read_error_handler(directory, rea_jackcess::DatabaseFormat::v2003);
        test_long_value_release_after_global_map_promotion(directory,
                                                          rea_jackcess::DatabaseFormat::v2000);
        test_long_value_release_after_global_map_promotion(directory,
                                                          rea_jackcess::DatabaseFormat::v2003);
        test_later_oversized_batch_row_is_rejected(directory);
        test_create_tables_with_user_indexes(directory);
        test_add_index_to_existing_table(directory, rea_jackcess::DatabaseFormat::v2000, "index-v2000.mdb");
        test_add_index_to_existing_table(directory, rea_jackcess::DatabaseFormat::v2003, "index-v2003.mdb");
        test_index_maintenance_across_row_moves(directory);
        test_money_index_keys(directory);
        test_index_seek_and_range(directory);
        test_index_leaf_merge_on_delete(directory, false);
        test_index_leaf_merge_on_delete(directory, true);
        test_global_map_growth(directory, false);
        test_global_map_growth(directory, true);
        test_owned_map_growth(directory);
        test_guid_index_keys(directory);
        test_descending_index_keys(directory);
        test_binary_index_keys(directory);
        test_text_index_keys(directory);
        test_text_key_code_tables();
        test_supported_text_sort_orders(directory);
        test_memo_index_support(directory);
        test_index_null_constraints(directory);
        test_create_composite_primary_key(directory);
        test_composite_index_maintenance(directory);
        test_composite_index_maintenance(directory, rea_jackcess::DatabaseFormat::v2003);
        test_invalid_composite_index_definitions(directory);
        test_binary_index_prefix(directory);
        test_binary_index_split(directory);
        test_append_with_reference_usage_map(directory);
        test_index_prefix_contraction(directory);
        test_index_delete_all_rows(directory);
        test_index_split_and_multi_leaf_maintenance(directory);
        test_index_three_level_tree(directory);
        test_index_key_types_and_rejections(directory);
        test_unique_index_keys(directory);
        test_long_value_updates_reuse_free_pages(directory);
        test_v2003_table_and_row_writes(directory);
        test_unknown_access_version_row_writes_are_rejected(directory);
        test_row_writer_rejects_unsupported_column_layouts(directory);
        test_delete_rows_from_unindexed_table(directory);
        test_unsupported_table_schema_preserves_destination(directory);
        test_table_definition_page_boundary_is_supported(directory);
        test_multipage_table_definition_is_supported(directory);
        std::cout << "database creation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "database creation tests failed: " << error.what() << '\n';
        return 1;
    }
}

#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const bool single_table = argc == 3 && std::string(argv[2]) == "--single";
    const bool maximum_definition = argc == 3 && std::string(argv[2]) == "--maximum-definition";
    const bool batch_rows = argc == 3 && std::string(argv[2]) == "--batch-rows";
    const bool update_row = argc == 3 && std::string(argv[2]) == "--update-row";
    const bool delete_row = argc == 3 && std::string(argv[2]) == "--delete-row";
    const bool fixed_numeric_rows = argc == 3 && std::string(argv[2]) == "--fixed-numeric-rows";
    const bool variable_rows = argc == 3 && std::string(argv[2]) == "--variable-rows";
    const bool writes_rows = batch_rows || update_row || delete_row || fixed_numeric_rows || variable_rows;
    if (argc != 2 && !single_table && !maximum_definition && !writes_rows) {
        std::cerr << "Usage: create_tables_for_jackcess_check <database.mdb> "
                     "[--single|--maximum-definition|--batch-rows|--update-row|--delete-row|--fixed-numeric-rows|--variable-rows]\n";
        return 2;
    }

    try {
        rea_jackcess::TableBuilder first_table("T");
        first_table.add_column("Id", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder second_table("Table_1");
        second_table.add_column("Code", rea_jackcess::DataType::int32)
            .add_column("GroupId", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder batch_table("T");
        batch_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("GroupId", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder fixed_numeric_table("T");
        fixed_numeric_table.add_column("ByteValue", rea_jackcess::DataType::byte)
            .add_column("ShortValue", rea_jackcess::DataType::int16)
            .add_column("LongValue", rea_jackcess::DataType::int32)
            .add_column("SingleValue", rea_jackcess::DataType::float32)
            .add_column("DoubleValue", rea_jackcess::DataType::float64);

        rea_jackcess::TableBuilder variable_table("Variable");
        variable_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Name", rea_jackcess::DataType::text)
            .add_column("Payload", rea_jackcess::DataType::binary)
            .add_column("Note", rea_jackcess::DataType::text);

        std::vector<rea_jackcess::TableBuilder> tables{
            variable_rows ? variable_table
                          : (fixed_numeric_rows ? fixed_numeric_table : (writes_rows ? batch_table : first_table))};
        if (!single_table) {
            if (maximum_definition) {
                rea_jackcess::TableBuilder large_table("Large");
                for (char suffix = 'A'; suffix <= 'Z'; ++suffix) {
                    large_table.add_column("C" + std::string(61, 'A') + suffix + '0',
                                           rea_jackcess::DataType::int32);
                }
                tables = {large_table};
            } else if (!writes_rows) {
                tables.push_back(second_table);
            }
        }
        {
            const auto database = rea_jackcess::Database::create(
                argv[1], rea_jackcess::DatabaseFormat::v2000, tables);
            std::size_t user_table_count = 0;
            for (const auto& table : database.tables()) {
                user_table_count += table.is_user_table() ? 1 : 0;
            }
            std::cout << "Created " << user_table_count << " Jet4 user table(s) in "
                      << std::string(argv[1]) << '\n';
        }

        if (writes_rows) {
            auto database = rea_jackcess::Database::open(
                argv[1], rea_jackcess::DatabaseOpenMode::read_write);
            auto tables_after_open = database.tables();
            const auto table = std::find_if(tables_after_open.begin(), tables_after_open.end(),
                                            [&](const auto& candidate) {
                                                return candidate.name() == (variable_rows ? "Variable" : "T");
                                            });
            if (table == tables_after_open.end()) {
                throw std::runtime_error("Read-write reopen did not discover the expected table");
            }

            if (variable_rows) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, std::string("A"), rea_jackcess::BlobValue{{0, 0x7f, 0xff}}, std::string("🔥")},
                    {std::int32_t{2}, std::string{}, rea_jackcess::BlobValue{}, std::monostate{}},
                    {std::int32_t{3}, std::monostate{}, rea_jackcess::BlobValue{{0x80}}, std::string("雪")},
                    {std::int32_t{4}, std::string(255, 'x'),
                     rea_jackcess::BlobValue{std::vector<std::uint8_t>(255, 0xa5)}, std::string(255, 'y')},
                });
                table->update_row(row_ids.front(),
                                  {std::int32_t{10}, std::string("B"),
                                   rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
                std::cout << "Wrote UTF-8 TEXT and BINARY rows, including empty and NULL values\n";
            } else if (fixed_numeric_rows) {
                const auto row_ids = table->add_rows({
                    {std::int8_t{-128}, std::int16_t{-32768}, std::int32_t{-2147483647 - 1}, -1.25F, 1.0 / 3.0},
                    {std::int8_t{127}, std::int16_t{32767}, std::int32_t{2147483647}, 3.5F, 2.5},
                    {std::int8_t{1}, std::int16_t{-10}, std::int32_t{100}, 0.25F, 42.0},
                });
                table->update_row(row_ids.back(),
                                  {std::int8_t{-1}, std::int16_t{0}, std::int32_t{123456}, 12.5F, -0.5});
                std::cout << "Wrote fixed BYTE, SHORT, LONG, SINGLE, and DOUBLE rows\n";
            } else if (batch_rows) {
                std::vector<std::vector<rea_jackcess::Value>> rows;
                rows.reserve(500);
                for (std::int32_t value = 0; value < 500; ++value) {
                    rows.push_back({value, value + 1000});
                }
                const auto row_ids = table->add_rows(rows);
                if (row_ids.size() != rows.size()) {
                    throw std::runtime_error("Batch insertion returned the wrong RowId count");
                }
                (void)table->add_row({std::int32_t{501}, std::monostate{}});
                std::cout << "Inserted 500 rows across Jet4 data pages and one nullable row\n";
            } else if (update_row) {
                const auto row_id = table->add_row({std::int32_t{10}, std::int32_t{20}});
                table->update_row(row_id, {std::int32_t{11}, std::monostate{}});
                std::cout << "Updated one fixed-width Jet4 row in place\n";
            } else {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, std::int32_t{10}},
                    {std::int32_t{2}, std::int32_t{20}},
                    {std::int32_t{3}, std::int32_t{30}},
                });
                table->delete_row(row_ids[1]);
                std::cout << "Deleted one Jet4 row and retained the surrounding rows\n";
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Table creation fixture failed: " << error.what() << '\n';
        return 1;
    }
}

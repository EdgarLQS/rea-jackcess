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
    if (argc != 2 && !single_table && !maximum_definition && !batch_rows) {
        std::cerr << "Usage: create_tables_for_jackcess_check <database.mdb> "
                     "[--single|--maximum-definition|--batch-rows]\n";
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

        std::vector<rea_jackcess::TableBuilder> tables{batch_rows ? batch_table : first_table};
        if (!single_table) {
            if (maximum_definition) {
                rea_jackcess::TableBuilder large_table("Large");
                for (char suffix = 'A'; suffix <= 'Z'; ++suffix) {
                    large_table.add_column("C" + std::string(61, 'A') + suffix + '0',
                                           rea_jackcess::DataType::int32);
                }
                tables = {large_table};
            } else if (!batch_rows) {
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

        if (batch_rows) {
            auto database = rea_jackcess::Database::open(
                argv[1], rea_jackcess::DatabaseOpenMode::read_write);
            auto tables_after_open = database.tables();
            const auto table = std::find_if(tables_after_open.begin(), tables_after_open.end(),
                                            [](const auto& candidate) {
                                                return candidate.name() == "T";
                                            });
            if (table == tables_after_open.end()) {
                throw std::runtime_error("Read-write reopen did not discover table T");
            }

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
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Table creation fixture failed: " << error.what() << '\n';
        return 1;
    }
}

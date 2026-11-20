#include "rea_jackcess/database.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const bool single_table = argc == 3 && std::string(argv[2]) == "--single";
    const bool maximum_definition = argc == 3 && std::string(argv[2]) == "--maximum-definition";
    if (argc != 2 && !single_table && !maximum_definition) {
        std::cerr << "Usage: create_tables_for_jackcess_check <database.mdb> [--single|--maximum-definition]\n";
        return 2;
    }

    try {
        rea_jackcess::TableBuilder first_table("T");
        first_table.add_column("Id", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder second_table("Table_1");
        second_table.add_column("Code", rea_jackcess::DataType::int32)
            .add_column("GroupId", rea_jackcess::DataType::int32);

        std::vector<rea_jackcess::TableBuilder> tables{first_table};
        if (!single_table) {
            if (maximum_definition) {
                rea_jackcess::TableBuilder large_table("Large");
                for (char suffix = 'A'; suffix <= 'Z'; ++suffix) {
                    large_table.add_column("C" + std::string(61, 'A') + suffix + '0',
                                           rea_jackcess::DataType::int32);
                }
                tables = {large_table};
            } else {
                tables.push_back(second_table);
            }
        }
        const auto database = rea_jackcess::Database::create(
            argv[1], rea_jackcess::DatabaseFormat::v2000, tables);
        std::size_t user_table_count = 0;
        for (const auto& table : database.tables()) {
            user_table_count += table.is_user_table() ? 1 : 0;
        }
        std::cout << "Created " << user_table_count << " Jet4 user table(s) in "
                  << std::string(argv[1]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Table creation fixture failed: " << error.what() << '\n';
        return 1;
    }
}

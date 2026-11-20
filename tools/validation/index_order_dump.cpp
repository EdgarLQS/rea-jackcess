#include "rea_jackcess/database.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: index_order_dump <database-file>\n";
        return 2;
    }

    try {
        const auto database = rea_jackcess::Database::open(argv[1]);
        std::map<std::string, std::vector<std::string>> sequences;
        for (const auto& table : database.tables()) {
            if (!table.is_user_table() || table.is_linked()) {
                continue;
            }
            for (const auto& index : table.indexes()) {
                auto cursor = table.rows_by_index(index.name);
                auto& sequence = sequences[table.name() + "\t" + index.name];
                while (const auto row = cursor.next()) {
                    const auto& id = row->id();
                    sequence.push_back(std::to_string(id.page_number) + ":" +
                                       std::to_string(id.row_number));
                }
            }
        }

        for (const auto& [index, sequence] : sequences) {
            std::cout << index << '\t';
            for (std::size_t position = 0; position < sequence.size(); ++position) {
                if (position != 0) {
                    std::cout << ',';
                }
                std::cout << sequence[position];
            }
            std::cout << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

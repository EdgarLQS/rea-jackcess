#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: create_memo_index_fixture <database.mdb> <V2000|V2003>\n";
        return 2;
    }

    try {
        const std::string format_name(argv[2]);
        rea_jackcess::DatabaseFormat format;
        if (format_name == "V2000") {
            format = rea_jackcess::DatabaseFormat::v2000;
        } else if (format_name == "V2003") {
            format = rea_jackcess::DatabaseFormat::v2003;
        } else {
            throw std::invalid_argument("Format must be V2000 or V2003");
        }

        const std::string long_a = std::string(700, 'x') + "A";
        const std::string long_b = std::string(700, 'y') + "B";
        rea_jackcess::TableBuilder builder("MemoIndexed");
        builder.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Note", rea_jackcess::DataType::memo)
            .add_index(rea_jackcess::IndexBuilder("IX_Note", "Note", true))
            .add_index(rea_jackcess::IndexBuilder("IX_NoteDesc", "Note", true, true));
        (void)rea_jackcess::Database::create(argv[1], format, {builder});

        auto database = rea_jackcess::Database::open(
            argv[1], rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "MemoIndexed";
        });
        if (table == tables.end()) {
            throw std::runtime_error("Created MemoIndexed table was not discoverable");
        }

        const auto rows = table->add_rows({
            {std::int32_t{1}, rea_jackcess::MemoValue{"Alpha", {}}},
            {std::int32_t{2}, rea_jackcess::MemoValue{"Bravo", {}}},
            {std::int32_t{3}, rea_jackcess::MemoValue{"Charlie", {}}},
            {std::int32_t{4}, rea_jackcess::MemoValue{long_a, {}}},
            {std::int32_t{5}, rea_jackcess::MemoValue{long_b, {}}},
        });
        table->update_row(rows[1], {std::int32_t{2}, rea_jackcess::MemoValue{"Delta", {}}});
        table->delete_row(rows[2]);

        std::cout << "Created " << format_name << " Memo index fixture at " << argv[1] << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Memo index fixture creation failed: " << error.what() << '\n';
        return 1;
    }
}

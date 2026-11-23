#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: create_date_range_fixture <output.mdb> <v2000|v2003>\n";
        return 2;
    }

    try {
        const std::filesystem::path output(argv[1]);
        if (std::filesystem::exists(output)) {
            throw std::runtime_error("refusing to replace an existing output file");
        }
        const auto format = std::string(argv[2]) == "v2000"
                                ? rea_jackcess::DatabaseFormat::v2000
                                : std::string(argv[2]) == "v2003"
                                      ? rea_jackcess::DatabaseFormat::v2003
                                      : throw std::invalid_argument("format must be v2000 or v2003");
        {
            auto created = rea_jackcess::Database::create(
                output, format,
                {rea_jackcess::TableBuilder("Dated")
                     .add_column("When", rea_jackcess::DataType::short_date_time)
                     .add_column("Label", rea_jackcess::DataType::text)});
            (void)created;
        }

        auto database = rea_jackcess::Database::open(output, rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = database.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& candidate) {
            return candidate.name() == "Dated";
        });
        if (table == tables.end()) {
            throw std::runtime_error("created database is missing table Dated");
        }
        table->add_rows({
            {rea_jackcess::DateTimeValue{"1765-06-14T12:45"}, std::string("pre-epoch")},
            {rea_jackcess::DateTimeValue{"0100-06-14T12:45"}, std::string("earliest")},
            {rea_jackcess::DateTimeValue{"1899-12-30T00:00"}, std::string("epoch")},
            {rea_jackcess::DateTimeValue{"2024-03-04T05:06:07"}, std::string("modern")},
        });
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "failed to create date-range fixture: " << error.what() << '\n';
        return 1;
    }
}

#include "rea_jackcess/csv.hpp"
#include "rea_jackcess/database.hpp"
#include "rea_jackcess/database_file.hpp"
#include "rea_jackcess/version.hpp"
#include "catalog/system_catalog.hpp"
#include "catalog/table_definition.hpp"
#include "page/page_file.hpp"
#include "canonical_json.hpp"

#include <iostream>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace {

void write_json_string(std::string_view value) {
    constexpr char hex_digits[] = "0123456789abcdef";
    std::cout << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                std::cout << "\\\"";
                break;
            case '\\':
                std::cout << "\\\\";
                break;
            case '\b':
                std::cout << "\\b";
                break;
            case '\f':
                std::cout << "\\f";
                break;
            case '\n':
                std::cout << "\\n";
                break;
            case '\r':
                std::cout << "\\r";
                break;
            case '\t':
                std::cout << "\\t";
                break;
            default:
                if (character < 0x20) {
                    std::cout << "\\u00" << hex_digits[character >> 4] << hex_digits[character & 0x0f];
                } else {
                    std::cout << static_cast<char>(character);
                }
                break;
        }
    }
    std::cout << '"';
}

template <typename T>
void write_optional_number(const std::optional<T>& value) {
    if (value) {
        std::cout << *value;
    } else {
        std::cout << "null";
    }
}

void write_optional_string(const std::optional<std::string>& value) {
    if (value) {
        write_json_string(*value);
    } else {
        std::cout << "null";
    }
}

}

int main(int argc, char* argv[]) {
    const std::string_view option = argc >= 2 ? argv[1] : "";
    if (argc == 1 || option == "--help") {
        std::cout << "Usage: rea-jackcess [--help | --version | create <database-file> <v2000|v2003> | "
                     "inspect <database-file> | canonical <database-file> | "
                     "canonical <database-file> [--code-page <n>] | "
                     "export <database-file> <table> <csv-file> [--header] | "
                     "import <database-file> <table> <csv-file> [--header]\n";
        return 0;
    }
    if (option == "--version") {
        std::cout << "rea-jackcess " << rea_jackcess::version() << '\n';
        return 0;
    }
    if (argc == 4 && option == "create") {
        rea_jackcess::DatabaseFormat format;
        const std::string_view format_option = argv[3];
        if (format_option == "v2000") {
            format = rea_jackcess::DatabaseFormat::v2000;
        } else if (format_option == "v2003") {
            format = rea_jackcess::DatabaseFormat::v2003;
        } else {
            std::cerr << "Unsupported database format: " << format_option << '\n';
            return 2;
        }

        try {
            const auto database = rea_jackcess::Database::create(argv[2], format);
            const auto file = rea_jackcess::DatabaseFile::open(database.path());
            const char* format_name = format == rea_jackcess::DatabaseFormat::v2000 ? "V2000" : "V2003";
            std::cout << "Created empty Jet4 " << format_name << " database: " << database.path().string()
                      << " (" << file.page_count() << " pages)\n";
            return 0;
        } catch (const rea_jackcess::DatabaseError& error) {
            std::cerr << "Cannot create database: " << error.what();
            if (!error.path().empty()) {
                std::cerr << " [file: " << error.path().string() << ']';
            }
            std::cerr << '\n';
            return 1;
        }
    }
    if ((argc == 3 || argc == 5) && option == "canonical") {
        std::optional<std::uint16_t> code_page;
        if (argc == 5) {
            if (std::string_view(argv[3]) != "--code-page") {
                std::cerr << "Unknown canonical option: " << argv[3] << '\n';
                return 2;
            }
            const long parsed = std::strtol(argv[4], nullptr, 10);
            if (parsed <= 0 || parsed > 65535) {
                std::cerr << "Invalid code page: " << argv[4] << '\n';
                return 2;
            }
            code_page = static_cast<std::uint16_t>(parsed);
        }
        try {
            const auto database = rea_jackcess::detail::PageFile::open(argv[2], {code_page, {}});
            rea_jackcess::detail::write_canonical_json(std::cout, database, database.path());
            return 0;
        } catch (const rea_jackcess::DatabaseError& error) {
            std::cerr << "Cannot write canonical database dump: " << error.what();
            if (!error.path().empty()) {
                std::cerr << " [file: " << error.path().string();
                if (error.page_number()) {
                    std::cerr << ", page: " << *error.page_number();
                }
                std::cerr << ']';
            }
            std::cerr << '\n';
            return 1;
        }
    }
    if (argc == 3 && option == "inspect") {
        try {
            const auto database = rea_jackcess::detail::PageFile::open(argv[2]);
            const auto& format = database.format();
            std::cout << "{\"format\":";
            write_json_string(format.name());
            std::cout << ",\"pageSize\":" << format.page_size << ",\"pageCount\":" << database.page_count();
            if (format.version == rea_jackcess::EngineVersion::jet4) {
                const auto catalog = rea_jackcess::detail::read_table_definition(database, 2);
                const auto objects = rea_jackcess::detail::read_system_catalog_objects(database, catalog);
                std::cout << ",\"systemCatalog\":{\"page\":2,\"rowCount\":" << catalog.row_count
                          << ",\"columnCount\":" << catalog.column_count
                          << ",\"indexCount\":" << catalog.logical_index_count
                          << ",\"definitionPages\":" << (catalog.continuation_page_count + 1)
                          << ",\"columns\":[";
                for (std::size_t index = 0; index < catalog.columns.size(); ++index) {
                    if (index != 0) {
                        std::cout << ',';
                    }
                    const auto& column = catalog.columns[index];
                    std::cout << "{\"name\":";
                    write_json_string(column.name);
                    std::cout << ",\"type\":" << static_cast<unsigned int>(column.type)
                              << ",\"columnNumber\":" << column.column_number
                              << ",\"columnId\":" << column.column_id << '}';
                }
                std::cout << "],\"objects\":[";
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    if (index != 0) {
                        std::cout << ',';
                    }
                    const auto& object = objects[index];
                    std::cout << "{\"id\":";
                    write_optional_number(object.id);
                    std::cout << ",\"parentId\":";
                    write_optional_number(object.parent_id);
                    std::cout << ",\"name\":";
                    write_optional_string(object.name);
                    std::cout << ",\"type\":";
                    write_optional_number(object.type);
                    std::cout << ",\"flags\":";
                    write_optional_number(object.flags);
                    std::cout << '}';
                }
                std::cout << "]}";
            }
            std::cout << "}\n";
            return 0;
        } catch (const rea_jackcess::DatabaseError& error) {
            std::cerr << "Cannot inspect database: " << error.what();
            if (!error.path().empty()) {
                std::cerr << " [file: " << error.path().string();
                if (error.page_number()) {
                    std::cerr << ", page: " << *error.page_number();
                }
                const auto& context = error.context();
                if (context.table_name) {
                    std::cerr << ", table: " << *context.table_name;
                }
                if (context.row_number) {
                    std::cerr << ", row: " << *context.row_number;
                }
                if (context.column_number) {
                    std::cerr << ", column: " << *context.column_number;
                }
                if (context.column_name) {
                    std::cerr << " (" << *context.column_name << ')';
                }
                std::cerr << ']';
            }
            std::cerr << '\n';
            return 1;
        }
    }

    if ((argc == 5 || argc == 6) && (option == "export" || option == "import")) {
        rea_jackcess::CsvOptions options;
        if (argc == 6) {
            if (std::string_view(argv[5]) != "--header") {
                std::cerr << "Unknown export/import option: " << argv[5] << '\n';
                return 2;
            }
            options.header = true;
        }
        try {
            const auto mode = option == "import" ? rea_jackcess::DatabaseOpenMode::read_write
                                                 : rea_jackcess::DatabaseOpenMode::read_only;
            auto database = rea_jackcess::Database::open(argv[2], mode);
            std::optional<rea_jackcess::Table> table;
            for (const auto& candidate : database.tables()) {
                if (candidate.name() == argv[3] && !candidate.is_linked()) {
                    table = candidate;
                    break;
                }
            }
            if (!table) {
                std::cerr << "Table is unavailable or not a local table: " << argv[3] << '\n';
                return 2;
            }
            if (option == "export") {
                rea_jackcess::export_table_csv(*table, argv[4], options);
                std::cout << "Exported " << table->name() << " to " << argv[4] << '\n';
            } else {
                const auto rows = rea_jackcess::import_table_csv(*table, argv[4], options);
                std::cout << "Imported " << rows << " rows into " << table->name() << '\n';
            }
            return 0;
        } catch (const std::invalid_argument& error) {
            std::cerr << "Cannot convert the CSV data: " << error.what() << '\n';
            return 1;
        } catch (const rea_jackcess::DatabaseError& error) {
            std::cerr << "CSV " << option << " failed: " << error.what();
            if (!error.path().empty()) {
                std::cerr << " [file: " << error.path().string() << ']';
            }
            std::cerr << '\n';
            return 1;
        }
    }

    std::cerr << "Invalid arguments. Run rea-jackcess --help for usage.\n";
    return 2;
}

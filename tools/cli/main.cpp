#include "rea_jackcess/database_file.hpp"
#include "rea_jackcess/version.hpp"
#include "catalog/system_catalog.hpp"
#include "catalog/table_definition.hpp"
#include "page/page_file.hpp"

#include <iostream>
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
        std::cout << "Usage: rea-jackcess [--help | --version | inspect <database-file>]\n";
        return 0;
    }
    if (option == "--version") {
        std::cout << "rea-jackcess " << rea_jackcess::version() << '\n';
        return 0;
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
                std::cerr << ']';
            }
            std::cerr << '\n';
            return 1;
        }
    }

    std::cerr << "Invalid arguments. Run rea-jackcess --help for usage.\n";
    return 2;
}

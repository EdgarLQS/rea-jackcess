#include <rea_jackcess/database.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace rea_jackcess;

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto format = std::string(argv[2]) == "v2003" ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    const std::filesystem::path path(argv[1]);
    Database::create(path, format,
                     {TableBuilder("BinaryValues")
                          .add_column("Id", DataType::int32)
                          .add_column("Payload", DataType::binary)});
    const BlobValue serialized_hello{{0xac, 0xed, 0x00, 0x05, 0x74, 0x00, 0x05,
                                      0x68, 0x65, 0x6c, 0x6c, 0x6f}};
    const BlobValue serialized_unicode{{0xac, 0xed, 0x00, 0x05, 0x74, 0x00, 0x09,
                                        0x41, 0xc0, 0x80, 0xed, 0xa0, 0xbd,
                                        0xed, 0xb8, 0x80}};
    const BlobValue raw_bytes{{0x00, 0xff, 0x10}};
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    for (auto table : database.tables()) {
        if (table.is_user_table() && table.name() == "BinaryValues") {
            table.add_rows({{std::int32_t{1}, serialized_hello},
                            {std::int32_t{2}, serialized_unicode},
                            {std::int32_t{3}, raw_bytes}});
            return 0;
        }
    }
    return 1;
}

#include "data_page.hpp"

#include "page_types.hpp"

#include <cstddef>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t jet4_row_count_offset = 12;
constexpr std::uint64_t jet4_row_directory_offset = 14;
constexpr std::uint64_t jet3_row_count_offset = 8;
constexpr std::uint64_t jet3_row_directory_offset = 10;
constexpr std::uint16_t row_offset_mask = 0x1fff;
constexpr std::uint16_t deleted_row_mask = 0x8000;
constexpr std::uint16_t overflow_row_mask = 0x4000;

[[noreturn]] void corrupted_page(const PageFile& file,
                                 std::uint64_t page_number,
                                 const char* reason) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, reason, file.path(), page_number);
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

}

DataPage parse_data_page(const PageFile& file,
                         std::uint64_t page_number,
                         const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != file.format().page_size) {
        corrupted_page(file, page_number, "Data page has an invalid byte length");
    }
    if (bytes[0] != static_cast<std::uint8_t>(PageType::data)) {
        corrupted_page(file, page_number, "Expected a data page");
    }

    const bool jet3 = file.format().version == EngineVersion::jet3;
    const std::uint64_t row_count_offset = jet3 ? jet3_row_count_offset : jet4_row_count_offset;
    const std::uint64_t row_directory_offset =
        jet3 ? jet3_row_directory_offset : jet4_row_directory_offset;
    const std::uint16_t row_count = read_u16(bytes, row_count_offset);
    const std::uint64_t directory_end = row_directory_offset + static_cast<std::uint64_t>(row_count) * 2;
    if (row_count > 255 || directory_end > bytes.size()) {
        corrupted_page(file, page_number, "Data page row directory exceeds page bounds");
    }

    DataPage result{page_number, {}};
    result.rows.reserve(row_count);
    std::uint16_t previous_start = static_cast<std::uint16_t>(bytes.size());
    for (std::uint16_t row_number = 0; row_number < row_count; ++row_number) {
        const std::uint16_t raw_location = read_u16(bytes, row_directory_offset + row_number * 2);
        const std::uint16_t start_offset = raw_location & row_offset_mask;
        const std::uint16_t end_offset = previous_start;
        if (start_offset < directory_end || start_offset > end_offset) {
            corrupted_page(file, page_number, "Data page row location is outside its row data region");
        }

        result.rows.push_back({
            row_number,
            start_offset,
            end_offset,
            (raw_location & deleted_row_mask) != 0,
            (raw_location & overflow_row_mask) != 0,
            std::vector<std::uint8_t>(bytes.begin() + start_offset, bytes.begin() + end_offset),
        });
        previous_start = start_offset;
    }

    return result;
}

DataPage read_data_page(const PageFile& file, std::uint64_t page_number) {
    const auto bytes = file.read_page(page_number);
    return parse_data_page(file, page_number, bytes);
}

}

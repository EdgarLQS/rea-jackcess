#include "jet4_global_usage_map.hpp"

#include "jet4_byte_order.hpp"

#include "../page/data_page.hpp"

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t global_usage_map_page_number = 1;
constexpr std::uint8_t global_usage_map_row_number = 0;
constexpr std::uint64_t inline_map_data_offset = 5;

[[noreturn]] void unsupported_operation(const PageFile& file, const char* message) {
    throw DatabaseError(DatabaseErrorCode::unsupported_operation, message, file.path());
}

[[noreturn]] void corrupted_file(const PageFile& file, const char* message) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                        message,
                        file.path(),
                        global_usage_map_page_number);
}

}

std::vector<std::uint8_t> mark_global_pages_allocated(
    const PageFile& file,
    const std::vector<std::uint64_t>& page_numbers) {
    auto page = file.read_page(global_usage_map_page_number);
    const DataPage data_page = parse_data_page(file, global_usage_map_page_number, page);
    if (global_usage_map_row_number >= data_page.rows.size()) {
        corrupted_file(file, "Global usage map declaration row is missing");
    }
    const auto& declaration = data_page.rows[global_usage_map_row_number];
    if (declaration.deleted || declaration.overflow || declaration.bytes.size() < inline_map_data_offset ||
        declaration.bytes[0] != 0) {
        unsupported_operation(file, "Global usage map promotion is not yet supported for page allocation");
    }

    const std::uint64_t first_page_number = read_u32(declaration.bytes, 1);
    const std::uint64_t bitmap_size = declaration.bytes.size() - inline_map_data_offset;
    for (const std::uint64_t page_number : page_numbers) {
        if (page_number < first_page_number ||
            page_number - first_page_number >= bitmap_size * 8) {
            unsupported_operation(file, "Global usage map expansion is not yet supported for page allocation");
        }

        const std::uint64_t bit_index = page_number - first_page_number;
        const std::uint64_t bitmap_offset = declaration.start_offset + inline_map_data_offset + bit_index / 8;
        const std::uint8_t bit_mask = static_cast<std::uint8_t>(1u << (bit_index % 8));
        if ((page[bitmap_offset] & bit_mask) == 0) {
            corrupted_file(file, "Appended page is not marked free in the global usage map");
        }
        page[bitmap_offset] = static_cast<std::uint8_t>(page[bitmap_offset] & ~bit_mask);
    }
    return page;
}

}

#include "usage_map.hpp"

#include "../page/data_page.hpp"
#include "../page/page_types.hpp"
#include "../format/jet_format.hpp"

#include <set>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t inline_usage_map_data_offset = 5;
constexpr std::uint64_t reference_usage_map_page_offset = 1;
constexpr std::uint64_t usage_map_page_data_offset = 4;

[[noreturn]] void corrupted_map(const PageFile& file,
                                std::uint64_t page_number,
                                const char* reason) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, reason, file.path(), page_number);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t read_u24(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16);
}

void add_bitmap_pages(const PageFile& file,
                      std::uint64_t context_page,
                      const std::vector<std::uint8_t>& bytes,
                      std::uint64_t offset,
                      std::uint64_t count,
                      std::uint64_t first_page,
                      std::set<std::uint64_t>& pages) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        corrupted_map(file, context_page, "Usage map bitmap extends beyond its data");
    }

    for (std::uint64_t byte_index = 0; byte_index < count; ++byte_index) {
        const std::uint8_t value = bytes[offset + byte_index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((value & (1u << bit)) == 0) {
                continue;
            }
            const std::uint64_t page_number = first_page + byte_index * 8 + bit;
            if (page_number >= file.page_count()) {
                corrupted_map(file, context_page, "Usage map references a page outside the database");
            }
            pages.insert(page_number);
        }
    }
}

}

std::vector<std::uint64_t> read_usage_map_pages(const PageFile& file, const UsageMapReference& reference) {
    if (!has_jet4_page_layout(file.format().version)) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "Jet4/ACE usage map layout requested for an unsupported engine version",
                            file.path(),
                            reference.page_number);
    }
    if (reference.page_number == 0 || reference.page_number >= file.page_count()) {
        corrupted_map(file, reference.page_number, "Usage map references an invalid declaration page");
    }

    const DataPage declaration_page = read_data_page(file, reference.page_number);
    if (reference.row_number >= declaration_page.rows.size()) {
        corrupted_map(file, reference.page_number, "Usage map declaration row is outside the data page");
    }
    const DataPageRow& declaration = declaration_page.rows[reference.row_number];
    if (declaration.deleted || declaration.overflow || declaration.bytes.empty()) {
        corrupted_map(file, reference.page_number, "Usage map declaration row is invalid");
    }

    const std::uint8_t map_type = declaration.bytes[0];
    std::set<std::uint64_t> pages;
    if (map_type == 0) {
        if (declaration.bytes.size() < inline_usage_map_data_offset) {
            corrupted_map(file, reference.page_number, "Inline usage map declaration is truncated");
        }
        const std::uint64_t first_page = read_u32(declaration.bytes, 1);
        add_bitmap_pages(file,
                         reference.page_number,
                         declaration.bytes,
                         inline_usage_map_data_offset,
                         declaration.bytes.size() - inline_usage_map_data_offset,
                         first_page,
                         pages);
    } else if (map_type == 1) {
        const std::uint64_t map_page_count = (declaration.bytes.size() - 1) / 4;
        const std::uint64_t pages_per_map_page =
            (file.format().page_size - usage_map_page_data_offset) * 8;
        for (std::uint64_t index = 0; index < map_page_count; ++index) {
            const std::uint32_t map_page_number =
                read_u32(declaration.bytes, reference_usage_map_page_offset + index * 4);
            if (map_page_number == 0) {
                continue;
            }
            if (map_page_number >= file.page_count()) {
                corrupted_map(file, reference.page_number, "Usage map references a page outside the database");
            }

            const auto map_page = file.read_page(map_page_number);
            if (map_page[0] != static_cast<std::uint8_t>(PageType::usage_map)) {
                corrupted_map(file, map_page_number, "Usage map page has an invalid page type");
            }
            add_bitmap_pages(file,
                             map_page_number,
                             map_page,
                             usage_map_page_data_offset,
                             map_page.size() - usage_map_page_data_offset,
                             index * pages_per_map_page,
                             pages);
        }
    } else {
        corrupted_map(file, reference.page_number, "Usage map has an unsupported map type");
    }

    return {pages.begin(), pages.end()};
}

}

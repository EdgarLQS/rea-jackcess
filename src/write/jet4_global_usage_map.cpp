#include "jet4_global_usage_map.hpp"

#include "../page/data_page.hpp"
#include "../page/page_types.hpp"
#include "../page/page_writer.hpp"

#include <algorithm>
#include "../util/little_endian.hpp"

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t declaration_page_number = 1;
constexpr std::uint64_t inline_bitmap_offset = 5;
constexpr std::uint64_t reference_bitmap_offset = 4;

[[noreturn]] void map_error(const PageFile& file, DatabaseErrorCode code, const char* message,
                           std::uint64_t page_number = declaration_page_number) {
    throw DatabaseError(code, message, file.path(), page_number);
}

std::vector<std::uint8_t>& stage_page(const PageFile& file, GlobalUsagePageUpdates& updates,
                                      std::uint64_t number) {
    if (const auto existing = updates.find(number); existing != updates.end()) {
        return existing->second;
    }
    return updates.emplace(number, file.read_page(number)).first->second;
}

void set_free_bit(const PageFile& file, std::vector<std::uint8_t>& bitmap,
                  std::uint64_t offset, std::uint64_t bit, bool free,
                  std::uint64_t bitmap_page_number) {
    auto& byte = bitmap[offset + bit / 8];
    const auto mask = static_cast<std::uint8_t>(1u << (bit % 8));
    if (((byte & mask) != 0) == free) {
        map_error(file, DatabaseErrorCode::corrupted_file,
                  free ? "Released page is already marked free in the global usage map"
                       : "Appended page is not marked free in the global usage map",
                  bitmap_page_number);
    }
    byte = static_cast<std::uint8_t>(free ? byte | mask : byte & ~mask);
}

void update_global_pages(const PageFile& file, GlobalUsagePageUpdates& updates,
                         const std::vector<std::uint64_t>& numbers, bool free) {
    if (numbers.empty()) {
        return;
    }
    // Stage the declaration only when it carries the actual bitmap.
    const auto existing = updates.find(declaration_page_number);
    const auto page = existing == updates.end() ? file.read_page(declaration_page_number)
                                                : existing->second;
    const auto data = parse_data_page(file, declaration_page_number, page);
    if (data.rows.empty() || data.rows.front().deleted || data.rows.front().overflow ||
        data.rows.front().bytes.size() < inline_bitmap_offset) {
        map_error(file, DatabaseErrorCode::corrupted_file, "Invalid global usage map declaration");
    }
    const auto& declaration = data.rows.front();
    const auto& bytes = declaration.bytes;
    switch (bytes[0]) {
        case 0: {
            auto& staged = updates.try_emplace(declaration_page_number, page).first->second;
            const auto start = read_u32(bytes, 1);
            const auto capacity = (bytes.size() - inline_bitmap_offset) * 8;
            for (const auto number : numbers) {
                if (number < start || number - start >= capacity) {
                    map_error(file, DatabaseErrorCode::unsupported_operation,
                              "Global inline usage map expansion is not yet supported");
                }
                set_free_bit(file, staged, declaration.start_offset + inline_bitmap_offset,
                             number - start, free, declaration_page_number);
            }
            return;
        }
        case 1: {
            const auto capacity = (file.format().page_size - reference_bitmap_offset) * 8;
            const auto slots = (bytes.size() - 1) / 4;
            for (const auto number : numbers) {
                const auto slot = number / capacity;
                const auto bitmap_number = slot < slots ? read_u32(bytes, 1 + slot * 4) : 0;
                if (bitmap_number == 0) {
                    map_error(file, DatabaseErrorCode::unsupported_operation,
                              "Global usage map needs an additional reference bitmap page");
                }
                // An earlier allocation in this batch may have promoted the map and staged
                // a bitmap page that is not appended to the file yet.
                if (bitmap_number >= file.page_count() && updates.find(bitmap_number) == updates.end()) {
                    map_error(file, DatabaseErrorCode::corrupted_file,
                              "Global usage map references a bitmap outside the database", bitmap_number);
                }
                auto& bitmap = stage_page(file, updates, bitmap_number);
                if (bitmap[0] != static_cast<std::uint8_t>(PageType::usage_map)) {
                    map_error(file, DatabaseErrorCode::corrupted_file,
                              "Global usage map bitmap has an invalid page type", bitmap_number);
                }
                set_free_bit(file, bitmap, reference_bitmap_offset, number % capacity, free, bitmap_number);
            }
            return;
        }
        default:
            map_error(file, DatabaseErrorCode::unsupported_operation, "Unsupported global usage map type");
    }
}

}

std::vector<std::uint64_t> mark_global_pages_allocated(
    const PageFile& file, GlobalUsagePageUpdates& updates,
    const std::vector<std::uint64_t>& page_numbers) {
    if (page_numbers.empty()) {
        return {};
    }
    const auto existing = updates.find(declaration_page_number);
    const auto page = existing == updates.end() ? file.read_page(declaration_page_number)
                                                : existing->second;
    const auto data = parse_data_page(file, declaration_page_number, page);
    if (data.rows.empty() || data.rows.front().deleted || data.rows.front().overflow ||
        data.rows.front().bytes.size() < inline_bitmap_offset) {
        map_error(file, DatabaseErrorCode::corrupted_file, "Invalid global usage map declaration");
    }
    const auto& declaration = data.rows.front();
    const auto& original = declaration.bytes;
    const auto start = read_u32(original, 1);
    const auto inline_capacity = (original.size() - inline_bitmap_offset) * 8;
    const auto covered_inline = [&](std::uint64_t number) {
        return number >= start && number - start < inline_capacity;
    };
    if (original[0] == 0 && std::all_of(page_numbers.begin(), page_numbers.end(), covered_inline)) {
        update_global_pages(file, updates, page_numbers, false);
        return {};
    }
    if (original[0] != 0 && original[0] != 1) {
        map_error(file, DatabaseErrorCode::unsupported_operation, "Unsupported global usage map type");
    }
    const auto capacity = (file.format().page_size - reference_bitmap_offset) * 8;
    auto bytes = original;
    const bool promote = original[0] == 0;
    if (promote) {
        std::fill(bytes.begin(), bytes.end(), 0);
        bytes[0] = 1;
    }

    const auto slots = (bytes.size() - 1) / 4;
    auto next_page = *std::max_element(page_numbers.begin(), page_numbers.end()) + 1;
    std::vector<std::uint64_t> appended;
    const auto change_bit = [&](std::uint64_t number) {
        const auto slot = number / capacity;
        if (slot >= slots) {
            map_error(file, DatabaseErrorCode::unsupported_operation,
                      "Global usage map has no reference slot for this page");
        }
        const auto pointer = 1 + slot * 4;
        auto bitmap_number = read_u32(bytes, pointer);
        if (bitmap_number == 0) {
            if (next_page >= 0x1000000) {
                map_error(file, DatabaseErrorCode::unsupported_operation,
                          "Global usage bitmap exceeds the three-byte page reference limit");
            }
            bitmap_number = static_cast<std::uint32_t>(next_page++);
            write_u32(bytes, pointer, bitmap_number);
            auto& bitmap = updates.emplace(bitmap_number,
                                            std::vector<std::uint8_t>(file.format().page_size, 0xff))
                               .first->second;
            bitmap[0] = static_cast<std::uint8_t>(PageType::usage_map);
            bitmap[1] = 1;
            bitmap[2] = bitmap[3] = 0;
            appended.push_back(bitmap_number);
        }
        if (bitmap_number >= file.page_count() &&
            std::find(appended.begin(), appended.end(), bitmap_number) == appended.end()) {
            map_error(file, DatabaseErrorCode::corrupted_file,
                      "Global usage map references a bitmap outside the database", bitmap_number);
        }
        auto& bitmap = stage_page(file, updates, bitmap_number);
        if (bitmap[0] != static_cast<std::uint8_t>(PageType::usage_map)) {
            map_error(file, DatabaseErrorCode::corrupted_file,
                      "Global usage map bitmap has an invalid page type", bitmap_number);
        }
        set_free_bit(file, bitmap, reference_bitmap_offset, number % capacity, false, bitmap_number);
    };

    if (promote) {
        // Preserve explicit free bits, including pages freed earlier in this operation.
        // Pages already in the file outside the old inline range are conservatively used.
        for (std::uint64_t number = 0; number < file.page_count(); ++number) {
            if (covered_inline(number)) {
                const auto bit = number - start;
                if ((original[inline_bitmap_offset + bit / 8] & (1u << (bit % 8))) != 0) {
                    continue;
                }
            }
            change_bit(number);
        }
    }
    for (const auto number : page_numbers) {
        if (promote && covered_inline(number)) {
            const auto bit = number - start;
            if ((original[inline_bitmap_offset + bit / 8] & (1u << (bit % 8))) == 0) {
                map_error(file, DatabaseErrorCode::corrupted_file,
                          "Appended page is not marked free in the global usage map");
            }
        }
        change_bit(number);
    }
    // A bitmap page can itself cross a reference-slot boundary. Process any
    // additional bitmap pages created while marking these pages as used.
    for (std::size_t index = 0; index < appended.size(); ++index) {
        change_bit(appended[index]);
    }
    if (bytes != original) {
        auto& staged = updates.try_emplace(declaration_page_number, page).first->second;
        std::copy(bytes.begin(), bytes.end(), staged.begin() + declaration.start_offset);
    }
    return appended;
}

void append_usage_bitmap_pages(PageFileWriter& writer, const GlobalUsagePageUpdates& updates,
                               const std::vector<std::uint64_t>& appended) {
    for (const auto number : appended) {
        if (writer.append_page(updates.at(number)) != number) {
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Usage map page was appended at an unexpected page number",
                                writer.path(), number);
        }
    }
}

void mark_global_pages_free(const PageFile& file, GlobalUsagePageUpdates& updates,
                            const std::vector<std::uint64_t>& page_numbers) {
    update_global_pages(file, updates, page_numbers, true);
}

}

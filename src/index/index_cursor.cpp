#include "index_cursor.hpp"

#include "../page/page_types.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t table_definition_page_offset = 4;
constexpr std::uint64_t previous_page_offset = 12;
constexpr std::uint64_t next_page_offset = 16;
constexpr std::uint64_t child_tail_page_offset = 20;
constexpr std::uint64_t compressed_prefix_length_offset = 24;
constexpr std::uint64_t index_level_offset = 26;
constexpr std::uint64_t entry_mask_offset = 27;
constexpr std::uint64_t entry_mask_size = 453;
constexpr std::uint64_t entry_data_offset = entry_mask_offset + entry_mask_size;
constexpr std::uint64_t leaf_entry_trailer_size = 4;
constexpr std::uint64_t node_entry_trailer_size = 8;

struct IndexPageEntry {
    std::vector<std::uint8_t> key_bytes;
    IndexRowLocation row;
    std::optional<std::uint32_t> child_page_number;
};

struct IndexPage {
    std::uint32_t page_number;
    std::uint32_t previous_page_number;
    std::uint32_t next_page_number;
    std::uint32_t child_tail_page_number;
    std::uint8_t level;
    bool leaf;
    std::vector<IndexPageEntry> entries;
};

[[noreturn]] void invalid_index_page(const PageFile& file,
                                     const std::string& table_name,
                                     std::uint64_t page_number,
                                     const char* reason) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                        reason,
                        file.path(),
                        page_number,
                        std::move(context));
}

std::uint16_t read_u16_le(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t read_u24_be(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 2]);
}

std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

int compare_entries(const IndexPageEntry& left, const IndexPageEntry& right) {
    if (std::lexicographical_compare(left.key_bytes.begin(),
                                     left.key_bytes.end(),
                                     right.key_bytes.begin(),
                                     right.key_bytes.end())) {
        return -1;
    }
    if (std::lexicographical_compare(right.key_bytes.begin(),
                                     right.key_bytes.end(),
                                     left.key_bytes.begin(),
                                     left.key_bytes.end())) {
        return 1;
    }
    if (left.row.page_number != right.row.page_number) {
        return left.row.page_number < right.row.page_number ? -1 : 1;
    }
    if (left.row.row_number != right.row.row_number) {
        return left.row.row_number < right.row.row_number ? -1 : 1;
    }
    return 0;
}

void validate_page_reference(const PageFile& file,
                             const std::string& table_name,
                             std::uint64_t source_page,
                             std::uint32_t referenced_page,
                             const char* reason) {
    if (referenced_page != 0 && referenced_page >= file.page_count()) {
        invalid_index_page(file, table_name, source_page, reason);
    }
}

IndexPage read_index_page(const PageFile& file,
                          const std::string& table_name,
                          std::uint64_t table_definition_page_number,
                          std::uint32_t page_number) {
    if (page_number == 0 || page_number >= file.page_count()) {
        invalid_index_page(file, table_name, page_number, "Index page reference is outside the database");
    }

    const std::vector<std::uint8_t> bytes = file.read_page(page_number);
    if (bytes.size() != file.format().page_size || bytes.size() < entry_data_offset) {
        invalid_index_page(file, table_name, page_number, "Index page has an invalid byte length");
    }
    if (bytes[0] != static_cast<std::uint8_t>(PageType::index_leaf) &&
        bytes[0] != static_cast<std::uint8_t>(PageType::index_node)) {
        invalid_index_page(file, table_name, page_number, "Index reference points to a non-index page");
    }
    if (read_u32_le(bytes, table_definition_page_offset) != table_definition_page_number) {
        invalid_index_page(file, table_name, page_number,
                           "Index page belongs to a different table definition");
    }

    const bool leaf = bytes[0] == static_cast<std::uint8_t>(PageType::index_leaf);
    const std::uint8_t level = bytes[index_level_offset];
    if ((leaf && level != 0) || (!leaf && level == 0)) {
        invalid_index_page(file, table_name, page_number, "Index page type and level disagree");
    }

    const std::uint32_t previous_page_number = read_u32_le(bytes, previous_page_offset);
    const std::uint32_t next_page_number = read_u32_le(bytes, next_page_offset);
    const std::uint32_t child_tail_page_number = read_u32_le(bytes, child_tail_page_offset);
    validate_page_reference(file, table_name, page_number, previous_page_number,
                            "Index page has an invalid previous-page reference");
    validate_page_reference(file, table_name, page_number, next_page_number,
                            "Index page has an invalid next-page reference");
    validate_page_reference(file, table_name, page_number, child_tail_page_number,
                            "Index page has an invalid child-tail reference");

    const std::uint16_t prefix_length = read_u16_le(bytes, compressed_prefix_length_offset);
    const std::uint16_t free_space = read_u16_le(bytes, 2);
    const std::uint64_t data_capacity = bytes.size() - entry_data_offset;
    if (free_space > data_capacity) {
        invalid_index_page(file, table_name, page_number, "Index page free-space count is out of range");
    }

    std::vector<IndexPageEntry> entries;
    std::vector<std::uint8_t> common_prefix;
    std::uint64_t last_end = 0;
    const std::uint64_t trailer_size = leaf ? leaf_entry_trailer_size : node_entry_trailer_size;
    for (std::uint64_t mask_byte = 0; mask_byte < entry_mask_size; ++mask_byte) {
        const std::uint8_t mask = bytes[entry_mask_offset + mask_byte];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((mask & static_cast<std::uint8_t>(1u << bit)) == 0) {
                continue;
            }

            const std::uint64_t end = mask_byte * 8 + bit;
            if (end <= last_end || end > data_capacity) {
                invalid_index_page(file, table_name, page_number, "Index entry mask has an invalid boundary");
            }
            const std::uint64_t compressed_length = end - last_end;
            if (compressed_length < trailer_size) {
                invalid_index_page(file, table_name, page_number, "Index entry is shorter than its row reference");
            }

            const std::uint64_t start = entry_data_offset + last_end;
            const std::uint64_t key_suffix_length = compressed_length - trailer_size;
            IndexPageEntry entry;
            if (entries.empty()) {
                if (prefix_length > key_suffix_length) {
                    invalid_index_page(file, table_name, page_number,
                                       "Index entry prefix is longer than its first key");
                }
                entry.key_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                       bytes.begin() + static_cast<std::ptrdiff_t>(start + key_suffix_length));
                common_prefix.assign(entry.key_bytes.begin(),
                                     entry.key_bytes.begin() + static_cast<std::ptrdiff_t>(prefix_length));
            } else {
                entry.key_bytes = common_prefix;
                entry.key_bytes.insert(entry.key_bytes.end(),
                                       bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                       bytes.begin() + static_cast<std::ptrdiff_t>(start + key_suffix_length));
            }

            const std::uint64_t row_reference_offset = start + key_suffix_length;
            entry.row.page_number = read_u24_be(bytes, row_reference_offset);
            entry.row.row_number = bytes[row_reference_offset + 3];
            if (entry.row.page_number == 0 || entry.row.page_number >= file.page_count()) {
                invalid_index_page(file, table_name, page_number,
                                   "Index entry points outside the database row pages");
            }
            if (!leaf) {
                const std::uint32_t child_page_number = read_u32_be(bytes, row_reference_offset + 4);
                if (child_page_number == 0 || child_page_number >= file.page_count()) {
                    invalid_index_page(file, table_name, page_number,
                                       "Index node entry has an invalid child-page reference");
                }
                entry.child_page_number = child_page_number;
            }
            if (!entries.empty() && compare_entries(entries.back(), entry) >= 0) {
                invalid_index_page(file, table_name, page_number, "Index entries are not in ascending order");
            }

            entries.push_back(std::move(entry));
            last_end = end;
        }
    }

    if (entries.empty() && prefix_length != 0) {
        invalid_index_page(file, table_name, page_number, "Empty index page has a non-empty key prefix");
    }
    if (last_end + free_space != data_capacity) {
        invalid_index_page(file, table_name, page_number,
                           "Index page free-space count does not match its entry boundaries");
    }
    if (!leaf && entries.empty() && child_tail_page_number == 0) {
        invalid_index_page(file, table_name, page_number, "Index node has no child pages");
    }

    return {page_number,
            previous_page_number,
            next_page_number,
            child_tail_page_number,
            level,
            leaf,
            std::move(entries)};
}

}

struct IndexEntryCursor::Impl {
    Impl(const PageFile& source_file,
         std::string source_table_name,
         std::uint64_t source_table_definition_page_number,
         std::uint32_t root_page_number)
        : file(source_file),
          table_name(std::move(source_table_name)),
          table_definition_page_number(source_table_definition_page_number) {
        if (file.format().version != EngineVersion::jet4) {
            DatabaseErrorContext context;
            context.table_name = table_name;
            throw DatabaseError(DatabaseErrorCode::unsupported_version,
                                "Jet4 index traversal requested for a different engine version",
                                file.path(),
                                std::nullopt,
                                std::move(context));
        }
        if (root_page_number == 0) {
            exhausted = true;
            return;
        }

        std::uint32_t page_number = root_page_number;
        std::optional<std::uint8_t> parent_level;
        while (true) {
            if (!visited_pages.insert(page_number).second) {
                invalid_index_page(file, table_name, page_number, "Index tree contains a child-page cycle");
            }
            IndexPage page = read_index_page(file,
                                             table_name,
                                             table_definition_page_number,
                                             page_number);
            if (parent_level && page.level + 1 != *parent_level) {
                invalid_index_page(file, table_name, page_number,
                                   "Index child page has an unexpected tree level");
            }
            if (page.leaf) {
                if (page.previous_page_number != 0) {
                    invalid_index_page(file, table_name, page_number,
                                       "Leftmost index leaf has a previous-page reference");
                }
                current_leaf = std::move(page);
                return;
            }

            const std::uint32_t first_child = page.entries.empty()
                                                  ? page.child_tail_page_number
                                                  : *page.entries.front().child_page_number;
            if (first_child == 0) {
                invalid_index_page(file, table_name, page_number,
                                   "Index node has no leftmost child page");
            }
            parent_level = page.level;
            page_number = first_child;
        }
    }

    std::optional<IndexRowLocation> next() {
        if (exhausted || !current_leaf) {
            return std::nullopt;
        }

        while (next_entry_index >= current_leaf->entries.size()) {
            const std::uint32_t next_page_number = current_leaf->next_page_number;
            if (next_page_number == 0) {
                exhausted = true;
                return std::nullopt;
            }
            if (!visited_pages.insert(next_page_number).second) {
                invalid_index_page(file, table_name, next_page_number,
                                   "Index leaf chain contains a page cycle");
            }

            const std::uint32_t previous_page_number = current_leaf->page_number;
            const std::optional<IndexPageEntry> previous_entry = last_entry;
            IndexPage next_page = read_index_page(file,
                                                  table_name,
                                                  table_definition_page_number,
                                                  next_page_number);
            if (!next_page.leaf || next_page.level != current_leaf->level) {
                invalid_index_page(file, table_name, next_page_number,
                                   "Index leaf chain references a page at a different level");
            }
            if (next_page.previous_page_number != previous_page_number) {
                invalid_index_page(file, table_name, next_page_number,
                                   "Index leaf sibling references are inconsistent");
            }
            if (previous_entry && !next_page.entries.empty() &&
                compare_entries(*previous_entry, next_page.entries.front()) >= 0) {
                invalid_index_page(file, table_name, next_page_number,
                                   "Index entries are out of order across leaf pages");
            }
            current_leaf = std::move(next_page);
            next_entry_index = 0;
        }

        const IndexPageEntry& entry = current_leaf->entries[next_entry_index++];
        last_entry = entry;
        return entry.row;
    }

    const PageFile& file;
    std::string table_name;
    std::uint64_t table_definition_page_number;
    std::set<std::uint32_t> visited_pages;
    std::optional<IndexPage> current_leaf;
    std::optional<IndexPageEntry> last_entry;
    std::size_t next_entry_index = 0;
    bool exhausted = false;
};

IndexEntryCursor::IndexEntryCursor(const PageFile& file,
                                   std::string table_name,
                                   std::uint64_t table_definition_page_number,
                                   std::uint32_t root_page_number)
    : impl_(std::make_unique<Impl>(file,
                                   std::move(table_name),
                                   table_definition_page_number,
                                   root_page_number)) {}

IndexEntryCursor::~IndexEntryCursor() = default;

IndexEntryCursor::IndexEntryCursor(IndexEntryCursor&& other) noexcept = default;

IndexEntryCursor& IndexEntryCursor::operator=(IndexEntryCursor&& other) noexcept = default;

std::optional<IndexRowLocation> IndexEntryCursor::next() {
    if (!impl_) {
        return std::nullopt;
    }
    return impl_->next();
}

}

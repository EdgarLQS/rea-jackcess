#include "jet4_index_entry.hpp"

#include "jet4_fixed_type.hpp"
#include "jet4_variable_type.hpp"

#include "../index/jet4_text_key.hpp"
#include "../page/page_types.hpp"
#include "../value/text_codec.hpp"
#include "../value/jet4_value.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

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

constexpr std::uint8_t asc_start_flag = 0x7f;
constexpr std::uint8_t asc_null_flag = 0x00;

constexpr std::uint8_t boolean_type = 0x01;
constexpr std::uint8_t byte_type = 0x02;
constexpr std::uint8_t short_type = 0x03;
constexpr std::uint8_t long_type = 0x04;
constexpr std::uint8_t money_type = 0x05;
constexpr std::uint8_t float_type = 0x06;
constexpr std::uint8_t double_type = 0x07;
constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t memo_type = 0x0c;
constexpr std::uint8_t guid_type = 0x0f;
constexpr std::size_t kept_index_key_length = jet4_max_index_key_length - 2;

const std::array<std::uint16_t, 256>& jet4_key_digest_table() {
    static const auto table = [] {
        std::array<std::uint16_t, 256> result{};
        for (std::uint32_t index = 0; index < result.size(); ++index) {
            std::uint32_t reg = index << 8;
            for (int bit = 0; bit < 8; ++bit) {
                reg = ((reg << 1) & 0xffffu) ^ ((reg & 0x8000u) != 0 ? 0x8005u : 0u);
            }
            result[index] = static_cast<std::uint16_t>(((reg & 0xffu) << 8) | (reg >> 8));
        }
        return result;
    }();
    return table;
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t read_u24_be(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return (static_cast<std::uint64_t>(bytes[offset]) << 16) |
           (static_cast<std::uint64_t>(bytes[offset + 1]) << 8) |
           bytes[offset + 2];
}

std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           bytes[offset + 3];
}

void write_u16(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void write_u24_be(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>(value & 0xff);
}

void write_u32_be(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xff);
}

[[noreturn]] void unsupported(const PageFile& file,
                              const char* message,
                              std::uint64_t page_number) {
    throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                        message,
                        file.path(),
                        page_number);
}

[[noreturn]] void invalid(const PageFile& file, const char* message, std::uint64_t page_number) {
    throw DatabaseError(DatabaseErrorCode::corrupted_file, message, file.path(), page_number);
}

void flip_first_bit(std::vector<std::uint8_t>& bytes, std::size_t offset) {
    bytes[offset] = static_cast<std::uint8_t>(bytes[offset] ^ 0x80);
}

void flip_all(std::vector<std::uint8_t>& bytes, std::size_t offset) {
    for (std::size_t index = offset; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(bytes[index] ^ 0xff);
    }
}

std::vector<std::uint8_t> encode_integer_key(std::vector<std::uint8_t> payload) {
    flip_first_bit(payload, 0);
    std::vector<std::uint8_t> key{asc_start_flag};
    key.insert(key.end(), payload.begin(), payload.end());
    return key;
}

// Ascending BINARY/GUID payloads are stored as 8-byte segments, each followed by a length byte
// that is 9 while more data follows and the remaining byte count for the final segment.
std::vector<std::uint8_t> encode_general_binary_key(const std::uint8_t* data,
                                                   std::size_t size) {
    std::vector<std::uint8_t> key{asc_start_flag};
    if (size == 0) {
        return key;
    }
    std::size_t position = 0;
    std::size_t remaining = size;
    while (remaining > 8) {
        key.insert(key.end(), data + position, data + position + 8);
        key.push_back(9);
        position += 8;
        remaining -= 8;
    }
    key.insert(key.end(), data + position, data + position + remaining);
    key.insert(key.end(), 8 - remaining, 0);
    key.push_back(static_cast<std::uint8_t>(remaining));
    return key;
}

std::vector<std::uint8_t> encode_floating_key(std::vector<std::uint8_t> payload) {
    const bool negative = (payload[0] & 0x80) != 0;
    if (!negative) {
        flip_first_bit(payload, 0);
    } else {
        flip_all(payload, 0);
    }
    std::vector<std::uint8_t> key{asc_start_flag};
    key.insert(key.end(), payload.begin(), payload.end());
    return key;
}

bool key_less(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right) {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

bool entry_less(const Jet4IndexEntry& left, const Jet4IndexEntry& right) {
    if (left.key != right.key) {
        return key_less(left.key, right.key);
    }
    if (left.row_page_number != right.row_page_number) {
        return left.row_page_number < right.row_page_number;
    }
    return left.row_number < right.row_number;
}

bool entry_equal(const Jet4IndexEntry& left, const Jet4IndexEntry& right) {
    return left.key == right.key && left.row_page_number == right.row_page_number &&
           left.row_number == right.row_number;
}

struct ParsedPage {
    std::uint64_t page_number = 0;
    bool leaf = true;
    std::uint8_t level = 0;
    std::uint32_t previous_page_number = 0;
    std::uint32_t next_page_number = 0;
    std::uint32_t child_tail_page_number = 0;
    std::vector<Jet4IndexEntry> entries;
};

std::uint64_t trailer_size(bool leaf) {
    return leaf ? leaf_entry_trailer_size : node_entry_trailer_size;
}

std::size_t common_key_prefix(const Jet4IndexEntry& first, const Jet4IndexEntry& last) {
    return static_cast<std::size_t>(
        std::mismatch(first.key.begin(), first.key.end(), last.key.begin(), last.key.end()).first -
        first.key.begin());
}

std::uint64_t entry_capacity(std::uint32_t page_size) {
    return std::min(page_size - entry_data_offset, entry_mask_size * 8 - 1);
}

std::uint64_t entries_size(const std::vector<Jet4IndexEntry>& entries, bool leaf) {
    std::uint64_t size = 0;
    for (const auto& entry : entries) {
        size += entry.key.size() + trailer_size(leaf);
    }
    return size;
}

std::uint64_t compressed_size(std::uint64_t bytes, std::size_t count, std::size_t prefix) {
    return bytes - prefix * (count - 1);
}

bool stores_child_tail(const ParsedPage& page, std::size_t child_count) {
    // Jackcess uses a tail only for leaf children and demotes it when one child remains.
    return !page.leaf && page.level == 1 && child_count > 1;
}

std::uint64_t stored_entries_size(const ParsedPage& page,
                                  std::size_t begin,
                                  std::size_t end,
                                  std::uint64_t full_size) {
    if (begin == end) {
        return 0;
    }
    if (stores_child_tail(page, end - begin)) {
        --end;
        full_size -= page.entries[end].key.size() + node_entry_trailer_size;
    }
    return compressed_size(full_size, end - begin,
                           common_key_prefix(page.entries[begin], page.entries[end - 1]));
}

bool entries_fit(const ParsedPage& page, std::uint32_t page_size) {
    return stored_entries_size(page, 0, page.entries.size(), entries_size(page.entries, page.leaf))
           <= entry_capacity(page_size);
}

// Try the existing count-based split first. A shared prefix is recomputed independently
// for each half because one changed boundary key can shrink the prefix substantially.
std::size_t split_position(const ParsedPage& page,
                           std::uint32_t page_size,
                           std::size_t preferred) {
    const auto capacity = entry_capacity(page_size);
    const auto total = entries_size(page.entries, page.leaf);
    const auto count = page.entries.size();
    std::uint64_t head = 0;
    std::size_t position = 0;
    for (std::size_t left = 1; left < count; ++left) {
        head += page.entries[left - 1].key.size() + trailer_size(page.leaf);
        if (stored_entries_size(page, 0, left, head) > capacity) {
            break;
        }
        if (stored_entries_size(page, left, count, total - head) <= capacity) {
            position = left;
            if (left >= preferred) {
                break;
            }
        }
    }
    return position;
}

ParsedPage parse_page(const PageFile& file,
                      std::uint64_t definition_page_number,
                      std::uint64_t page_number) {
    if (page_number == 0 || page_number >= file.page_count()) {
        invalid(file, "Index page reference is outside the database", page_number);
    }
    const std::vector<std::uint8_t> bytes = file.read_page(page_number);
    if (bytes.size() != file.format().page_size || bytes.size() < entry_data_offset) {
        invalid(file, "Index page has an invalid byte length", page_number);
    }
    if (bytes[0] != static_cast<std::uint8_t>(PageType::index_leaf) &&
        bytes[0] != static_cast<std::uint8_t>(PageType::index_node)) {
        invalid(file, "Index reference points to a non-index page", page_number);
    }
    if (read_u32(bytes, table_definition_page_offset) != definition_page_number) {
        unsupported(file, "Index page belongs to a different table definition", page_number);
    }
    const auto prefix_length = read_u16(bytes, compressed_prefix_length_offset);

    ParsedPage page;
    page.page_number = page_number;
    page.leaf = bytes[0] == static_cast<std::uint8_t>(PageType::index_leaf);
    page.level = bytes[index_level_offset];
    if ((page.leaf && page.level != 0) || (!page.leaf && page.level == 0)) {
        invalid(file, "Index page type and level disagree", page_number);
    }
    page.previous_page_number = read_u32(bytes, previous_page_offset);
    page.next_page_number = read_u32(bytes, next_page_offset);
    page.child_tail_page_number = read_u32(bytes, child_tail_page_offset);

    const std::uint64_t data_capacity = bytes.size() - entry_data_offset;
    const std::uint64_t trailer = trailer_size(page.leaf);
    std::uint64_t previous_end = 0;
    for (std::uint64_t mask_byte = 0; mask_byte < entry_mask_size; ++mask_byte) {
        const std::uint8_t mask = bytes[entry_mask_offset + mask_byte];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((mask & static_cast<std::uint8_t>(1u << bit)) == 0) {
                continue;
            }
            const std::uint64_t end = mask_byte * 8 + bit;
            if (end <= previous_end || end > data_capacity || end - previous_end < trailer) {
                invalid(file, "Index entry mask is outside the page", page_number);
            }
            const std::uint64_t start = entry_data_offset + previous_end;
            const std::uint64_t key_size = end - previous_end - trailer;
            const std::uint64_t trailer_offset = start + key_size;
            Jet4IndexEntry entry;
            if (page.entries.empty()) {
                if (prefix_length > key_size) {
                    invalid(file, "Index prefix is longer than the first key", page_number);
                }
            } else {
                const auto& first_key = page.entries.front().key;
                entry.key.assign(first_key.begin(), first_key.begin() + prefix_length);
            }
            entry.key.insert(entry.key.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start),
                             bytes.begin() + static_cast<std::ptrdiff_t>(trailer_offset));
            entry.row_page_number = static_cast<std::uint32_t>(read_u24_be(bytes, trailer_offset));
            entry.row_number = bytes[trailer_offset + 3];
            if (!page.leaf) {
                entry.child_page_number = read_u32_be(bytes, trailer_offset + 4);
                if (entry.child_page_number == 0 || entry.child_page_number >= file.page_count()) {
                    invalid(file, "Index node entry has an invalid child page", page_number);
                }
            }
            if (entry.row_page_number == 0 || entry.row_page_number >= file.page_count()) {
                invalid(file, "Index entry has an invalid row page", page_number);
            }
            if (!page.entries.empty()) {
                const auto& previous = page.entries.back();
                if (entry_less(entry, previous) || (page.leaf && entry_equal(entry, previous))) {
                    invalid(file, "Index entries are not in ascending order", page_number);
                }
            }
            page.entries.push_back(std::move(entry));
            previous_end = end;
        }
    }
    if (page.entries.empty() && prefix_length != 0) {
        invalid(file, "Empty index page has a non-empty key prefix", page_number);
    }
    if (previous_end + read_u16(bytes, 2) != data_capacity) {
        invalid(file, "Index free-space count is inconsistent", page_number);
    }
    if (!page.leaf && page.entries.empty() && page.child_tail_page_number == 0) {
        invalid(file, "Index node has no child pages", page_number);
    }
    return page;
}

struct TreePath {
    std::vector<std::uint64_t> pages;
    std::vector<std::size_t> indices;
};

struct TreeWriter {
    const PageFile& file;
    std::uint64_t definition_page_number;
    std::uint32_t root_page_number;
    std::uint64_t next_new_page_number;
    bool allow_split;
    bool unique;

    std::map<std::uint64_t, ParsedPage> pages;
    std::set<std::uint64_t> modified_pages;
    std::vector<std::uint64_t> appended_pages;
    std::vector<std::uint64_t> freed_pages;
    std::int64_t distinct_key_delta = 0;

    TreeWriter(const PageFile& source_file,
               std::uint64_t source_definition_page_number,
               std::uint32_t source_root_page_number,
               std::uint64_t first_new_page_number,
               bool source_allow_split,
               bool source_unique)
        : file(source_file),
          definition_page_number(source_definition_page_number),
          root_page_number(source_root_page_number),
          next_new_page_number(first_new_page_number),
          allow_split(source_allow_split),
          unique(source_unique) {}

    ParsedPage& page(std::uint64_t page_number) {
        const auto existing = pages.find(page_number);
        if (existing != pages.end()) {
            return existing->second;
        }
        auto parsed = parse_page(file, definition_page_number, page_number);
        if (parsed.child_tail_page_number != 0) {
            const auto tail_number = parsed.child_tail_page_number;
            if (parsed.leaf || parsed.level != 1 || tail_number == page_number) {
                invalid(file, "Index child tail must point from a level-one node to a leaf",
                        page_number);
            }
            auto tail = pages.find(tail_number);
            if (tail == pages.end()) {
                auto leaf = parse_page(file, definition_page_number, tail_number);
                if (!leaf.leaf || leaf.child_tail_page_number != 0) {
                    invalid(file, "Index child tail points to a non-leaf page", page_number);
                }
                tail = pages.emplace(tail_number, std::move(leaf)).first;
            }
            if (!tail->second.leaf || tail->second.entries.empty()) {
                invalid(file, "Index child tail points to an invalid leaf", page_number);
            }

            // Keep an explicit maximum in memory so descent and maintenance have one layout.
            auto tail_entry = node_entry(tail->second.entries.back(), tail_number);
            if (!parsed.entries.empty() && !entry_less(parsed.entries.back(), tail_entry)) {
                invalid(file, "Index child tail does not follow its node separators", page_number);
            }
            parsed.entries.push_back(std::move(tail_entry));
            parsed.child_tail_page_number = 0;
        }
        return pages.emplace(page_number, std::move(parsed)).first->second;
    }

    ParsedPage& touch(std::uint64_t page_number) {
        modified_pages.insert(page_number);
        return page(page_number);
    }

    ParsedPage& allocate(bool leaf, std::uint8_t level) {
        if (next_new_page_number >= 0x1000000) {
            unsupported(file, "Jet4 index page exceeds the three-byte page reference limit",
                        next_new_page_number);
        }
        ParsedPage page;
        page.page_number = next_new_page_number++;
        page.leaf = leaf;
        page.level = level;
        appended_pages.push_back(page.page_number);
        modified_pages.insert(page.page_number);
        return pages.emplace(page.page_number, std::move(page)).first->second;
    }

    // Records every page from the root down to a leaf together with the child entry used at
    // each level, so a split or a removal can walk back up and refresh ancestor separators.
    TreePath descend_path(const Jet4IndexEntry& entry) {
        TreePath path;
        std::uint64_t current = root_page_number;
        int expected_level = -1;
        while (true) {
            path.pages.push_back(current);
            ParsedPage& node = page(current);
            if (expected_level >= 0 && node.level != expected_level) {
                invalid(file, "Index child level is inconsistent with its parent", current);
            }
            if (node.leaf) {
                return path;
            }
            if (node.entries.empty()) {
                invalid(file, "Index node has no entries", current);
            }
            // Separators carry RowId as a tie-breaker when equal keys span multiple leaves.
            const auto last = node.entries.end() - 1;
            const auto child = std::find_if(node.entries.begin(), last, [&](const auto& separator) {
                return !entry_less(separator, entry);
            });
            const auto index = static_cast<std::size_t>(child - node.entries.begin());
            path.indices.push_back(index);
            expected_level = node.level - 1;
            current = node.entries[index].child_page_number;
        }
    }

    static TreePath parent_path(const TreePath& path) {
        TreePath parent;
        parent.pages.assign(path.pages.begin(), path.pages.end() - 1);
        parent.indices.assign(path.indices.begin(), path.indices.end() - 1);
        return parent;
    }

    // Node entries carry the maximum key of their child subtree, so whenever a subtree maximum
    // changes every separator above it must be rewritten as well.
    void refresh_path_max(const TreePath& path) {
        for (std::size_t level = path.pages.size(); level-- > 1;) {
            ParsedPage& child = page(path.pages[level]);
            if (child.entries.empty()) {
                return;
            }
            const std::size_t parent_index = path.indices[level - 1];
            ParsedPage& parent = page(path.pages[level - 1]);
            if (parent_index >= parent.entries.size() ||
                parent.entries[parent_index].child_page_number != child.page_number) {
                invalid(file, "Index parent entry does not reference its child page",
                        parent.page_number);
            }
            Jet4IndexEntry refreshed =
                node_entry(child.entries.back(), static_cast<std::uint32_t>(child.page_number));
            const Jet4IndexEntry& existing = parent.entries[parent_index];
            if (existing.key == refreshed.key &&
                existing.row_page_number == refreshed.row_page_number &&
                existing.row_number == refreshed.row_number) {
                continue;
            }
            touch(parent.page_number).entries[parent_index] = std::move(refreshed);
            if (!entries_fit(parent, file.format().page_size)) {
                if (!allow_split) {
                    unsupported(file, "Index node is full; this operation does not split index pages",
                                parent.page_number);
                }
                TreePath parent_tree = path;
                parent_tree.pages.resize(level);
                parent_tree.indices.resize(level - 1);
                split_node(parent_tree);
                return;
            }
        }
    }

    static Jet4IndexEntry node_entry(const Jet4IndexEntry& leaf_entry, std::uint32_t child_page) {
        Jet4IndexEntry entry;
        entry.key = leaf_entry.key;
        entry.row_page_number = leaf_entry.row_page_number;
        entry.row_number = leaf_entry.row_number;
        entry.child_page_number = child_page;
        return entry;
    }

    bool duplicate_key(const ParsedPage& leaf, std::size_t position,
                       const std::vector<std::uint8_t>& key) {
        if (position > 0 && leaf.entries[position - 1].key == key) {
            return true;
        }
        if (position < leaf.entries.size() && leaf.entries[position].key == key) {
            return true;
        }
        if (position == 0 && leaf.previous_page_number != 0) {
            const ParsedPage& previous = page(leaf.previous_page_number);
            if (previous.leaf && !previous.entries.empty() &&
                previous.entries.back().key == key) {
                return true;
            }
        }
        return false;
    }

    void insert(Jet4IndexEntry entry) {
        TreePath path = descend_path(entry);
        const std::uint64_t leaf_page_number = path.pages.back();
        ParsedPage& leaf = page(leaf_page_number);
        if (!leaf.leaf) {
            invalid(file, "Index descent did not reach a leaf page", leaf_page_number);
        }

        const auto position = static_cast<std::size_t>(std::distance(
            leaf.entries.begin(),
            std::lower_bound(leaf.entries.begin(), leaf.entries.end(), entry, entry_less)));
        const bool duplicate = duplicate_key(leaf, position, entry.key);
        if (duplicate && unique && !entry.all_columns_null) {
            throw DatabaseError(DatabaseErrorCode::constraint_violation,
                                "Duplicate key violates a unique index",
                                file.path(),
                                leaf_page_number);
        }
        leaf.entries.insert(leaf.entries.begin() + static_cast<std::ptrdiff_t>(position),
                            std::move(entry));
        modified_pages.insert(leaf_page_number);
        // The Jet4 count tracks distinct keys, so duplicates leave it unchanged.
        distinct_key_delta += duplicate ? 0 : 1;

        if (entries_fit(leaf, file.format().page_size)) {
            refresh_path_max(path);
            return;
        }
        if (!allow_split) {
            unsupported(file, "Index leaf is full; this operation does not split index pages",
                        leaf_page_number);
        }
        split_leaf(path);
    }

    void split_leaf(const TreePath& path) {
        const std::uint64_t leaf_page_number = path.pages.back();
        ParsedPage& leaf = page(leaf_page_number);
        const std::size_t total = leaf.entries.size();
        if (total < 2) {
            unsupported(file, "Index leaf with fewer than two entries cannot be split",
                        leaf_page_number);
        }
        // Jackcess keeps the larger half (including the new entry) on the head page.
        const auto head_count = split_position(leaf, file.format().page_size, (total + 1) / 2);
        if (head_count == 0) {
            unsupported(file, "Index leaf entries do not fit in two pages", leaf_page_number);
        }
        std::vector<Jet4IndexEntry> head(leaf.entries.begin(),
                                         leaf.entries.begin() +
                                             static_cast<std::ptrdiff_t>(head_count));
        std::vector<Jet4IndexEntry> tail(leaf.entries.begin() +
                                             static_cast<std::ptrdiff_t>(head_count),
                                         leaf.entries.end());
        const Jet4IndexEntry head_max = head.back();
        const Jet4IndexEntry tail_max = tail.back();
        const std::uint32_t old_previous = leaf.previous_page_number;
        const std::uint32_t old_next = leaf.next_page_number;

        if (path.pages.size() == 1) {
            // The root leaf cannot be split in place: move its contents under two new leaf
            // pages and turn the root page into a node with one entry per leaf.
            ParsedPage& head_page = allocate(true, 0);
            const std::uint64_t head_page_number = head_page.page_number;
            head_page.entries = std::move(head);
            head_page.previous_page_number = old_previous;

            ParsedPage& tail_page = allocate(true, 0);
            const std::uint64_t tail_page_number = tail_page.page_number;
            tail_page.entries = std::move(tail);
            tail_page.previous_page_number = static_cast<std::uint32_t>(head_page_number);
            tail_page.next_page_number = old_next;
            // The old root page becomes a node page, so it must not appear in the leaf chain.
            head_page.next_page_number = static_cast<std::uint32_t>(tail_page_number);

            if (old_previous != 0) {
                touch(old_previous).next_page_number = static_cast<std::uint32_t>(head_page_number);
            }
            if (old_next != 0) {
                touch(old_next).previous_page_number = static_cast<std::uint32_t>(tail_page_number);
            }

            ParsedPage& root = touch(root_page_number);
            root.leaf = false;
            root.level = 1;
            root.child_tail_page_number = 0;
            root.previous_page_number = 0;
            root.next_page_number = 0;
            root.entries.clear();
            root.entries.push_back(
                node_entry(head_max, static_cast<std::uint32_t>(head_page_number)));
            root.entries.push_back(
                node_entry(tail_max, static_cast<std::uint32_t>(tail_page_number)));
            return;
        }

        ParsedPage& head_page = allocate(true, 0);
        const std::uint64_t head_page_number = head_page.page_number;
        head_page.entries = std::move(head);
        head_page.previous_page_number = old_previous;
        head_page.next_page_number = static_cast<std::uint32_t>(leaf_page_number);

        leaf.entries = std::move(tail);
        leaf.previous_page_number = static_cast<std::uint32_t>(head_page_number);
        if (old_previous != 0) {
            touch(old_previous).next_page_number = static_cast<std::uint32_t>(head_page_number);
        }

        const std::size_t parent_level = path.pages.size() - 2;
        ParsedPage& parent = touch(path.pages[parent_level]);
        const std::size_t parent_index = path.indices[parent_level];
        if (parent_index >= parent.entries.size() ||
            parent.entries[parent_index].child_page_number != leaf_page_number) {
            invalid(file, "Index parent entry does not reference the split leaf",
                    parent.page_number);
        }
        // The new head leaf precedes the original leaf, and the original leaf now ends at
        // tail_max, so both separators are rewritten in place.
        parent.entries[parent_index] =
            node_entry(tail_max, static_cast<std::uint32_t>(leaf_page_number));
        parent.entries.insert(parent.entries.begin() +
                                  static_cast<std::ptrdiff_t>(parent_index),
                              node_entry(head_max, static_cast<std::uint32_t>(head_page_number)));

        const TreePath parent_tree = parent_path(path);
        if (!entries_fit(parent, file.format().page_size)) {
            split_node(parent_tree);
            return;
        }
        refresh_path_max(parent_tree);
    }

    // Splits a full node page. Node entries hold the maximum key of each child subtree, so the
    // half that moves into the new page keeps the larger keys and the parent receives one extra
    // separator. Root splits allocate two sibling nodes and raise the root by one level.
    void split_node(const TreePath& path) {
        const std::uint64_t node_page_number = path.pages.back();
        ParsedPage& node = page(node_page_number);
        if (node.leaf) {
            invalid(file, "Index split expected a node page", node_page_number);
        }
        const auto head_count =
            split_position(node, file.format().page_size, node.entries.size() / 2);
        if (head_count == 0) {
            unsupported(file, "Index node entries do not fit in two pages", node_page_number);
        }
        std::vector<Jet4IndexEntry> head(node.entries.begin(),
                                         node.entries.begin() +
                                             static_cast<std::ptrdiff_t>(head_count));
        std::vector<Jet4IndexEntry> tail(node.entries.begin() +
                                             static_cast<std::ptrdiff_t>(head_count),
                                         node.entries.end());
        const Jet4IndexEntry head_max = head.back();
        const Jet4IndexEntry tail_max = tail.back();

        if (path.pages.size() == 1) {
            // The root page number is fixed by the table definition, so both halves move into
            // new node pages and the root becomes the next level up.
            ParsedPage& head_page = allocate(false, node.level);
            const std::uint64_t head_page_number = head_page.page_number;
            head_page.entries = std::move(head);

            ParsedPage& tail_page = allocate(false, node.level);
            const std::uint64_t tail_page_number = tail_page.page_number;
            tail_page.entries = std::move(tail);

            ParsedPage& root = touch(root_page_number);
            root.leaf = false;
            root.level = static_cast<std::uint8_t>(node.level + 1);
            root.child_tail_page_number = 0;
            root.previous_page_number = 0;
            root.next_page_number = 0;
            root.entries.clear();
            root.entries.push_back(
                node_entry(head_max, static_cast<std::uint32_t>(head_page_number)));
            root.entries.push_back(
                node_entry(tail_max, static_cast<std::uint32_t>(tail_page_number)));
            return;
        }

        ParsedPage& tail_page = allocate(false, node.level);
        const std::uint64_t tail_page_number = tail_page.page_number;
        tail_page.entries = std::move(tail);
        node.entries = std::move(head);

        const std::size_t parent_level = path.pages.size() - 2;
        ParsedPage& parent = touch(path.pages[parent_level]);
        const std::size_t parent_index = path.indices[parent_level];
        if (parent_index >= parent.entries.size() ||
            parent.entries[parent_index].child_page_number != node_page_number) {
            invalid(file, "Index parent entry does not reference the split node",
                    parent.page_number);
        }
        parent.entries[parent_index] =
            node_entry(head_max, static_cast<std::uint32_t>(node_page_number));
        parent.entries.insert(parent.entries.begin() +
                                  static_cast<std::ptrdiff_t>(parent_index + 1),
                              node_entry(tail_max, static_cast<std::uint32_t>(tail_page_number)));

        const TreePath parent_tree = parent_path(path);
        if (!entries_fit(parent, file.format().page_size)) {
            split_node(parent_tree);
            return;
        }
        refresh_path_max(parent_tree);
    }

    // Detaches an emptied page from the tree. Leaf pages also leave the sibling chain; an empty
    // node is detached from its parent, and the condition bubbles up until an ancestor still
    // has entries or the root is reached. The root falling empty becomes an empty leaf again,
    // which is the state a newly created index starts in.
    void detach_empty_page(const TreePath& path) {
        const std::uint64_t page_number = path.pages.back();
        ParsedPage& target = page(page_number);
        if (target.leaf) {
            const std::uint32_t previous_page_number = target.previous_page_number;
            const std::uint32_t next_page_number = target.next_page_number;
            if (previous_page_number != 0) {
                touch(previous_page_number).next_page_number = next_page_number;
            }
            if (next_page_number != 0) {
                touch(next_page_number).previous_page_number = previous_page_number;
            }
        }

        if (path.pages.size() == 1) {
            // The root keeps its page number and reverts to the empty-leaf layout.
            ParsedPage& root = touch(root_page_number);
            root.leaf = true;
            root.level = 0;
            root.entries.clear();
            root.previous_page_number = 0;
            root.next_page_number = 0;
            root.child_tail_page_number = 0;
            return;
        }

        const TreePath parent_tree = parent_path(path);
        ParsedPage& parent = touch(parent_tree.pages.back());
        // The child entry index belongs to the parent page, which is the last page of the
        // parent path, so its position in `path.indices` is one less than the path length.
        const std::size_t parent_index = path.indices[parent_tree.pages.size() - 1];
        if (parent_index >= parent.entries.size() ||
            parent.entries[parent_index].child_page_number != page_number) {
            invalid(file, "Index parent entry does not reference the emptied page",
                    parent.page_number);
        }
        parent.entries.erase(parent.entries.begin() +
                             static_cast<std::ptrdiff_t>(parent_index));

        modified_pages.erase(page_number);
        freed_pages.push_back(page_number);

        if (!parent.entries.empty()) {
            refresh_path_max(parent_tree);
            return;
        }
        detach_empty_page(parent_tree);
    }

    // Merges an underfull leaf into a sibling that shares its parent node. Only siblings under
    // the same node are candidates: the parent separator list belongs to that node, while a leaf
    // reached through the sibling chain may belong to another subtree. Merging keeps the leaf
    // chain intact and frees the emptied page, which the caller releases in the usage maps.
    bool merge_underfull_leaf(const TreePath& path) {
        if (path.pages.size() < 2) {
            return false;
        }
        const std::uint64_t leaf_page_number = path.pages.back();
        const ParsedPage& leaf = page(leaf_page_number);
        if (!leaf.leaf || leaf.entries.empty()) {
            return false;
        }
        const std::uint32_t page_size = file.format().page_size;
        const std::uint64_t capacity = entry_capacity(page_size);
        const std::uint64_t used =
            stored_entries_size(leaf, 0, leaf.entries.size(), entries_size(leaf.entries, true));
        if (used * 2 > capacity) {
            return false;
        }

        const std::size_t parent_level = path.pages.size() - 2;
        ParsedPage& parent = page(path.pages[parent_level]);
        const std::size_t index = path.indices[parent_level];
        if (index >= parent.entries.size() ||
            parent.entries[index].child_page_number != leaf_page_number) {
            invalid(file, "Index parent entry does not reference the underfull leaf", parent.page_number);
        }
        if (parent.entries.size() < 2) {
            return false;
        }
        const bool merge_right = index + 1 < parent.entries.size();
        const std::size_t sibling_index = merge_right ? index + 1 : index - 1;
        const std::uint64_t sibling_number =
            parent.entries[sibling_index].child_page_number;
        const ParsedPage& sibling = page(sibling_number);
        if (!sibling.leaf || sibling.entries.empty()) {
            invalid(file, "Index sibling of an underfull leaf is not a leaf", sibling_number);
        }

        const std::uint64_t target_number = merge_right ? leaf_page_number : sibling_number;
        const std::uint64_t source_number = merge_right ? sibling_number : leaf_page_number;
        ParsedPage& target = touch(target_number);
        ParsedPage& source = touch(source_number);

        std::vector<Jet4IndexEntry> combined = target.entries;
        combined.insert(combined.end(), source.entries.begin(), source.entries.end());
        ParsedPage probe;
        probe.leaf = true;
        probe.entries = combined;
        if (!entries_fit(probe, page_size)) {
            return false;
        }
        target.entries = std::move(combined);
        target.next_page_number = source.next_page_number;
        if (source.next_page_number != 0) {
            touch(source.next_page_number).previous_page_number =
                static_cast<std::uint32_t>(target_number);
        }

        // The erased separator belongs to the page that disappears: the sibling when merging it
        // into the current leaf, otherwise the current leaf itself.
        const std::size_t erased_index = merge_right ? sibling_index : index;
        ParsedPage& current_parent = touch(path.pages[parent_level]);
        if (erased_index >= current_parent.entries.size() ||
            current_parent.entries[erased_index].child_page_number != source_number) {
            invalid(file, "Index parent entry does not reference the merged leaf",
                    current_parent.page_number);
        }
        current_parent.entries.erase(current_parent.entries.begin() +
                                     static_cast<std::ptrdiff_t>(erased_index));
        modified_pages.erase(source_number);
        freed_pages.push_back(source_number);

        if (merge_right) {
            refresh_path_max(path);
        } else {
            TreePath target_path = path;
            target_path.pages.back() = target_number;
            target_path.indices.back() = index - 1;
            refresh_path_max(target_path);
        }
        return true;
    }

    void remove(const Jet4IndexEntry& entry) {
        const TreePath path = descend_path(entry);
        const std::uint64_t leaf_page_number = path.pages.back();
        ParsedPage& leaf = page(leaf_page_number);
        const auto position = std::find_if(leaf.entries.begin(),
                                           leaf.entries.end(),
                                           [&](const Jet4IndexEntry& candidate) {
                                               return entry_equal(candidate, entry);
                                           });
        if (position == leaf.entries.end()) {
            throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                "Index entry to remove is missing",
                                file.path(),
                                leaf_page_number);
        }
        const std::size_t removed_index = static_cast<std::size_t>(
            std::distance(leaf.entries.begin(), position));
        const std::vector<std::uint8_t> key = entry.key;
        leaf.entries.erase(position);
        const bool leaf_emptied = leaf.entries.empty();
        modified_pages.insert(leaf_page_number);

        bool key_remains = std::any_of(leaf.entries.begin(),
                                       leaf.entries.end(),
                                       [&](const Jet4IndexEntry& candidate) {
                                           return candidate.key == key;
                                       });
        if (!key_remains && (leaf_emptied || removed_index == 0) &&
            leaf.previous_page_number != 0) {
            const ParsedPage& previous = page(leaf.previous_page_number);
            key_remains = previous.leaf && !previous.entries.empty() &&
                          previous.entries.back().key == key;
        }
        if (!key_remains && (leaf_emptied || removed_index == leaf.entries.size()) &&
            leaf.next_page_number != 0) {
            const ParsedPage& next = page(leaf.next_page_number);
            key_remains = next.leaf && !next.entries.empty() && next.entries.front().key == key;
        }
        distinct_key_delta -= key_remains ? 0 : 1;

        if (!leaf_emptied) {
            // A leaf left below half a page is merged with a sibling when the two fit in one
            // page; otherwise removing the greatest entry of a subtree lowers the separators
            // above it.
            if (merge_underfull_leaf(path)) {
                return;
            }
            refresh_path_max(path);
            return;
        }
        detach_empty_page(path);
    }

    Jet4IndexApplyResult finish() {
        Jet4IndexApplyResult result;
        for (const std::uint64_t page_number : modified_pages) {
            result.pages.push_back(
                {page_number, serialize(pages.at(page_number), file.format().page_size)});
        }
        result.appended_pages = appended_pages;
        result.freed_pages = freed_pages;
        result.distinct_key_delta = distinct_key_delta;
        return result;
    }

    std::vector<std::uint8_t> serialize(const ParsedPage& page, std::uint32_t page_size) const {
        if (!entries_fit(page, page_size)) {
            unsupported(file, "Index entries exceed the Jet4 page capacity", page.page_number);
        }
        std::vector<std::uint8_t> bytes(page_size, 0);
        bytes[0] = static_cast<std::uint8_t>(page.leaf ? PageType::index_leaf
                                                       : PageType::index_node);
        bytes[1] = 0x01;
        write_u32(bytes, table_definition_page_offset,
                  static_cast<std::uint32_t>(definition_page_number));
        write_u32(bytes, previous_page_offset, page.previous_page_number);
        write_u32(bytes, next_page_offset, page.next_page_number);
        const bool has_tail = stores_child_tail(page, page.entries.size());
        const auto count = page.entries.size() - (has_tail ? 1 : 0);
        write_u32(bytes, child_tail_page_offset,
                  has_tail ? page.entries.back().child_page_number : 0);
        const auto prefix = count == 0 ? 0 :
            common_key_prefix(page.entries.front(), page.entries[count - 1]);
        write_u16(bytes, compressed_prefix_length_offset, static_cast<std::uint16_t>(prefix));
        bytes[index_level_offset] = page.level;

        const std::uint64_t data_capacity = page_size - entry_data_offset;
        std::uint64_t data_offset = entry_data_offset;
        std::uint64_t cumulative_end = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& entry = page.entries[index];
            const auto skip = cumulative_end == 0 ? 0 : prefix;
            const auto key_size = entry.key.size() - skip;
            std::copy(entry.key.begin() + static_cast<std::ptrdiff_t>(skip), entry.key.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(data_offset));
            data_offset += key_size;
            write_u24_be(bytes, data_offset, entry.row_page_number);
            data_offset += 3;
            bytes[data_offset++] = entry.row_number;
            if (!page.leaf) {
                write_u32_be(bytes, data_offset, entry.child_page_number);
                data_offset += 4;
            }
            cumulative_end += key_size + trailer_size(page.leaf);
            bytes[entry_mask_offset + cumulative_end / 8] |=
                static_cast<std::uint8_t>(1u << (cumulative_end % 8));
        }
        write_u16(bytes, 2, static_cast<std::uint16_t>(data_capacity - cumulative_end));
        return bytes;
    }
};

}

// Descending NULL uses 0xff. Non-NULL entries start with 0x80 and the payload is
// inverted according to the key type.
std::vector<std::uint8_t> descend_asc_index_key(std::vector<std::uint8_t> ascending,
                                               std::uint8_t type_code) {
    if (ascending.size() == 1 && ascending[0] == asc_null_flag) {
        return {0xff};
    }
    ascending[0] = 0x80;
    for (std::size_t index = 1; index < ascending.size(); ++index) {
        // BINARY continuation markers stay 0x09. The final segment length is inverted.
        if (type_code != jet4_binary_type_code || index % 9 != 0 ||
            index == ascending.size() - 1) {
            ascending[index] ^= 0xff;
        }
    }
    return ascending;
}

bool jet4_index_key_type_supported(std::uint8_t type_code) noexcept {
    switch (type_code) {
        case boolean_type:
        case byte_type:
        case short_type:
        case long_type:
        case money_type:
        case float_type:
        case double_type:
        case jet4_binary_type_code:
        case guid_type:
            return true;
        default:
            return false;
    }
}

std::vector<std::uint8_t> encode_jet4_index_key(std::uint8_t type_code, const Value& value) {
    if (type_code == boolean_type) {
        // Jet4 stores BOOLEAN values in the row NULL mask, so a boolean is never NULL when read
        // back; an unset bit means FALSE and the key must match that FALSE entry.
        if (std::holds_alternative<std::monostate>(value)) {
            return {asc_start_flag, 0xff};
        }
        const auto* boolean_value = std::get_if<bool>(&value);
        if (boolean_value == nullptr) {
            throw std::invalid_argument("BOOLEAN index key requires a bool value");
        }
        return {asc_start_flag, static_cast<std::uint8_t>(*boolean_value ? 0x00 : 0xff)};
    }
    if (std::holds_alternative<std::monostate>(value)) {
        return {asc_null_flag};
    }
    switch (type_code) {
        case byte_type: {
            const auto* byte_value = std::get_if<std::int8_t>(&value);
            if (byte_value == nullptr) {
                throw std::invalid_argument("BYTE index key requires an int8 value");
            }
            return encode_integer_key({static_cast<std::uint8_t>(*byte_value)});
        }
        case short_type: {
            const auto* short_value = std::get_if<std::int16_t>(&value);
            if (short_value == nullptr) {
                throw std::invalid_argument("SHORT index key requires an int16 value");
            }
            const auto raw = static_cast<std::uint16_t>(*short_value);
            return encode_integer_key({static_cast<std::uint8_t>((raw >> 8) & 0xff),
                                       static_cast<std::uint8_t>(raw & 0xff)});
        }
        case long_type: {
            const auto* long_value = std::get_if<std::int32_t>(&value);
            if (long_value == nullptr) {
                throw std::invalid_argument("LONG index key requires an int32 value");
            }
            const auto raw = static_cast<std::uint32_t>(*long_value);
            return encode_integer_key({static_cast<std::uint8_t>((raw >> 24) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 16) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 8) & 0xff),
                                       static_cast<std::uint8_t>(raw & 0xff)});
        }
        case money_type: {
            const auto* money_value = std::get_if<DecimalValue>(&value);
            if (money_value == nullptr) {
                throw std::invalid_argument("MONEY index key requires a DecimalValue");
            }
            const auto raw = static_cast<std::uint64_t>(jet4_money_scaled_value(money_value->text));
            return encode_integer_key({static_cast<std::uint8_t>((raw >> 56) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 48) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 40) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 32) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 24) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 16) & 0xff),
                                       static_cast<std::uint8_t>((raw >> 8) & 0xff),
                                       static_cast<std::uint8_t>(raw & 0xff)});
        }
        case guid_type: {
            const auto* guid_value = std::get_if<GuidValue>(&value);
            if (guid_value == nullptr) {
                throw std::invalid_argument("GUID index key requires a GuidValue");
            }
            const auto bytes = jet4_guid_bytes(guid_value->text);
            return encode_general_binary_key(bytes.data(), bytes.size());
        }
        case jet4_binary_type_code: {
            const auto* blob = std::get_if<BlobValue>(&value);
            if (blob == nullptr) {
                throw std::invalid_argument("BINARY index key requires a BlobValue");
            }
            return encode_general_binary_key(blob->bytes.data(), blob->bytes.size());
        }
        case float_type: {
            const auto* float_value = std::get_if<float>(&value);
            if (float_value == nullptr) {
                throw std::invalid_argument("SINGLE index key requires a float value");
            }
            std::uint32_t bits = 0;
            std::memcpy(&bits, float_value, sizeof(bits));
            return encode_floating_key({static_cast<std::uint8_t>((bits >> 24) & 0xff),
                                        static_cast<std::uint8_t>((bits >> 16) & 0xff),
                                        static_cast<std::uint8_t>((bits >> 8) & 0xff),
                                        static_cast<std::uint8_t>(bits & 0xff)});
        }
        case double_type: {
            const auto* double_value = std::get_if<double>(&value);
            if (double_value == nullptr) {
                throw std::invalid_argument("DOUBLE index key requires a double value");
            }
            std::uint64_t bits = 0;
            std::memcpy(&bits, double_value, sizeof(bits));
            std::vector<std::uint8_t> payload(8);
            for (std::size_t index = 0; index < payload.size(); ++index) {
                payload[index] =
                    static_cast<std::uint8_t>((bits >> ((7 - index) * 8)) & 0xff);
            }
            return encode_floating_key(std::move(payload));
        }
        default:
            throw std::invalid_argument("Unsupported Jet4 index key column type");
    }
}

std::vector<std::uint8_t> encode_jet4_index_key(std::uint8_t type_code,
                                                const EncodedColumnValue& encoded) {
    if (type_code == boolean_type) {
        return {asc_start_flag, static_cast<std::uint8_t>(encoded.boolean_value ? 0x00 : 0xff)};
    }
    if (!encoded.bytes.has_value()) {
        return {asc_null_flag};
    }
    const auto& bytes = *encoded.bytes;
    switch (type_code) {
        case byte_type:
            if (bytes.size() < 1) {
                throw std::invalid_argument("BYTE index key requires one stored byte");
            }
            return encode_integer_key({bytes[0]});
        case short_type:
            if (bytes.size() < 2) {
                throw std::invalid_argument("SHORT index key requires two stored bytes");
            }
            return encode_integer_key({bytes[1], bytes[0]});
        case long_type:
            if (bytes.size() < 4) {
                throw std::invalid_argument("LONG index key requires four stored bytes");
            }
            return encode_integer_key({bytes[3], bytes[2], bytes[1], bytes[0]});
        case money_type:
            if (bytes.size() < 8) {
                throw std::invalid_argument("MONEY index key requires eight stored bytes");
            }
            return encode_integer_key({bytes[7], bytes[6], bytes[5], bytes[4],
                                       bytes[3], bytes[2], bytes[1], bytes[0]});
        case guid_type:
            if (bytes.size() < 16) {
                throw std::invalid_argument("GUID index key requires sixteen stored bytes");
            }
            return encode_general_binary_key(bytes.data(), 16);
        case jet4_binary_type_code:
            return encode_general_binary_key(bytes.data(), bytes.size());
        case float_type:
            if (bytes.size() < 4) {
                throw std::invalid_argument("SINGLE index key requires four stored bytes");
            }
            return encode_floating_key({bytes[3], bytes[2], bytes[1], bytes[0]});
        case double_type:
            if (bytes.size() < 8) {
                throw std::invalid_argument("DOUBLE index key requires eight stored bytes");
            }
            return encode_floating_key({bytes[7], bytes[6], bytes[5], bytes[4],
                                        bytes[3], bytes[2], bytes[1], bytes[0]});
        default:
            throw std::invalid_argument("Unsupported Jet4 index key column type");
    }
}

bool jet4_text_sort_order_supported(const Jet4TextSortOrder& sort_order) noexcept {
    if (sort_order.lcid == 0) {
        return true;
    }
    if (sort_order.lcid == 1033) {
        return sort_order.version == 0 || sort_order.version == 1 || sort_order.version == 0xff;
    }
    if (sort_order.version != 0) {
        return false;
    }
    switch (sort_order.lcid) {
        case 1049: // Russian; Jackcess 5.2.0 writes General Legacy weights
        case 1055: // Turkish; Jackcess 5.2.0 writes General Legacy weights
        case 1058: // Ukrainian; Jackcess 5.2.0 writes General Legacy weights
            return true;
        default:
            return false;
    }
}

std::vector<std::uint8_t> truncate_jet4_index_key(std::vector<std::uint8_t> key) {
    if (key.size() <= jet4_max_index_key_length) {
        return key;
    }

    std::uint16_t digest = 0;
    const auto& table = jet4_key_digest_table();
    for (std::size_t index = kept_index_key_length; index < key.size() - 1; ++index) {
        digest = static_cast<std::uint16_t>((digest >> 8) ^ table[digest & 0xff] ^ key[index]);
    }

    key.resize(jet4_max_index_key_length);
    key[kept_index_key_length] = static_cast<std::uint8_t>(digest >> 8);
    key[kept_index_key_length + 1] = static_cast<std::uint8_t>(digest);
    return key;
}

bool jet4_index_key_supported(const ColumnDefinition& column) noexcept {
    if (column.type != text_type && column.type != memo_type) {
        return jet4_index_key_type_supported(column.type);
    }
    return column.text_sort_order && jet4_text_sort_order_supported(*column.text_sort_order);
}

std::vector<std::uint8_t> encode_jet4_index_key(const ColumnDefinition& column,
                                                const Value& value,
                                                bool descending) {
    if (!jet4_index_key_supported(column)) {
        throw std::invalid_argument("Unsupported Jet4 index key column type or text sort order");
    }
    if (column.type == text_type || column.type == memo_type) {
        if (std::holds_alternative<std::monostate>(value)) {
            return {static_cast<std::uint8_t>(descending ? 0xff : 0x00)};
        }

        std::string memo_text;
        std::string_view text;
        if (column.type == text_type) {
            const auto* text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                throw std::invalid_argument("TEXT index key requires a string");
            }
            text = *text_value;
        } else if (const auto* memo_value = std::get_if<MemoValue>(&value)) {
            memo_text = memo_value->raw_bytes.empty()
                            ? memo_value->text
                            : decode_jet4_text(memo_value->raw_bytes);
            text = memo_text;
        } else if (const auto* text_value = std::get_if<std::string>(&value)) {
            // String keys are accepted by index lookup APIs for both TEXT and MEMO columns.
            text = *text_value;
        } else {
            throw std::invalid_argument("MEMO index key requires text");
        }

        const auto units = utf8_to_utf16(text, text.size(), "Text index key exceeds its UTF-16 limit");
        if (column.text_sort_order->lcid == 1033) {
            if (column.text_sort_order->version == 1) {
                return encode_jet4_general_key(units, descending);
            }
            if (column.text_sort_order->version == 0xff) {
                return encode_jet4_general97_key(units, descending);
            }
        }
        return encode_jet4_general_legacy_key(units, descending);
    }

    auto key = encode_jet4_index_key(column.type, value);
    return descending ? descend_asc_index_key(std::move(key), column.type) : std::move(key);
}

std::vector<std::uint8_t> encode_jet4_index_key(const ColumnDefinition& column,
                                                const EncodedColumnValue& encoded,
                                                bool descending) {
    if (column.type == text_type) {
        if (!encoded.bytes) {
            return encode_jet4_index_key(column, Value{}, descending);
        }
        return encode_jet4_index_key(column, Value{decode_jet4_text(*encoded.bytes)}, descending);
    }
    if (column.type == memo_type) {
        if (!encoded.bytes) {
            return encode_jet4_index_key(column, Value{}, descending);
        }
        throw std::invalid_argument("MEMO index keys require the decoded long value");
    }

    auto key = encode_jet4_index_key(column.type, encoded);
    return descending ? descend_asc_index_key(std::move(key), column.type) : std::move(key);
}

Jet4IndexApplyResult apply_jet4_index_changes(const PageFile& file,
                                              const std::string& table_name,
                                              std::uint64_t definition_page_number,
                                              std::uint32_t root_page_number,
                                              std::uint64_t first_new_page_number,
                                              const std::vector<Jet4IndexEntry>& removals,
                                              const std::vector<Jet4IndexEntry>& insertions,
                                              bool allow_split,
                                              bool unique) {
    (void)table_name;
    TreeWriter writer(file,
                      definition_page_number,
                      root_page_number,
                      first_new_page_number,
                      allow_split,
                      unique);
    for (const auto& entry : removals) {
        writer.remove(entry);
    }
    for (const auto& entry : insertions) {
        writer.insert(entry);
    }
    return writer.finish();
}

Jet4IndexApplyResult build_jet4_index(const PageFile& file,
                                     std::uint64_t definition_page_number,
                                     std::uint64_t first_new_page_number,
                                     const std::vector<Jet4IndexEntry>& entries,
                                     bool unique) {
    TreeWriter writer(file,
                      definition_page_number,
                      static_cast<std::uint32_t>(first_new_page_number),
                      first_new_page_number,
                      true,
                      unique);
    writer.allocate(true, 0);
    for (const auto& entry : entries) {
        writer.insert(entry);
    }
    return writer.finish();
}

}

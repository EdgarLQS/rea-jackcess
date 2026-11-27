#pragma once

#include "rea_jackcess/table.hpp"

#include "../catalog/row_reader.hpp"
#include "../catalog/table_definition.hpp"
#include "../page/page_file.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// One entry to insert into or remove from an index tree.
struct Jet4IndexEntry {
    std::vector<std::uint8_t> key;    // Encoded index key bytes (empty for an all-NULL key).
    std::uint32_t row_page_number = 0; // Data page of the referenced table row.
    std::uint8_t row_number = 0;       // Row slot of the referenced table row.
    // Node entries point at a child index page; leaf entries leave this zero.
    std::uint32_t child_page_number = 0;
    bool all_columns_null = false;     // True when every key column is NULL (affects uniqueness).
};

// A page image produced by an index operation, ready to be written.
struct Jet4IndexPageWrite {
    std::uint64_t page_number;       // Page to overwrite.
    std::vector<std::uint8_t> bytes; // Full new page contents.
};

struct Jet4IndexApplyResult {
    // Modified existing pages and newly allocated pages, all ready to be written.
    std::vector<Jet4IndexPageWrite> pages;
    // Subset of pages that must be appended at the end of the file, in ascending order.
    std::vector<std::uint64_t> appended_pages;
    // Pages that left the tree because their leaf was merged away; the caller must release them
    // in the index usage map and in the global page map.
    std::vector<std::uint64_t> freed_pages;
    // Change of the Jet4 distinct-key count: +1 for a new key, -1 when the last entry with a
    // key disappears, 0 otherwise.
    std::int64_t distinct_key_delta = 0;
};

// Single-column key support for both index directions, including the stored text sort order.
// True when `sort_order` names a collation the writer can encode keys for.
bool jet4_text_sort_order_supported(const Jet4TextSortOrder& sort_order) noexcept;
// True when the writer can build index keys for `column` (supported type and collation).
bool jet4_index_key_supported(const ColumnDefinition& column) noexcept;
inline constexpr std::size_t jet4_max_index_key_length = 510;

// Access truncates complete index keys above 510 bytes and replaces two bytes with a CRC-16 digest.
std::vector<std::uint8_t> truncate_jet4_index_key(std::vector<std::uint8_t> key);

std::vector<std::uint8_t> encode_jet4_index_key(const ColumnDefinition& column,
                                                const Value& value,
                                                bool descending);
// Encodes a key from the stored column bytes of an existing row.
std::vector<std::uint8_t> encode_jet4_index_key(const ColumnDefinition& column,
                                                const EncodedColumnValue& encoded,
                                                bool descending);

// Applies removals and then insertions to the index tree rooted at root_page_number, using one
// consistent view of the pages so that several keys can be applied in one operation. A full
// leaf is split into two leaf pages and the root leaf is first nested under a new node page;
// full nodes split recursively and a full root raises the tree level. A leaf emptied by removals
// is unlinked from the leaf chain and dropped from its parent, and its page is returned in
// freed_pages. Nodes that keep a child tail and compressed page prefixes are rejected instead
// of written. When `unique` is set, an insertion with a key that already exists is rejected.
//
// first_new_page_number must be the page number the caller will append next, and every
// allocated page is appended in the returned order. With allow_split false a full leaf is
// rejected instead, which is used by callers that cannot append index pages.
Jet4IndexApplyResult apply_jet4_index_changes(const PageFile& file,
                                              const std::string& table_name,
                                              std::uint64_t definition_page_number,
                                              std::uint32_t root_page_number,
                                              std::uint64_t first_new_page_number,
                                              const std::vector<Jet4IndexEntry>& removals,
                                              const std::vector<Jet4IndexEntry>& insertions,
                                              bool allow_split,
                                              bool unique = false);

// Builds a new tree entirely in memory. The first appended page is its root.
Jet4IndexApplyResult build_jet4_index(const PageFile& file,
                                     std::uint64_t definition_page_number,
                                     std::uint64_t first_new_page_number,
                                     const std::vector<Jet4IndexEntry>& entries,
                                     bool unique);

}

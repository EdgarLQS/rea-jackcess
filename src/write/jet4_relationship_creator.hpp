#pragma once

#include "../catalog/table_catalog.hpp"
#include "../page/page_file.hpp"

#include "jet4_row_writer.hpp"
#include "rea_jackcess/database.hpp"

#include <string>
#include <utility>
#include <vector>

namespace rea_jackcess::detail {

struct RelationshipDefinition {
    std::string name;
    std::string from_table;
    std::string to_table;
    // Parent column first, child column second, in mapping order.
    std::vector<std::pair<std::string, std::string>> columns;
    bool referential_integrity = true;
    bool cascade_updates = false;
    bool cascade_deletes = false;
    bool cascade_null_on_delete = false;
    RelationshipJoinType join_type = RelationshipJoinType::inner;
};

// A rewritten table definition: the first page, its continuation chain in stream order, and the
// continuation pages appended past the current end of the file.
struct DefinitionWrite {
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> pages;
    std::vector<std::uint64_t> new_pages;
    std::uint64_t next_free_page = 0;
};

// Builds the pages of a definition with one more logical index, repacking the whole logical
// stream so definitions that already span pages can grow again. A null physical index only adds
// a logical alias. `next_free_page` is the first page number that may be appended.
DefinitionWrite definition_with_added_jet4_index(const PageFile& file,
                                                const ReadableTableSchema& table,
                                                const TableDefinition& definition,
                                                const LogicalIndexDefinition& index,
                                                const PhysicalIndexDefinition* physical,
                                                std::uint64_t next_free_page);

// Merges a definition write into staged pages and registers appended pages in the global map.
void stage_definition_write(const PageFile& file, Jet4StagedPages& staged, DefinitionWrite write);

// Returns the catalog positions of tables whose index definitions changed.
std::vector<std::size_t> create_jet4_relationship(Jet4WriteContext& context,
                                                  const RelationshipDefinition& definition);

}

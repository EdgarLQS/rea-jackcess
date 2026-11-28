#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

// Classifies the file (Jet4 with an AccessVersion, ACE, MSISAM, ...) from its header and, for
// Jet4, the AccessVersion property.
FileFormat identify_file_format(const PageFile& file);

// One object row from the MSysObjects system catalog.
struct SystemCatalogObject {
    std::optional<std::int32_t> id;              // MSysObjects.Id.
    std::optional<std::int32_t> parent_id;       // MSysObjects.ParentId.
    std::optional<std::string> name;             // MSysObjects.Name.
    std::optional<std::int16_t> type;            // MSysObjects.Type (1 = table, 5 = query, ...).
    std::optional<std::int32_t> flags;           // MSysObjects.Flags (bit 0x08 = hidden).
    std::uint64_t page_number = 0;               // Physical page of the catalog row.
    std::uint16_t row_number = 0;                // Physical row slot of the catalog row.
    bool property_column_present = false;        // True when the LvProp property column exists on this row.
    std::optional<std::vector<std::uint8_t>> property_bytes; // Raw LvProp bytes for the row.
    std::optional<std::string> property_error;   // Error text if the property bytes could not be read.
    std::optional<std::uint64_t> property_error_page; // Page associated with that error.
    std::optional<std::string> linked_database;  // DatabaseName stored on a linked-table row.
    std::optional<std::string> linked_table;     // ForeignName (target table) on a linked-table row.
    std::optional<std::string> connection_string; // ODBC connect string on a linked-table row.
};

// Reads the object rows of MSysObjects. `property_object_name`, when set, selects the column whose
// LvProp bytes to read; `read_link_fields` additionally reads the linked-table target columns.
std::vector<SystemCatalogObject> read_system_catalog_objects(const PageFile& file,
                                                              const TableDefinition& definition,
                                                              std::optional<std::string_view> property_object_name =
                                                                  std::nullopt,
                                                              bool read_link_fields = false);

}

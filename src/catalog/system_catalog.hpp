#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

struct SystemCatalogObject {
    std::optional<std::int32_t> id;
    std::optional<std::int32_t> parent_id;
    std::optional<std::string> name;
    std::optional<std::int16_t> type;
    std::optional<std::int32_t> flags;
    std::uint64_t page_number = 0;
    std::uint16_t row_number = 0;
    bool property_column_present = false;
    std::optional<std::vector<std::uint8_t>> property_bytes;
    std::optional<std::string> property_error;
    std::optional<std::uint64_t> property_error_page;
};

std::vector<SystemCatalogObject> read_system_catalog_objects(const PageFile& file,
                                                              const TableDefinition& definition,
                                                              std::optional<std::string_view> property_object_name =
                                                                  std::nullopt);

}

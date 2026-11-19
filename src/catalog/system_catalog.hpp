#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

struct SystemCatalogObject {
    std::optional<std::int32_t> id;
    std::optional<std::int32_t> parent_id;
    std::optional<std::string> name;
    std::optional<std::int16_t> type;
    std::optional<std::int32_t> flags;
};

std::vector<SystemCatalogObject> read_system_catalog_objects(const PageFile& file,
                                                              const TableDefinition& definition);

}

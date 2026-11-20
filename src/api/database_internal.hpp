#pragma once

#include "catalog/table_catalog.hpp"
#include "page/page_file.hpp"
#include "rea_jackcess/database.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace rea_jackcess::detail {

struct DatabaseState {
    DatabaseState(PageFile file, TableSchemaCatalog schema);

    PageFile file;
    TableSchemaCatalog schema;
    std::vector<std::vector<ColumnInfo>> columns;
    std::vector<std::vector<IndexInfo>> indexes;
    std::vector<DatabaseDiagnostic> diagnostics;
};

struct TableState {
    std::shared_ptr<DatabaseState> database;
    std::size_t schema_index;
};

}

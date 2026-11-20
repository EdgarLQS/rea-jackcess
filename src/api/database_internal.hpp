#pragma once

#include "catalog/table_catalog.hpp"
#include "page/page_file.hpp"
#include "page/page_file_write_lock.hpp"
#include "rea_jackcess/database.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace rea_jackcess::detail {

struct DatabaseState {
    DatabaseState(PageFile file,
                  TableSchemaCatalog schema,
                  bool writable,
                  std::optional<PageFileWriteLock> write_lock);

    PageFile file;
    TableSchemaCatalog schema;
    bool writable;
    std::optional<PageFileWriteLock> write_lock;
    bool write_state_valid = true;
    std::mutex write_mutex;
    std::vector<std::vector<ColumnInfo>> columns;
    std::vector<std::vector<IndexInfo>> indexes;
    std::vector<DatabaseDiagnostic> diagnostics;
};

struct TableState {
    std::shared_ptr<DatabaseState> database;
    std::size_t schema_index;
};

}

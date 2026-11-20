#pragma once

#include "../catalog/table_catalog.hpp"
#include "../page/page_file.hpp"

#include "rea_jackcess/table.hpp"

#include <cstddef>
#include <mutex>
#include <vector>

namespace rea_jackcess::detail {

struct Jet4WriteContext {
    PageFile& file;
    TableSchemaCatalog& schema;
    bool writable;
    bool& write_state_valid;
    std::mutex& write_mutex;
};

std::vector<RowId> append_jet4_rows(Jet4WriteContext& context,
                                    std::size_t table_index,
                                    const std::vector<std::vector<Value>>& rows);

}

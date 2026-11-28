#pragma once

#include "../catalog/table_catalog.hpp"
#include "../page/page_file.hpp"

#include "rea_jackcess/table.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace rea_jackcess::detail {

class PageWriteUndo;

// Everything a Jet4 write operation needs: the file/schema being edited, the write gate, and the
// optional rollback and validation hooks. References point at the owning DatabaseState.
struct Jet4WriteContext {
    PageFile& file;                                             // Database being written.
    TableSchemaCatalog& schema;                                 // In-memory catalog kept in sync.
    bool writable;                                              // Whether writes are permitted.
    bool& write_state_valid;                                    // Cleared on a failed/unrecoverable write.
    std::recursive_mutex& write_mutex;                          // Serializes concurrent writes.
    PageWriteUndo* undo = nullptr;                              // Optional before-image recorder.
    const std::vector<std::vector<ColumnInfo>>* columns = nullptr;             // Public column info (for validation).
    const std::vector<std::vector<ColumnValidator>>* column_validators = nullptr; // Per-column validators.
};

// Runs `operation` under the write mutex, rejecting it when the handle is not writable or its write
// state was invalidated by an earlier failure.
void run_jet4_write_operation(Jet4WriteContext& context, const std::function<void()>& operation);

// Pages staged by one index/schema operation, ready to be written in a single batch.
struct Jet4StagedPages {
    std::map<std::uint64_t, std::vector<std::uint8_t>> pages; // Overwrites keyed by page number.
    std::vector<std::uint64_t> appended;                      // New pages to append, in order.
};

Jet4StagedPages stage_jet4_index(const PageFile& file, const ReadableTableSchema& table,
                                 const TableDefinition& definition, PhysicalIndexDefinition& physical,
                                 bool primary);
void write_jet4_schema_pages(
    Jet4WriteContext& context, const PageFile& file, const std::string& table_name,
    const Jet4StagedPages& staged,
    const std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>>& definitions);

std::vector<RowId> append_jet4_rows(Jet4WriteContext& context,
                                    std::size_t table_index,
                                    const std::vector<std::vector<Value>>& rows);

// Appends rows to a local catalog table such as MSysRelationships. Callers outside this
// package must not use it to bypass the user-table restriction on ordinary rows.
std::vector<RowId> append_jet4_catalog_rows(Jet4WriteContext& context,
                                            std::size_t table_index,
                                            const std::vector<std::vector<Value>>& rows);

void update_jet4_catalog_properties(Jet4WriteContext& context, std::size_t table_index,
                                    RowId row_id, const std::vector<std::uint8_t>& bytes);

void update_jet4_row(Jet4WriteContext& context,
                     std::size_t table_index,
                     RowId row_id,
                     const std::vector<Value>& values);

TableDefinition add_jet4_index(Jet4WriteContext& context, std::size_t table_index, const IndexBuilder& index);

void delete_jet4_row(Jet4WriteContext& context, std::size_t table_index, RowId row_id);

}

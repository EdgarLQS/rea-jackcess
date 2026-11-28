#pragma once

#include "catalog/table_catalog.hpp"
#include "page/page_file.hpp"
#include "page/page_file_write_lock.hpp"
#include "rea_jackcess/database.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// A scratch directory (holding temporary databases for custom linked sources) removed on destruction.
struct TemporaryDatabaseDirectory {
    std::filesystem::path path; // Directory to delete.

    ~TemporaryDatabaseDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// Shared, reference-counted state behind a Database handle: the open file, its parsed schema, the
// write-mode bookkeeping, linked-source caches, and the per-table public metadata.
struct DatabaseState {
    // `writable` enables writes; `write_lock` is engaged for a read-write handle.
    DatabaseState(PageFile file,
                  TableSchemaCatalog schema,
                  bool writable,
                  std::optional<PageFileWriteLock> write_lock);

    // Declared before PageFile so its directory is removed after the file handle closes.
    std::shared_ptr<TemporaryDatabaseDirectory> temporary_directory;
    // Retained until after PageFile closes, matching custom-source lifetime to linked tables.
    std::shared_ptr<CustomLinkedSource> custom_source;
    PageFile file;                  // The open database file (page I/O, codec, decode).
    TableSchemaCatalog schema;      // Every table definition read from the catalog.
    bool writable;                  // True for a read-write handle.
    std::optional<PageFileWriteLock> write_lock; // Exclusive advisory lock held while writable.
    bool write_state_valid = true;  // Cleared when a failed write left the file in an unknown state.
    std::recursive_mutex write_mutex;       // Serializes write operations within this handle.
    std::mutex linked_databases_mutex;      // Guards the two linked-source caches below.
    std::map<std::string, std::shared_ptr<DatabaseState>> linked_databases; // Cached linked targets by name.
    std::map<std::string, std::shared_ptr<CustomLinkedSource>> custom_linked_sources; // Custom sources by name.
    std::vector<std::vector<ColumnInfo>> columns;           // Public column info per table.
    std::vector<std::vector<ColumnValidator>> column_validators; // Per-column write validators.
    ErrorHandler error_handler;                             // Database-level decode error handler.
    std::vector<std::vector<IndexInfo>> indexes;            // Public index info per table.
    std::vector<DatabaseDiagnostic> diagnostics;            // Non-fatal problems encountered while opening.
};

// Chooses a unique, valid relationship name for a new relationship on `database`.
std::string prepare_relationship_name(const Database& database, const RelationshipBuilder& relationship);

// Shared state behind a Table handle: points into the owning database's schema by index.
struct TableState {
    std::shared_ptr<DatabaseState> database; // Owning database state.
    std::size_t schema_index;                // Index of this table in DatabaseState::schema.tables.
    ErrorHandler error_handler;              // Table-level decode error handler.
};

}

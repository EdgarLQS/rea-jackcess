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

struct TemporaryDatabaseDirectory {
    std::filesystem::path path;

    ~TemporaryDatabaseDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct DatabaseState {
    DatabaseState(PageFile file,
                  TableSchemaCatalog schema,
                  bool writable,
                  std::optional<PageFileWriteLock> write_lock);

    // Declared before PageFile so its directory is removed after the file handle closes.
    std::shared_ptr<TemporaryDatabaseDirectory> temporary_directory;
    // Retained until after PageFile closes, matching custom-source lifetime to linked tables.
    std::shared_ptr<CustomLinkedSource> custom_source;
    PageFile file;
    TableSchemaCatalog schema;
    bool writable;
    std::optional<PageFileWriteLock> write_lock;
    bool write_state_valid = true;
    std::recursive_mutex write_mutex;
    std::mutex linked_databases_mutex;
    std::map<std::string, std::shared_ptr<DatabaseState>> linked_databases;
    std::map<std::string, std::shared_ptr<CustomLinkedSource>> custom_linked_sources;
    std::vector<std::vector<ColumnInfo>> columns;
    std::vector<std::vector<ColumnValidator>> column_validators;
    ErrorHandler error_handler;
    std::vector<std::vector<IndexInfo>> indexes;
    std::vector<DatabaseDiagnostic> diagnostics;
};

std::string prepare_relationship_name(const Database& database, const RelationshipBuilder& relationship);

struct TableState {
    std::shared_ptr<DatabaseState> database;
    std::size_t schema_index;
    ErrorHandler error_handler;
};

}

#pragma once

#include "database_file.hpp"
#include "page_codec.hpp"
#include "query.hpp"
#include "table.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess {

namespace detail {
struct DatabaseState;
}

struct DatabaseDiagnostic {
    std::optional<std::string> table_name;
    std::string message;
    std::optional<std::uint64_t> page_number;
};

enum class LinkedTableKind { file, odbc };

struct LinkedTableInfo {
    std::string name;
    LinkedTableKind kind;
    std::optional<std::string> target_database;
    std::optional<std::string> target_table;
    // Stored ODBC connection text may contain credentials.
    std::optional<std::string> connection_string;
};

// Materialized rows for a table from a non-Access linked source. The schema name must match
// the requested table name. The temporary database inherits the linked source database's access mode.
struct LinkedTableSnapshot {
    TableBuilder schema;
    std::vector<std::vector<Value>> rows;
};

// State for one non-Access source. The object is retained while any linked table from it remains
// open. Table loading is lazy and serialized; returning nullopt means that this source has no such
// table, so file-path fallback is not attempted.
class CustomLinkedSource {
public:
    virtual ~CustomLinkedSource() = default;

    std::optional<LinkedTableSnapshot> load_table(std::string_view table_name) {
        std::lock_guard<std::mutex> lock(load_mutex_);
        return load_table_impl(table_name);
    }

protected:
    virtual std::optional<LinkedTableSnapshot> load_table_impl(std::string_view table_name) = 0;

private:
    std::mutex load_mutex_;
};

// Return custom source state, or nullptr to continue with the file path resolver.
using CustomLinkedSourceResolver = std::function<std::shared_ptr<CustomLinkedSource>(
    const std::filesystem::path& source, const LinkedTableInfo& link)>;

using LinkedTableResolver = std::function<std::filesystem::path(
    const std::filesystem::path& source, const LinkedTableInfo& link)>;

enum class DatabaseOpenMode {
    read_only,
    read_write,
};

enum class RelationshipJoinType { inner, left_outer, right_outer };

struct RelationshipColumnInfo {
    std::string from_column;
    std::string to_column;
};

struct RelationshipInfo {
    std::string name;
    std::string from_table;
    std::string to_table;
    std::vector<RelationshipColumnInfo> columns;
    std::uint32_t raw_flags;
    bool one_to_one;
    bool referential_integrity;
    bool cascade_updates;
    bool cascade_deletes;
    bool cascade_null_on_delete;
    RelationshipJoinType join_type;
};

// Collects the endpoints, column pairs and flags of a relationship created on an open,
// read-write database. Referential integrity requires a unique index on the referenced columns.
// A suitable child index is reused or created automatically.
class RelationshipBuilder {
public:
    RelationshipBuilder(std::string name, std::string from_table, std::string to_table);

    // The parent column comes first, the child column second, in mapping order.
    RelationshipBuilder& add_columns(std::string from_column, std::string to_column);
    RelationshipBuilder& with_referential_integrity(bool enabled = true);
    RelationshipBuilder& with_cascade_updates(bool enabled = true);
    RelationshipBuilder& with_cascade_deletes(bool enabled = true);
    RelationshipBuilder& with_cascade_null_on_delete(bool enabled = true);
    RelationshipBuilder& with_join_type(RelationshipJoinType join_type);

    const std::string& name() const noexcept;
    const std::string& from_table() const noexcept;
    const std::string& to_table() const noexcept;
    const std::vector<RelationshipColumnInfo>& columns() const noexcept;
    bool referential_integrity() const noexcept;
    bool cascade_updates() const noexcept;
    bool cascade_deletes() const noexcept;
    bool cascade_null_on_delete() const noexcept;
    RelationshipJoinType join_type() const noexcept;

private:
    std::string name_;
    std::string from_table_;
    std::string to_table_;
    std::vector<RelationshipColumnInfo> columns_;
    bool referential_integrity_ = true;
    bool cascade_updates_ = false;
    bool cascade_deletes_ = false;
    bool cascade_null_on_delete_ = false;
    RelationshipJoinType join_type_ = RelationshipJoinType::inner;
};

// Accessors on a moved-from Database throw std::logic_error.
// Options applied when a database is opened read-only.
struct DatabaseOptions {
    // Forces the code page used to decode legacy Jet3 text. 1252 (the default), 819/28591,
    // 936 (GBK) and 1251 are supported; any other value is rejected by the open call.
    std::optional<std::uint16_t> code_page;
    // Supplies the page codec for an encoded database. Leave empty for plain files: opening one
    // whose header declares encoding fails with unsupported_operation instead of decoding garbage.
    PageCodecFactory page_codec;
};

class Database {
public:
    // Opens an existing Jet4 or ACE database read-only. Other formats remain unsupported here.
    static Database open(const std::filesystem::path& path);

    // Opens an existing Jet4 database with the requested access mode. ACE currently permits read-only mode only.
    // Read-write handles hold an exclusive advisory lock until all dependent handles are closed.
    static Database open(const std::filesystem::path& path, DatabaseOpenMode mode);

    // Opens a database whose bytes already live in memory. The buffer must contain the whole
    // file; the handle is read-only and reports "<memory>" as its path in diagnostics.
    static Database open(const std::vector<std::uint8_t>& bytes);

    // Read-only opens with explicit options.
    static Database open(const std::filesystem::path& path, DatabaseOptions options);
    static Database open(const std::vector<std::uint8_t>& bytes, DatabaseOptions options);

    // Creates a Jet4 database, then opens it for reading.
    // Replacing an existing file fails while it has an active read-write handle.
    // Concurrent calls to either create overload targeting the same path must be serialized by the caller.
    static Database create(const std::filesystem::path& path, DatabaseFormat format);

    // Creates a Jet4 V2000 or V2003 database with the supplied empty table schemas, then opens it for reading.
    // Current schema writing supports BOOLEAN/BYTE/SHORT/LONG/MONEY/SINGLE/DOUBLE/GUID/TEXT/BINARY/MEMO/OLE columns whose definition
    // fits one page (MONEY uses four decimal places; TEXT defaults to 255 UTF-16 characters and BINARY to 255 bytes),
    // and 1-64 character ASCII names containing letters, digits, spaces, or underscores.
    // The returned Database handle remains read-only.
    static Database create(const std::filesystem::path& path,
                           DatabaseFormat format,
                           const std::vector<TableBuilder>& tables);

    const std::filesystem::path& path() const;
    const FormatInfo& format() const;
    FileFormat file_format() const;
    // Diagnostics include tables that could not be described, such as linked tables.
    const std::vector<DatabaseDiagnostic>& diagnostics() const;
    // Returns local tables discovered in the database, including system tables.
    std::vector<Table> tables() const;
    // Returns the stored Jet4 database property blocks.
    std::vector<PropertyMapInfo> property_maps() const;
    // Writes the raw property records; changing only PropertyInfo::value has no effect.
    void save_property_maps(const std::vector<PropertyMapInfo>& maps);
    // Installs per-column validators used by subsequent local table inserts and updates.
    // Returning an empty validator keeps the column's default behavior.
    void set_column_validator_factory(ColumnValidatorFactory factory);
    // Handles column decoding failures for tables opened from this Database; empty rethrows.
    void set_error_handler(ErrorHandler handler);

    // Reads local catalog metadata without opening linked targets.
    std::vector<LinkedTableInfo> linked_tables() const;
    void create_linked_table(std::string name, std::string target_database, std::string target_table);

    // Opens a file-linked table using only paths returned by the resolver. The target inherits the
    // source database's access mode; each source state caches linked targets by stored file name.
    // The returned Table retains its target state after the source handle is destroyed.
    Table open_linked_table(std::string_view name, const LinkedTableResolver& resolver) const;

    // Resolves and caches custom source state first, then loads requested tables on demand into
    // temporary Jet4 databases. A null source falls back to the file path resolver; a custom
    // source that lacks the table does not fall back and returns nullopt. Temporary databases
    // inherit the source database's access mode. Pass an empty file resolver when all links are
    // handled by custom sources.
    std::optional<Table> open_linked_table(std::string_view name,
                                           const LinkedTableResolver& file_resolver,
                                           const CustomLinkedSourceResolver& custom_resolver) const;

    // Reads saved Jet4 query metadata without executing queries.
    std::vector<QueryInfo> queries() const;

    // Reads Jet4 relationships whose two endpoints are available as local tables.
    std::vector<RelationshipInfo> relationships(bool include_system_tables = false) const;
    // Declares a relationship on a read-write handle: it appends the two index descriptors and
    // the MSysRelationships rows. The child must already index its foreign key columns.
    void create_relationship(const RelationshipBuilder& relationship);

private:
    explicit Database(std::shared_ptr<detail::DatabaseState> state);

    std::shared_ptr<detail::DatabaseState> state_;
};

}

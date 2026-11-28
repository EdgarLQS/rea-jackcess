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

// A non-fatal problem found while describing a database (for example a table that could not be
// described). Diagnostics do not abort the open; they are reported through Database::diagnostics().
struct DatabaseDiagnostic {
    // Table the diagnostic concerns, when applicable.
    std::optional<std::string> table_name;
    // Human-readable description of the problem.
    std::string message;
    // Page number involved, when the problem is page-scoped.
    std::optional<std::uint64_t> page_number;
};

// Kind of linked (external) table reference.
enum class LinkedTableKind { file, odbc }; // file = link to another Access file; odbc = link via ODBC.

// Stored metadata of a linked table, as read from the system catalog (the target is not opened).
struct LinkedTableInfo {
    // Link name as it appears in the database.
    std::string name;
    // Whether the link targets a file or an ODBC source.
    LinkedTableKind kind;
    // Stored target database path (file links) or DSN/database (ODBC), if recorded.
    std::optional<std::string> target_database;
    // Stored name of the table inside the target database.
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

// Access mode requested when opening an existing database.
enum class DatabaseOpenMode {
    read_only,  // No pages are written; no lock is taken.
    read_write, // Writes are allowed; takes an exclusive advisory lock on the file.
};

// Join type of a relationship between two tables.
enum class RelationshipJoinType { inner, left_outer, right_outer };

// One column mapping of a relationship (parent column -> child column).
struct RelationshipColumnInfo {
    // Column on the parent (referenced) table.
    std::string from_column;
    // Column on the child (referencing) table.
    std::string to_column;
};

// One relationship (foreign-key link between two local tables) read from the database.
struct RelationshipInfo {
    // Relationship name.
    std::string name;
    // Parent (referenced) table name.
    std::string from_table;
    // Child (referencing) table name.
    std::string to_table;
    // The column mappings in order.
    std::vector<RelationshipColumnInfo> columns;
    // Raw relationship flags word as stored.
    std::uint32_t raw_flags;
    // True for a one-to-one relationship.
    bool one_to_one;
    // True when referential integrity is enforced.
    bool referential_integrity;
    // Cascade actions declared on the relationship.
    bool cascade_updates;
    bool cascade_deletes;
    bool cascade_null_on_delete;
    // Relationship join type.
    RelationshipJoinType join_type;
};

// Collects the endpoints, column pairs and flags of a relationship created on an open,
// read-write database. Referential integrity requires a unique index on the referenced columns.
// A suitable child index is reused or created automatically.
class RelationshipBuilder {
public:
    RelationshipBuilder(std::string name, std::string from_table, std::string to_table);

    // The parent column comes first, the child column second, in mapping order.
    // @param from_column parent (referenced) column name.
    // @param to_column child (referencing) column name.
    RelationshipBuilder& add_columns(std::string from_column, std::string to_column);
    // Enables/disables referential integrity (the optional flag sets the state; default true).
    RelationshipBuilder& with_referential_integrity(bool enabled = true);
    RelationshipBuilder& with_cascade_updates(bool enabled = true);
    RelationshipBuilder& with_cascade_deletes(bool enabled = true);
    RelationshipBuilder& with_cascade_null_on_delete(bool enabled = true);
    // Sets the relationship's join type (inner / left outer / right outer).
    RelationshipBuilder& with_join_type(RelationshipJoinType join_type);

    // Accessors for the declared relationship.
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
// A Database value is a lightweight handle to shared internal state; moving it transfers the
// handle, and the last handle to be destroyed closes the file.
struct DatabaseOptions {
    // Forces the code page used to decode legacy Jet3 text. 1252 (the default), 819/28591,
    // 936 (GBK), 932 (Shift-JIS) and 1251 are supported; any other value is rejected by the open
    // call.
    std::optional<std::uint16_t> code_page;
    // Supplies the page codec for an encoded database. Leave empty for plain files: opening one
    // whose header declares encoding fails with unsupported_operation instead of decoding garbage.
    PageCodecFactory page_codec;
};

// A handle to an open database. Copy operations are not provided; move transfers the handle.
class Database {
public:
    // Opens an existing Jet4 or ACE database read-only. Other formats remain unsupported here.
    // @param path database file to open.
    // @throws DatabaseError on I/O failure, an unsupported format, or a file needing a codec.
    static Database open(const std::filesystem::path& path);

    // Opens an existing Jet4 database with the requested access mode. ACE currently permits read-only mode only.
    // Read-write handles hold an exclusive advisory lock until all dependent handles are closed.
    // @param path database file to open.
    // @param mode read_only or read_write.
    static Database open(const std::filesystem::path& path, DatabaseOpenMode mode);

    // Opens a database whose bytes already live in memory. The buffer must contain the whole
    // file; the handle is read-only and reports "<memory>" as its path in diagnostics.
    // @param bytes the complete file contents.
    static Database open(const std::vector<std::uint8_t>& bytes);

    // Read-only opens with explicit options.
    // @param options code page / page codec settings.
    static Database open(const std::filesystem::path& path, DatabaseOptions options);
    static Database open(const std::vector<std::uint8_t>& bytes, DatabaseOptions options);

    // Creates a Jet4 database, then opens it for reading.
    // Replacing an existing file fails while it has an active read-write handle.
    // Concurrent calls to either create overload targeting the same path must be serialized by the caller.
    // @param path destination file (created or replaced).
    // @param format v2000 or v2003.
    static Database create(const std::filesystem::path& path, DatabaseFormat format);

    // Creates a Jet4 V2000 or V2003 database with the supplied empty table schemas, then opens it for reading.
    // Current schema writing supports BOOLEAN/BYTE/SHORT/LONG/MONEY/SINGLE/DOUBLE/GUID/TEXT/BINARY/MEMO/OLE columns whose definition
    // fits one page (MONEY uses four decimal places; TEXT defaults to 255 UTF-16 characters and BINARY to 255 bytes),
    // and 1-64 character ASCII names containing letters, digits, spaces, or underscores.
    // The returned Database handle remains read-only.
    // @param path destination file.
    // @param format v2000 or v2003.
    // @param tables the empty tables to create (columns and initial indexes).
    static Database create(const std::filesystem::path& path,
                           DatabaseFormat format,
                           const std::vector<TableBuilder>& tables);

    // Database file path ("<memory>" for an in-memory open).
    const std::filesystem::path& path() const;
    // Format metadata (engine version, page size, read-only).
    const FormatInfo& format() const;
    // Jackcess-style finer format classification.
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
    // @param include_system_tables include relationships involving system tables.
    std::vector<RelationshipInfo> relationships(bool include_system_tables = false) const;
    // Declares a relationship on a read-write handle: it appends the two index descriptors and
    // the MSysRelationships rows. The child must already index its foreign key columns.
    // @param relationship the relationship to create (endpoints, column pairs, flags).
    void create_relationship(const RelationshipBuilder& relationship);

private:
    explicit Database(std::shared_ptr<detail::DatabaseState> state);

    std::shared_ptr<detail::DatabaseState> state_;
};

}

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rea_jackcess {

// Engine family detected from the file header.
enum class EngineVersion {
    jet3,    // Jet 3 (Access 97, .mdb).
    jet4,    // Jet 4 (Access 2000/2003, .mdb).
    ace2007, // ACE 2007 (.accdb).
    ace2010, // ACE 2010 (.accdb).
    ace2016, // ACE 2016 (.accdb).
    ace2019, // ACE 2019 (.accdb).
    msisam,  // MSISAM container (.mda / encoded).
};

// Finer format classification (Jackcess-style), combining the engine with the Access version.
enum class FileFormat {
    v1997,          // Jet3 / Access 97.
    generic_jet4,   // Jet4 without a recognised AccessVersion property.
    v2000,          // Jet4 Access 2000.
    v2003,          // Jet4 Access 2003.
    v2007,          // ACE 2007.
    v2010,          // ACE 2010.
    v2016,          // ACE 2016.
    v2019,          // ACE 2019.
    msisam,         // MSISAM container.
};

// Target format for Database::create (new database).
enum class DatabaseFormat {
    v2000, // Create an Access 2000 (Jet4 V2000) database.
    v2003, // Create an Access 2003 (Jet4 V2003) database.
};

// Stable failure categories reported through DatabaseError.
enum class DatabaseErrorCode {
    io_error,
    truncated_file,
    unsupported_version,
    corrupted_file,
    page_out_of_range,
    unsupported_operation,
    // A requested write would break a constraint the file already declares, so the
    // operation is rejected without changing any page.
    constraint_violation,
};

// Optional extra context attached to a DatabaseError, filled in as far as it is known.
struct DatabaseErrorContext {
    // Name of the table involved.
    std::optional<std::string> table_name;
    // Zero-based row number within that table.
    std::optional<std::uint64_t> row_number;
    // Zero-based column index within the table.
    std::optional<std::uint64_t> column_number;
    // Name of the column involved.
    std::optional<std::string> column_name;
};

// The single exception type for failures reading/writing a database. It derives from
// std::runtime_error and carries a stable code and as much location context as is known.
class DatabaseError : public std::runtime_error {
public:
    // `code` is the failure category; `message` is the human-readable text; `path` is the database
    // file; `page_number` and `context` (table/row/column) are optional location details.
    DatabaseError(DatabaseErrorCode code,
                  std::string message,
                  std::filesystem::path path = {},
                  std::optional<std::uint64_t> page_number = std::nullopt,
                  DatabaseErrorContext context = {});

    // The failure category.
    DatabaseErrorCode code() const noexcept;
    // Path of the database file, empty when unknown.
    const std::filesystem::path& path() const noexcept;
    // Page number involved, when the failure is page-scoped.
    std::optional<std::uint64_t> page_number() const noexcept;
    // Table/row/column context, with unset fields left empty.
    const DatabaseErrorContext& context() const noexcept;

private:
    DatabaseErrorCode code_;
    std::filesystem::path path_;
    std::optional<std::uint64_t> page_number_;
    DatabaseErrorContext context_;
};

// Format metadata of an opened database file.
struct FormatInfo {
    // Detected engine family.
    EngineVersion version;
    // Page size in bytes (the unit used for all page numbers).
    std::uint32_t page_size;
    // True when the handle was opened read-only.
    bool read_only;

    // Jet/ACE-style short name of the engine (e.g. "Jet3").
    const char* name() const noexcept;
};

class DatabaseFile {
public:
    // Opens the file read-only and identifies its engine version from the header.
    static DatabaseFile open(const std::filesystem::path& path);

    ~DatabaseFile();

    DatabaseFile(DatabaseFile&& other) noexcept;
    DatabaseFile& operator=(DatabaseFile&& other) noexcept;

    DatabaseFile(const DatabaseFile&) = delete;
    DatabaseFile& operator=(const DatabaseFile&) = delete;

    const std::filesystem::path& path() const noexcept;
    const FormatInfo& format() const noexcept;
    // For Jet4, inspects the database AccessVersion property in addition to the header.
    FileFormat file_format() const;
    // True when the header declares encoded pages (or the container is always encoded, MSISAM).
    // Opening a database with such pages needs DatabaseOptions::page_codec.
    bool is_encoded() const;
    std::uint64_t page_count() const noexcept;

    // Returns stored page bytes. Page zero is not unmasked and encoded pages are not decoded.
    std::vector<std::uint8_t> read_page(std::uint64_t page_number) const;

private:
    struct Impl;
    explicit DatabaseFile(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}

#include "jet4_database_creator.hpp"

#include "jet4_template_data.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace rea_jackcess::detail {
namespace {

std::filesystem::path absolute_target_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot resolve database output path: " + error.message(),
                            path);
    }
    absolute_path = absolute_path.lexically_normal();
    if (absolute_path.filename().empty()) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database output path must name a file",
                            path);
    }
    return absolute_path;
}

std::filesystem::path create_staging_directory(const std::filesystem::path& parent,
                                               const std::filesystem::path& target_path) {
    static std::atomic<std::uint64_t> next_id{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

    for (std::uint64_t attempt = 0; attempt < 128; ++attempt) {
        const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
        const auto directory = parent /
                               (".rea-jackcess-" + std::to_string(timestamp) + "-" + std::to_string(id));
        std::error_code error;
        if (std::filesystem::create_directory(directory, error)) {
            return directory;
        }
        if (error && error != std::errc::file_exists) {
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Cannot create a staging directory: " + error.message(),
                                target_path);
        }
    }

    throw DatabaseError(DatabaseErrorCode::io_error,
                        "Cannot reserve a unique database staging directory",
                        target_path);
}

void write_template(const std::filesystem::path& path,
                    const std::filesystem::path& target_path,
                    Jet4TemplateView source) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot create staged database file",
                            target_path);
    }

    output.write(reinterpret_cast<const char*>(source.data), static_cast<std::streamsize>(source.size));
    output.flush();
    if (!output) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot write staged database file",
                            target_path);
    }
    output.close();
    if (output.fail()) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot close staged database file",
                            target_path);
    }

    std::error_code error;
    const auto written_size = std::filesystem::file_size(path, error);
    if (error || written_size != source.size) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            error ? "Cannot verify staged database size: " + error.message()
                                  : "Staged database size does not match its template",
                            target_path);
    }
}

}

StagedDatabaseFile::StagedDatabaseFile(std::filesystem::path target_path,
                                       std::filesystem::path staging_directory,
                                       std::filesystem::path temporary_path)
    : target_path_(std::move(target_path)),
      staging_directory_(std::move(staging_directory)),
      temporary_path_(std::move(temporary_path)) {}

StagedDatabaseFile::~StagedDatabaseFile() {
    std::error_code ignored;
    std::filesystem::remove_all(staging_directory_, ignored);
}

const std::filesystem::path& StagedDatabaseFile::temporary_path() const noexcept {
    return temporary_path_;
}

void StagedDatabaseFile::publish() {
    if (published_) {
        throw std::logic_error("Staged database file has already been published");
    }

    std::error_code error;
    std::filesystem::rename(temporary_path_, target_path_, error);
    if (error) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot atomically publish database: " + error.message(),
                            target_path_);
    }
    published_ = true;
}

StagedDatabaseFile stage_jet4_database(const std::filesystem::path& target_path, DatabaseFormat format) {
    const auto source = jet4_template(format);
    if (source.data == nullptr || source.size == 0) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Only Jet4 V2000 and V2003 database creation is supported",
                            target_path);
    }

    const auto absolute_path = absolute_target_path(target_path);
    const auto staging_directory = create_staging_directory(absolute_path.parent_path(), absolute_path);
    const auto temporary_path = staging_directory / "staged.mdb";
    try {
        write_template(temporary_path, absolute_path, source);
        return StagedDatabaseFile(absolute_path, staging_directory, temporary_path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging_directory, ignored);
        throw;
    }
}

}

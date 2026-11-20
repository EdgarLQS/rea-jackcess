#include "page_writer.hpp"

#include "../format/jet_format.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t header_size = database_header_size;

[[noreturn]] void throw_unsupported_write_format(const std::filesystem::path& path) {
    throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                        "Page writing is currently supported only for Jet4 databases",
                        path);
}

}

PageFileWriter::PageFileWriter(std::filesystem::path path,
                               std::fstream stream,
                               FormatInfo format,
                               std::uint64_t page_count)
    : path_(std::move(path)),
      stream_(std::move(stream)),
      format_(format),
      page_count_(page_count) {}

PageFileWriter PageFileWriter::open(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const std::uint64_t file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot determine database file size: " + filesystem_error.message(),
                            path);
    }

    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        throw DatabaseError(DatabaseErrorCode::io_error, "Cannot open database file for writing", path);
    }
    if (file_size < header_size) {
        throw DatabaseError(DatabaseErrorCode::truncated_file,
                            "Database file is shorter than its header",
                            path);
    }

    std::array<std::uint8_t, database_header_size> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
        throw DatabaseError(DatabaseErrorCode::truncated_file, "Database header is incomplete", path);
    }

    const FormatInfo format = detect_format(header, path);
    if (format.version != EngineVersion::jet4) {
        throw_unsupported_write_format(path);
    }
    if (file_size < format.page_size || file_size % format.page_size != 0) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Database file size is not aligned to its page size",
                            path);
    }

    return PageFileWriter(path, std::move(stream), format, file_size / format.page_size);
}

const std::filesystem::path& PageFileWriter::path() const noexcept {
    return path_;
}

const FormatInfo& PageFileWriter::format() const noexcept {
    return format_;
}

std::uint64_t PageFileWriter::page_count() const {
    std::scoped_lock lock(mutex_);
    return page_count_;
}

void PageFileWriter::write_page(std::uint64_t page_number, const std::vector<std::uint8_t>& page) {
    std::scoped_lock lock(mutex_);
    ensure_usable();
    if (page.size() != format_.page_size) {
        throw std::invalid_argument("Page data size does not match the database page size");
    }
    if (page_number >= page_count_) {
        throw DatabaseError(DatabaseErrorCode::page_out_of_range,
                            "Database page number is out of range",
                            path_,
                            page_number);
    }

    const std::uint64_t offset = page_number * format_.page_size;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw DatabaseError(DatabaseErrorCode::page_out_of_range,
                            "Database page offset exceeds stream limits",
                            path_,
                            page_number);
    }

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        fail_io("Cannot seek to database page for writing", page_number);
    }
    stream_.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size()));
    if (!stream_) {
        fail_io("Cannot write database page", page_number);
    }
}

std::uint64_t PageFileWriter::append_page(const std::vector<std::uint8_t>& page) {
    std::scoped_lock lock(mutex_);
    ensure_usable();
    if (page.size() != format_.page_size) {
        throw std::invalid_argument("Page data size does not match the database page size");
    }

    const std::uint64_t page_number = page_count_;
    const std::uint64_t offset = page_number * format_.page_size;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw DatabaseError(DatabaseErrorCode::page_out_of_range,
                            "Database append offset exceeds stream limits",
                            path_,
                            page_number);
    }

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        fail_io("Cannot seek to database end for appending", page_number);
    }
    stream_.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size()));
    if (!stream_) {
        fail_io("Cannot append database page", page_number);
    }
    ++page_count_;
    return page_number;
}

void PageFileWriter::flush() {
    std::scoped_lock lock(mutex_);
    ensure_usable();
    stream_.flush();
    if (!stream_) {
        fail_io("Cannot flush database writes", page_count_ == 0 ? 0 : page_count_ - 1);
    }
}

void PageFileWriter::ensure_usable() const {
    if (unusable_) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Page writer stopped after a previous I/O failure",
                            path_);
    }
}

void PageFileWriter::fail_io(const char* message, std::uint64_t page_number) {
    unusable_ = true;
    throw DatabaseError(DatabaseErrorCode::io_error, message, path_, page_number);
}

}

#include "page_file.hpp"

#include "../format/jet_format.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <system_error>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t header_size = database_header_size;

}

PageFile::PageFile(std::filesystem::path path,
                   std::ifstream stream,
                   FormatInfo format,
                   std::uint64_t file_size)
    : path_(std::move(path)), stream_(std::move(stream)), format_(format), file_size_(file_size) {}

PageFile::PageFile(std::filesystem::path path,
                   std::vector<std::uint8_t> bytes,
                   FormatInfo format)
    : path_(std::move(path)),
      format_(format),
      file_size_(bytes.size()),
      memory_(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes))) {}

PageFile PageFile::open(const std::filesystem::path& path, std::optional<std::uint16_t> code_page) {
    std::error_code filesystem_error;
    const std::uint64_t file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot determine database file size: " + filesystem_error.message(), path);
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw DatabaseError(DatabaseErrorCode::io_error, "Cannot open database file", path);
    }
    if (file_size < header_size) {
        throw DatabaseError(DatabaseErrorCode::truncated_file, "Database file is shorter than its header", path);
    }

    std::array<std::uint8_t, database_header_size> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
        throw DatabaseError(DatabaseErrorCode::truncated_file, "Database header is incomplete", path);
    }

    const FormatInfo format = detect_format(header, path);
    if (file_size < format.page_size || file_size % format.page_size != 0) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Database file size is not aligned to its page size", path);
    }

    PageFile result(path, std::move(stream), format, file_size);
    result.code_page_override_ = code_page;
    return result;
}

PageFile PageFile::open(std::vector<std::uint8_t> bytes,
                        std::string memory_name,
                        std::optional<std::uint16_t> code_page) {
    const std::filesystem::path name(std::move(memory_name));
    if (bytes.size() < header_size) {
        throw DatabaseError(DatabaseErrorCode::truncated_file,
                            "In-memory database is shorter than its header",
                            name);
    }

    std::array<std::uint8_t, database_header_size> header{};
    std::copy_n(bytes.begin(), header.size(), header.begin());
    const FormatInfo format = detect_format(header, name);
    if (bytes.size() < format.page_size || bytes.size() % format.page_size != 0) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "In-memory database size is not aligned to its page size",
                            name);
    }
    PageFile result(name, std::move(bytes), format);
    result.code_page_override_ = code_page;
    return result;
}

PageFile::PageFile(PageFile&& other)
    : path_(std::move(other.path_)),
      stream_(std::move(other.stream_)),
      format_(other.format_),
      file_size_(other.file_size_),
      memory_(std::move(other.memory_)),
      code_page_override_(other.code_page_override_) {}

PageFile& PageFile::operator=(PageFile&& other) {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(stream_mutex_, other.stream_mutex_);
    path_ = std::move(other.path_);
    stream_ = std::move(other.stream_);
    format_ = other.format_;
    file_size_ = other.file_size_;
    memory_ = std::move(other.memory_);
    code_page_override_ = other.code_page_override_;
    return *this;
}

const std::filesystem::path& PageFile::path() const noexcept {
    return path_;
}

const FormatInfo& PageFile::format() const noexcept {
    return format_;
}

std::uint64_t PageFile::page_count() const noexcept {
    return file_size_ / format_.page_size;
}

std::vector<std::uint8_t> PageFile::read_page(std::uint64_t page_number) const {
    if (page_number >= page_count()) {
        throw DatabaseError(DatabaseErrorCode::page_out_of_range,
                            "Database page number is out of range", path_, page_number);
    }

    const std::uint64_t offset = page_number * format_.page_size;
    if (memory_) {
        const auto begin = memory_->begin() + static_cast<std::ptrdiff_t>(offset);
        return {begin, begin + static_cast<std::ptrdiff_t>(format_.page_size)};
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw DatabaseError(DatabaseErrorCode::page_out_of_range,
                            "Database page offset exceeds stream limits", path_, page_number);
    }

    std::vector<std::uint8_t> page(format_.page_size);
    std::scoped_lock lock(stream_mutex_);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        throw DatabaseError(DatabaseErrorCode::io_error, "Cannot seek to database page", path_, page_number);
    }
    stream_.read(reinterpret_cast<char*>(page.data()), static_cast<std::streamsize>(page.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(page.size())) {
        throw DatabaseError(DatabaseErrorCode::truncated_file, "Database page is incomplete", path_, page_number);
    }
    return page;
}

}

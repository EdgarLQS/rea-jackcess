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

PageWriteUndo::PageWriteUndo(std::filesystem::path path, std::uint64_t page_count,
                             std::uint32_t page_size)
    : path_(std::move(path)), original_page_count_(page_count), page_size_(page_size) {}

bool PageWriteUndo::dirty() const noexcept {
    return appended_ || !originals_.empty();
}

void PageWriteUndo::rollback() {
    if (!dirty()) {
        return;
    }
    if (!originals_.empty()) {
        std::fstream restore(path_, std::ios::binary | std::ios::in | std::ios::out);
        for (const auto& [page, bytes] : originals_) {
            restore.seekp(static_cast<std::streamoff>(page * page_size_));
            restore.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            if (!restore) {
                throw DatabaseError(DatabaseErrorCode::io_error, "Cannot restore original database page",
                                    path_, page);
            }
        }
        restore.close();
        if (!restore) {
            throw DatabaseError(DatabaseErrorCode::io_error, "Cannot flush restored database pages", path_);
        }
    }
    if (appended_) {
        std::error_code error;
        std::filesystem::resize_file(path_, original_page_count_ * page_size_, error);
        if (error) {
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Cannot remove appended database pages: " + error.message(), path_);
        }
    }
    originals_.clear();
    appended_ = false;
    if (journal_) {
        // The in-process rollback already restored the pages, so the replay record goes too.
        journal_->commit();
        journal_.reset();
    }
}

PageFileWriter::PageFileWriter(std::filesystem::path path,
                               std::fstream stream,
                               FormatInfo format,
                               std::uint64_t page_count,
                               PageWriteUndo* undo)
    : path_(std::move(path)),
      stream_(std::move(stream)),
      format_(format),
      page_count_(page_count),
      undo_(undo) {}

PageFileWriter PageFileWriter::open(const std::filesystem::path& path, PageWriteUndo* undo) {
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

    const auto page_count = file_size / format.page_size;
    if (undo && (undo->path_ != path || undo->page_size_ != format.page_size ||
                 undo->original_page_count_ > page_count)) {
        throw std::invalid_argument("Page undo does not match the database");
    }
    return PageFileWriter(path, std::move(stream), format, page_count, undo);
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

    if (undo_ && page_number < undo_->original_page_count_ &&
        undo_->originals_.count(page_number) == 0) {
        std::vector<std::uint8_t> original(format_.page_size);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream_.read(reinterpret_cast<char*>(original.data()),
                     static_cast<std::streamsize>(original.size()));
        if (!stream_) {
            fail_io("Cannot read original database page before writing", page_number);
        }
        const auto [position, inserted] = undo_->originals_.emplace(page_number, std::move(original));
        if (inserted) {
            // Journal the pre-image before the page is overwritten so a crash mid-batch can be
            // replayed by the next open. Only the first image of a page is kept.
            if (!undo_->journal_) {
                undo_->journal_ = std::make_unique<WriteJournal>(path_, undo_->original_page_count_,
                                                                 format_.page_size);
            }
            undo_->journal_->record_page(page_number, position->second);
        }
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
    if (undo_) {
        if (!undo_->journal_) {
            undo_->journal_ = std::make_unique<WriteJournal>(path_, undo_->original_page_count_,
                                                             format_.page_size);
        }
        undo_->journal_->record_append();
        undo_->appended_ = true;
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
    if (undo_ && undo_->journal_) {
        // The batch reached the database, so replaying it is no longer needed.
        undo_->journal_->commit();
        undo_->journal_.reset();
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

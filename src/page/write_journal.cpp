#include "write_journal.hpp"

#include "rea_jackcess/database_file.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace rea_jackcess::detail {
namespace {

constexpr char journal_magic[8] = {'R', 'J', 'R', 'N', 'L', '0', '0', '1'};
constexpr std::uint8_t record_kind_page = 0;
constexpr std::uint8_t record_kind_append = 1;
constexpr std::size_t header_size = 8 + 4 + 8;
constexpr std::size_t record_header_size = 1 + 8;

[[noreturn]] void journal_error(const std::filesystem::path& path, const char* message) {
    throw DatabaseError(DatabaseErrorCode::io_error,
                        std::string(message) + ": " + std::strerror(errno),
                        path);
}

void write_all(int fd, const std::filesystem::path& path, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < size) {
        const auto result = ::write(fd, bytes + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            journal_error(path, "Cannot append to the write journal");
        }
        written += static_cast<std::size_t>(result);
    }
}

void read_exact(int fd, const std::filesystem::path& path, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t read_bytes = 0;
    while (read_bytes < size) {
        const auto result = ::read(fd, bytes + read_bytes, size - read_bytes);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            journal_error(path, "Cannot read the write journal");
        }
        if (result == 0) {
            throw DatabaseError(DatabaseErrorCode::truncated_file, "Write journal ends unexpectedly", path);
        }
        read_bytes += static_cast<std::size_t>(result);
    }
}

// Reads as many bytes as the record promises, or reports that the tail is incomplete.
bool read_available(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t read_bytes = 0;
    while (read_bytes < size) {
        const auto result = ::read(fd, bytes + read_bytes, size - read_bytes);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        read_bytes += static_cast<std::size_t>(result);
    }
    return true;
}

std::uint64_t read_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

void write_u64(std::uint8_t* bytes, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
    }
}

void write_u32(std::uint8_t* bytes, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
    }
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

}  // namespace

std::filesystem::path WriteJournal::path_for(const std::filesystem::path& database) {
    auto journal = database;
    journal += ".reajournal";
    return journal;
}

bool WriteJournal::pending(const std::filesystem::path& database) {
    std::error_code ignored;
    return std::filesystem::exists(path_for(database), ignored);
}

void WriteJournal::recover(const std::filesystem::path& database) {
    const auto path = path_for(database);
    const int journal = ::open(path.c_str(), O_RDONLY);
    if (journal < 0) {
        if (errno == ENOENT) {
            return;
        }
        journal_error(path, "Cannot open the write journal for recovery");
    }
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } journal_closer{journal};

    std::uint8_t header[header_size];
    if (!read_available(journal, header, sizeof(header)) ||
        std::memcmp(header, journal_magic, sizeof(journal_magic)) != 0) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Write journal header is missing or invalid",
                            path);
    }
    const std::uint32_t page_size = read_u32(header + 8);
    const std::uint64_t original_page_count = read_u64(header + 12);
    if (page_size < 512 || page_size > (1u << 20)) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Write journal declares an invalid page size",
                            path);
    }

    std::map<std::uint64_t, std::vector<std::uint8_t>> originals;
    while (true) {
        std::uint8_t record[record_header_size];
        if (!read_available(journal, record, sizeof(record))) {
            break;  // A partial tail describes pages that were never written.
        }
        const std::uint8_t kind = record[0];
        const std::uint64_t page_number = read_u64(record + 1);
        if (kind == record_kind_append) {
            continue;
        }
        if (kind != record_kind_page) {
            throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                "Write journal contains an unknown record",
                                path);
        }
        std::vector<std::uint8_t> bytes(page_size);
        if (!read_available(journal, bytes.data(), bytes.size())) {
            break;
        }
        originals[page_number] = std::move(bytes);
    }
    ::close(journal);
    journal_closer.fd = -1;

    const int database_fd = ::open(database.c_str(), O_RDWR);
    if (database_fd < 0) {
        journal_error(database, "Cannot open the database for journal recovery");
    }
    struct DbCloser {
        int fd;
        ~DbCloser() { ::close(fd); }
    } database_closer{database_fd};

    for (const auto& [page_number, bytes] : originals) {
        const off_t offset = static_cast<off_t>(page_number * page_size);
        if (::pwrite(database_fd, bytes.data(), bytes.size(), offset) !=
            static_cast<ssize_t>(bytes.size())) {
            journal_error(database, "Cannot restore a page from the write journal");
        }
    }
    // Pages appended by the interrupted batch are cut away; for a file that never grew this is
    // a no-op.
    if (::ftruncate(database_fd, static_cast<off_t>(original_page_count * page_size)) != 0) {
        journal_error(database, "Cannot truncate appended pages during journal recovery");
    }
    if (::fsync(database_fd) != 0) {
        journal_error(database, "Cannot flush recovered database pages");
    }
    ::close(database_closer.fd);
    database_closer.fd = -1;

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

WriteJournal::WriteJournal(const std::filesystem::path& database,
                           std::uint64_t page_count,
                           std::uint32_t page_size)
    : path_(path_for(database)), page_size_(page_size) {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_ < 0) {
        journal_error(path_, "Cannot create the write journal");
    }
    std::uint8_t header[header_size];
    std::memcpy(header, journal_magic, sizeof(journal_magic));
    write_u32(header + 8, page_size);
    write_u64(header + 12, page_count);
    write_all(fd_, path_, header, sizeof(header));
    if (::fsync(fd_) != 0) {
        journal_error(path_, "Cannot flush the write journal header");
    }
}

WriteJournal::~WriteJournal() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void WriteJournal::record_page(std::uint64_t page_number, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != page_size_) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Write journal received a page of the wrong size",
                            path_,
                            page_number);
    }
    std::uint8_t record[record_header_size];
    record[0] = record_kind_page;
    write_u64(record + 1, page_number);
    write_all(fd_, path_, record, sizeof(record));
    write_all(fd_, path_, bytes.data(), bytes.size());
    // The image has to reach the disk before the database page it protects is overwritten.
    if (::fsync(fd_) != 0) {
        journal_error(path_, "Cannot flush a page image to the write journal");
    }
}

void WriteJournal::record_append() {
    std::uint8_t record[record_header_size];
    record[0] = record_kind_append;
    write_u64(record + 1, 0);
    write_all(fd_, path_, record, sizeof(record));
    if (::fsync(fd_) != 0) {
        journal_error(path_, "Cannot flush the append marker to the write journal");
    }
}

void WriteJournal::commit() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
}

}

#pragma once

#include "rea_jackcess/database_file.hpp"
#include "rea_jackcess/page_codec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace rea_jackcess::detail {

// Open options for a PageFile.
struct PageFileOptions {
    // Forced legacy (Jet3) text code page; nullopt uses the format default.
    std::optional<std::uint16_t> code_page;
    // Factory that supplies a page codec for an encoded database; empty means no decoding.
    PageCodecFactory codec;
};

class PageFile {
public:
    static PageFile open(const std::filesystem::path& path, PageFileOptions options = {});

    // Opens a database that already lives in memory. The buffer must hold the complete file;
    // errors report `memory_name` instead of a path.
    static PageFile open(std::vector<std::uint8_t> bytes,
                         std::string memory_name = "<memory>",
                         PageFileOptions options = {});

    PageFile(PageFile&& other);
    PageFile& operator=(PageFile&& other);

    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;

    const std::filesystem::path& path() const noexcept;
    const FormatInfo& format() const noexcept;

    // Set when the caller forced a legacy code page; Jet3 text decoding uses it.
    std::optional<std::uint16_t> code_page_override() const noexcept { return code_page_override_; }
    // True when the header declares encoded pages, or the container is always encoded (MSISAM).
    bool encoded() const noexcept { return encoded_; }
    // True when a caller-supplied codec decodes pages 1 and above for this handle.
    bool has_codec() const noexcept { return static_cast<bool>(codec_); }
    // Rejects an encoded file that has no codec. This file type stays a raw accessor, so the
    // decision to demand a codec belongs to the handle that reads catalog and row pages.
    void require_decodable() const;
    std::uint64_t page_count() const noexcept;
    // Returns the stored bytes of page zero and decoded bytes of every other page.
    std::vector<std::uint8_t> read_page(std::uint64_t page_number) const;

private:
    PageFile(std::filesystem::path path,
             std::ifstream stream,
             FormatInfo format,
             std::uint64_t file_size);

    PageFile(std::filesystem::path path,
             std::vector<std::uint8_t> bytes,
             FormatInfo format);

    std::vector<std::uint8_t> read_raw_page(std::uint64_t page_number) const;
    // Identifies encoding and installs the caller's codec. Runs once, during open.
    void install_codec(PageFileOptions options);

    std::filesystem::path path_;
    mutable std::ifstream stream_;
    FormatInfo format_;
    std::uint64_t file_size_;
    mutable std::mutex stream_mutex_;
    std::optional<std::uint16_t> code_page_override_;
    bool encoded_ = false;
    // The factory ran but returned nullptr; only an error message distinguishes it from no factory.
    bool codec_declined_ = false;
    std::shared_ptr<PageCodec> codec_;
    // Set only for memory-backed files; read_page then serves slices of this buffer.
    std::shared_ptr<const std::vector<std::uint8_t>> memory_;
};

}

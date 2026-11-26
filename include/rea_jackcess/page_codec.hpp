#pragma once

#include "database_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace rea_jackcess {

// Decodes the stored bytes of one database page for a read-only handle. Access can encode every
// page except page zero, so the header stays readable before a key is derived; the codec is only
// asked for pages 1 and above.
//
// `page` always points at exactly one page (FormatInfo::page_size bytes) and is decoded in place,
// so a codec may neither change the page size nor retain the pointer. Throwing aborts the read: a
// DatabaseError keeps its own code, path and page number, and any other exception is reported as
// corrupted_file with the file and page that could not be decoded.
class PageCodec {
public:
    virtual ~PageCodec() = default;

    // `page_number` is never zero and always refers to an existing page of the file.
    virtual void decode_page(std::uint8_t* page, std::size_t size, std::uint64_t page_number) = 0;
};

// Handed to a PageCodecFactory while a database is being opened. The referenced objects and the
// reader live only for the duration of that call.
struct PageCodecContext {
    const FormatInfo& format;
    const std::filesystem::path& path;
    // Stored bytes of a page with no decoding applied, including page zero. Intended for reading
    // the header and page 1 to derive a key; it never re-enters the codec.
    std::function<std::vector<std::uint8_t>(std::uint64_t)> read_raw_page;
    // Whether the file states that its pages are encoded (a non-zero encoding key in the header, or
    // an MSISAM container, which is always encoded).
    bool encoded;
};

// Creates the codec for one open call. Returning nullptr means "this database needs no decoding",
// which is only accepted when the file does not declare encoding; an encoded file then fails with
// unsupported_operation instead of returning scrambled pages. The factory runs exactly once per
// open, before any catalog page is read, and the result is retained by the handle until it is
// destroyed.
using PageCodecFactory = std::function<std::shared_ptr<PageCodec>(const PageCodecContext&)>;

}

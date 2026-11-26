#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess {

// A read-only, seekable view over one OLE column value. The payload is materialised when the
// stream is created, so the stream stays valid after the row cursor moves on.
class OleStream {
public:
    std::size_t size() const noexcept { return bytes_.size(); }
    std::size_t position() const noexcept { return position_; }
    bool empty() const noexcept { return bytes_.empty(); }
    const std::string& column_name() const noexcept { return column_name_; }

    // Moves the read position. Positions past the end are rejected with std::out_of_range.
    void seek(std::size_t position);

    // Copies up to `count` bytes and advances the position. Returns the bytes copied, which is
    // smaller than `count` only at the end of the stream.
    std::size_t read(std::uint8_t* destination, std::size_t count);

    std::vector<std::uint8_t> read_all() const { return bytes_; }

    // Returns the bytes from `offset`, truncated at the end of the stream.
    std::vector<std::uint8_t> read_range(std::size_t offset, std::size_t count) const;

private:
    OleStream(std::vector<std::uint8_t> bytes, std::string column_name);

    std::vector<std::uint8_t> bytes_;
    std::string column_name_;
    std::size_t position_ = 0;

    friend class Table;
};

}  // namespace rea_jackcess

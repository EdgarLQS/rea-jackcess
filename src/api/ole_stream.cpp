#include "rea_jackcess/ole_stream.hpp"

#include <algorithm>
#include <stdexcept>

namespace rea_jackcess {

OleStream::OleStream(std::vector<std::uint8_t> bytes, std::string column_name)
    : bytes_(std::move(bytes)), column_name_(std::move(column_name)) {}

void OleStream::seek(std::size_t position) {
    if (position > bytes_.size()) {
        throw std::out_of_range("OLE stream position is past the end of the value");
    }
    position_ = position;
}

std::size_t OleStream::read(std::uint8_t* destination, std::size_t count) {
    if (destination == nullptr && count != 0) {
        throw std::invalid_argument("OLE stream read needs a destination buffer");
    }
    const std::size_t available = bytes_.size() - position_;
    const std::size_t copied = std::min(count, available);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), copied, destination);
    position_ += copied;
    return copied;
}

std::vector<std::uint8_t> OleStream::read_range(std::size_t offset, std::size_t count) const {
    if (offset >= bytes_.size()) {
        return {};
    }
    const std::size_t available = bytes_.size() - offset;
    const std::size_t copied = std::min(count, available);
    return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset + copied)};
}

}  // namespace rea_jackcess

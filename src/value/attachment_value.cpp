#include "attachment_value.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <zlib.h>

namespace rea_jackcess::detail {
namespace {

constexpr std::size_t wrapper_header_size = 8;

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::vector<std::uint8_t> inflate_payload(const std::vector<std::uint8_t>& encoded_data,
                                          std::size_t data_length) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        throw std::runtime_error("Could not initialize attachment decompression");
    }

    struct InflateGuard {
        z_stream& stream;
        ~InflateGuard() { inflateEnd(&stream); }
    } guard{stream};

    std::vector<std::uint8_t> result;
    result.reserve(std::min<std::size_t>(data_length, 64 * 1024));
    std::array<std::uint8_t, 16 * 1024> output{};
    std::size_t input_offset = wrapper_header_size;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0 && input_offset < encoded_data.size()) {
            const auto remaining = encoded_data.size() - input_offset;
            const auto chunk_size = std::min<std::size_t>(remaining,
                                                          std::numeric_limits<uInt>::max());
            stream.next_in = const_cast<Bytef *>(encoded_data.data() + input_offset);
            stream.avail_in = static_cast<uInt>(chunk_size);
            input_offset += chunk_size;
        }

        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());
        const auto input_before = stream.avail_in;
        status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = output.size() - stream.avail_out;
        if (produced > data_length - std::min(data_length, result.size())) {
            throw std::invalid_argument("Decoded attachment exceeds its declared size");
        }
        result.insert(result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(produced));

        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::invalid_argument("Attachment payload is not a valid deflate stream");
        }
        if (status == Z_OK && produced == 0 && stream.avail_in == input_before) {
            throw std::invalid_argument("Attachment deflate stream could not make progress");
        }
        if (status == Z_OK && stream.avail_in == 0 && input_offset == encoded_data.size() && produced == 0) {
            throw std::invalid_argument("Attachment deflate stream is truncated");
        }
    }
    if (result.size() != data_length) {
        throw std::invalid_argument("Decoded attachment size does not match its wrapper header");
    }
    return result;
}

}  // namespace

std::vector<std::uint8_t> decode_attachment_file_data(const std::vector<std::uint8_t>& encoded_data) {
    if (encoded_data.size() < wrapper_header_size) {
        throw std::invalid_argument("Attachment wrapper header is truncated");
    }

    const auto type_flag = read_u32(encoded_data, 0);
    const auto declared_length = read_u32(encoded_data, 4);
    if (type_flag > 1) {
        throw std::invalid_argument("Attachment wrapper uses an unknown payload type");
    }
    if (declared_length > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument("Attachment wrapper declares an unsupported payload size");
    }

    const auto data_length = static_cast<std::size_t>(declared_length);
    std::vector<std::uint8_t> decoded;
    if (type_flag == 0) {
        const auto available = encoded_data.size() - wrapper_header_size;
        if (available < data_length) {
            throw std::invalid_argument("Raw attachment payload is truncated");
        }
        decoded.assign(encoded_data.begin() + static_cast<std::ptrdiff_t>(wrapper_header_size),
                       encoded_data.begin() + static_cast<std::ptrdiff_t>(wrapper_header_size + data_length));
    } else {
        decoded = inflate_payload(encoded_data, data_length);
    }

    if (decoded.size() < 4) {
        throw std::invalid_argument("Attachment content header is truncated");
    }
    const auto content_header_length = static_cast<std::size_t>(read_u32(decoded, 0));
    if (content_header_length < 4 || content_header_length > decoded.size()) {
        throw std::invalid_argument("Attachment content header has an invalid size");
    }
    return {decoded.begin() + static_cast<std::ptrdiff_t>(content_header_length), decoded.end()};
}

}

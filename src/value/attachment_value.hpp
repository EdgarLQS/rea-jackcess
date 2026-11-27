#pragma once

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

// Unwraps the OLE payload stored for an ACE attachment into the raw file bytes. Throws
// std::invalid_argument when the wrapper or its deflate stream is malformed.
std::vector<std::uint8_t> decode_attachment_file_data(const std::vector<std::uint8_t>& encoded_data);

}

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

// Encodes non-NULL Jet4 text keys, including their start flags.
std::vector<std::uint8_t> encode_jet4_general_legacy_key(std::u16string_view text,
                                                        bool descending);
std::vector<std::uint8_t> encode_jet4_general_key(std::u16string_view text,
                                                  bool descending);
std::vector<std::uint8_t> encode_jet4_general97_key(std::u16string_view text,
                                                    bool descending);

}

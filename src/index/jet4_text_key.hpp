#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

// Encodes non-NULL Jet4 text keys, including their start flags. `text` is the key in UTF-16; the
// three functions differ only in collation (General Legacy, General, General 97). `descending`
// selects the byte order Access uses for a descending key.
std::vector<std::uint8_t> encode_jet4_general_legacy_key(std::u16string_view text,
                                                        bool descending);
std::vector<std::uint8_t> encode_jet4_general_key(std::u16string_view text,
                                                  bool descending);
std::vector<std::uint8_t> encode_jet4_general97_key(std::u16string_view text,
                                                    bool descending);

}

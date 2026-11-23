#pragma once

#include "row.hpp"
#include "value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rea_jackcess {

enum class ComplexColumnType {
    multi_value,
    attachment,
    version_history,
    unsupported,
};

struct MultiValueItem {
    Value value;
};

struct AttachmentValue {
    std::optional<std::string> file_url;
    std::optional<std::string> file_name;
    std::optional<std::string> file_type;
    Value file_timestamp;
    std::optional<std::int32_t> file_flags;
    std::optional<std::vector<std::uint8_t>> encoded_file_data;
    std::optional<std::vector<std::uint8_t>> file_data;
};

struct VersionHistoryValue {
    std::optional<std::string> value;
    Value modified_at;
};

struct UnsupportedComplexValue {};

using ComplexValuePayload = std::variant<MultiValueItem,
                                         AttachmentValue,
                                         VersionHistoryValue,
                                         UnsupportedComplexValue>;

// Read-only typed view of one row in an ACE complex column's flat table.
// `raw_value` retains the decoded flat-table row and the encoded bytes of unsupported fields.
struct ComplexValueInfo {
    std::int32_t id;
    std::int32_t complex_value_foreign_key;
    Row raw_value;
    ComplexValuePayload value;
};

}

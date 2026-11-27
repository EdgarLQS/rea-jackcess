#pragma once

#include "row.hpp"
#include "value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rea_jackcess {

// The kind of ACE complex column, classified from its type-table row.
enum class ComplexColumnType {
    multi_value,     // ACE "Multi-valued field": a list of scalar values.
    attachment,      // ACE "Attachment": one or more attached files per cell.
    version_history, // ACE "Version history": the change log of a cell.
    unsupported,     // A complex column whose type table could not be classified.
};

// One item of a multi-valued cell.
struct MultiValueItem {
    // The scalar value of a single list entry.
    Value value;
};

// One attached file inside an attachment cell.
struct AttachmentValue {
    // Stored source URL / path, if recorded.
    std::optional<std::string> file_url;
    // Original file name (e.g. "report.pdf").
    std::optional<std::string> file_name;
    // File type category text (e.g. "pdf").
    std::optional<std::string> file_type;
    // Stored modified timestamp of the attachment (usually a DATE value).
    Value file_timestamp;
    // Stored attachment flags word, if present.
    std::optional<std::int32_t> file_flags;
    // The OLE wrapper bytes exactly as stored in the flat table.
    std::optional<std::vector<std::uint8_t>> encoded_file_data;
    // The file payload the OLE wrapper carries, when it could be unwrapped.
    std::optional<std::vector<std::uint8_t>> file_data;
};

// One entry of a version-history cell.
struct VersionHistoryValue {
    // The historical value text at this revision.
    std::optional<std::string> value;
    // When this revision was recorded (usually a DATE value).
    Value modified_at;
};

// Placeholder for a complex type this library recognises as complex but cannot interpret.
struct UnsupportedComplexValue {};

// Typed payload of one complex cell; the active alternative matches the column's ComplexColumnType.
using ComplexValuePayload = std::variant<MultiValueItem,
                                         AttachmentValue,
                                         VersionHistoryValue,
                                         UnsupportedComplexValue>;

// Read-only typed view of one row in an ACE complex column's flat table.
// `raw_value` retains the decoded flat-table row and the encoded bytes of unsupported fields.
struct ComplexValueInfo {
    // The flat-table row's AutoNumber `Id`.
    std::int32_t id;
    // The flat-table complex foreign-key value that ties this row to its parent row's cell.
    std::int32_t complex_value_foreign_key;
    // The complete decoded flat-table row (all columns, including this complex value's fields).
    Row raw_value;
    // The typed payload parsed from `raw_value` according to the column's complex type.
    ComplexValuePayload value;
};

}

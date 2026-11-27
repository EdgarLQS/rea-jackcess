#pragma once

#include "value.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess {

// One property (name/value pair) inside a property block.
struct PropertyInfo {
    // Property name.
    std::string name;
    // Jet column type of the value (e.g. TEXT, LONG, BOOLEAN); drives how raw_value/value decode.
    std::uint8_t type_code;
    // Complete stored flag byte (DDL/required flags and other property attributes) as read.
    std::uint8_t flags;
    // The property value exactly as stored (row-data bytes), retained for a lossless save.
    std::vector<std::uint8_t> raw_value;
    // Bytes that follow the value record in the block, preserved verbatim on save.
    std::vector<std::uint8_t> trailing_bytes;
    // Decoded typed view of raw_value according to type_code.
    Value value;
    // Null properties have a type and flags in memory but are omitted from the value block on save.
    bool value_is_null = false;

    // Encodes a value using this property's existing Jet type and preserves its flags.
    // An incompatible or unsupported value raises std::invalid_argument.
    void set_value(Value new_value);
};

// A named block of properties (a database property block, or a table's/column's/ index's block).
struct PropertyMapInfo {
    // Block name (empty for the default database block; column/index name for named blocks).
    std::string name;
    // Block type word as stored in the block header.
    std::uint16_t type_code;
    // The properties contained in the block, in their stored order.
    std::vector<PropertyInfo> properties;

    // Property names are compared without ASCII case distinctions.
    PropertyInfo* find(std::string_view property_name);
    const PropertyInfo* find(std::string_view property_name) const;
    // Returns the stored value, or `default_value` when the property is absent or null.
    Value get_value(std::string_view property_name, Value default_value = {}) const;
    PropertyInfo& put(PropertyInfo property);
    // Infers the Jet type and built-in DDL flag using the fixed Jackcess 5.2.0 property rules.
    PropertyInfo& put(std::string name, Value value);
    // Writes an explicitly typed value; built-in properties keep their required DDL flag.
    PropertyInfo& put(std::string name,
                      std::uint8_t type_code,
                      Value value,
                      bool is_ddl = false);
    // Copies properties while preserving the complete flag byte and any raw metadata.
    void put_all(const std::vector<PropertyInfo>& properties);
    bool remove(std::string_view property_name);
};

}

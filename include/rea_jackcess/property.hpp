#pragma once

#include "value.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess {

struct PropertyInfo {
    std::string name;
    std::uint8_t type_code;
    std::uint8_t flags;
    std::vector<std::uint8_t> raw_value;
    std::vector<std::uint8_t> trailing_bytes;
    Value value;
    // Null properties have a type and flags in memory but are omitted from the value block on save.
    bool value_is_null = false;

    // Encodes a value using this property's existing Jet type and preserves its flags.
    // An incompatible or unsupported value raises std::invalid_argument.
    void set_value(Value new_value);
};

struct PropertyMapInfo {
    std::string name;
    std::uint16_t type_code;
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

#pragma once

#include "rea_jackcess/database.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::pgeo {

struct SpatialReferenceInfo {
    std::int32_t id;
    // Preserved as stored in GDB_SpatialRefs.SRTEXT; it may be WKT, a GUID, or another form.
    std::optional<std::string> definition;
};

struct LayerInfo {
    std::string name;
    std::string table_name;
    std::string object_id_column;
    std::string shape_column;
    std::int32_t shape_type;
    bool has_z;
    bool has_m;
    std::optional<std::int32_t> spatial_reference_id;
    std::optional<SpatialReferenceInfo> spatial_reference;
    std::vector<ColumnInfo> attributes;
};

struct Attribute {
    std::string name;
    Value value;
};

struct Feature {
    std::int32_t object_id;
    std::vector<Attribute> attributes;
    // nullopt is a NULL SHAPE; an engaged empty vector is a zero-length SHAPE value.
    std::optional<std::vector<std::uint8_t>> shape;
};

class LayerReader {
public:
    LayerReader(LayerReader&&) noexcept;
    LayerReader& operator=(LayerReader&&) noexcept;
    ~LayerReader();

    LayerReader(const LayerReader&) = delete;
    LayerReader& operator=(const LayerReader&) = delete;

    std::optional<Feature> next();

private:
    struct Impl;
    explicit LayerReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Dataset;
};

class Dataset {
public:
    static Dataset open(const std::filesystem::path& path);

    const std::vector<LayerInfo>& layers() const noexcept;
    LayerReader open_layer(std::string_view name) const;

private:
    Dataset(Database database, std::vector<LayerInfo> layers);

    Database database_;
    std::vector<LayerInfo> layers_;
};

}
